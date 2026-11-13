// Step OBJB: the proximity union's hold section and its reference roots
// (session/proximity_hold.hpp; docs/realtime-design.md 'Built (a replay
// restores the body-less references the proximity union reads, step OBJB)').

#include <session/proximity_hold.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bmmo::session {

    namespace {
        constexpr size_t kPose = 16;   // floats

        uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t get32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
                 | (static_cast<uint32_t>(p[3]) << 24);
        }
        void put16(std::vector<uint8_t>& out, size_t v) {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        void put32(std::vector<uint8_t>& out, uint32_t v) {
            for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(v >> s));
        }
    }

    bool parse_hold(const uint8_t* data, size_t size, hold_layout& h) {
        h = hold_layout{};
        size_t at = 0;
        const auto count = [&](size_t& v, size_t each) {
            if (at + 2 > size) return false;
            v = get16(data + at);
            at += 2;
            if (at + v * each > size) return false;
            at += v * each;
            return true;
        };
        if (!count(h.frames, 12)) return false;
        h.names_at = at;
        if (!count(h.names, 4)) return false;
        h.roots_at = at;
        if (at + 3 > size) return false;
        h.roots = get16(data + at);
        h.flags = data[at + 2];
        at += 3;
        if (at + h.roots * 4 > size) return false;
        at += h.roots * 4;
        if (h.flags & kHoldReferencePositions) {
            h.references_at = at;
            if (!count(h.references, 12)) return false;
        }
        h.end = at;
        return true;
    }

    hold_difference compare_hold(const uint8_t* a, size_t na, const uint8_t* b, size_t nb, std::string& what) {
        what.clear();
        hold_layout x, y;
        if (!parse_hold(a, na, x) || !parse_hold(b, nb, y) || x.frames != y.frames || x.names != y.names || x.roots != y.roots) {
            what = "proximity hold: the counts differ";
            return hold_difference::logical;
        }
        hold_difference d = hold_difference::none;
        for (size_t f = 0; f < x.frames; ++f)
            if (std::memcmp(a + 2 + f * 12, b + 2 + f * 12, 12) != 0) {
                what = "proximity hold: frame " + std::to_string(f) + " differs [float]";
                d = hold_difference::float_only;
                break;
            }
        for (size_t n = 0; n < x.names; ++n)
            if (std::memcmp(a + x.names_at + 2 + n * 4, b + y.names_at + 2 + n * 4, 4) != 0) {
                what = "proximity hold: by-name copy " + std::to_string(n) + " differs";
                return hold_difference::logical;
            }
        for (size_t r = 0; r < x.roots; ++r)
            if (std::memcmp(a + x.roots_at + 3 + r * 4, b + y.roots_at + 3 + r * 4, 4) != 0) {
                what = "proximity hold: reference root " + std::to_string(r) + " generation differs";
                return hold_difference::logical;
            }
        return d;
    }

    int reference_tracker::mode() {
        static const int mode = [] {
            const char* text = std::getenv("BMMO_REFERENCE_POSES");
            if (text && (std::strcmp(text, "off") == 0 || std::strcmp(text, "0") == 0)) return 0;
            return text && std::strcmp(text, "verify") == 0 ? 2 : 1;
        }();
        return mode;
    }

    reference_tracker::reference_tracker() = default;
    reference_tracker::~reference_tracker() = default;
    reference_tracker::reference_tracker(reference_tracker&&) noexcept = default;
    reference_tracker& reference_tracker::operator=(reference_tracker&&) noexcept = default;

    void reference_tracker::clear() {
        roots_.clear();
        objects_.clear();
        carried_.clear();
    }

    void reference_tracker::add(uint32_t object, uint32_t root, const float* root_pose) {
        if (mode() == 0) return;
        objects_.push_back(object);
        if (!root || !root_pose) return;
        if (std::any_of(roots_.begin(), roots_.end(), [root](const root_state& s) { return s.id == root; })) return;
        root_state s;
        s.id = root;
        s.poses.assign(root_pose, root_pose + kPose);
        s.generations.push_back(0);
        roots_.push_back(std::move(s));
    }

    size_t reference_tracker::roots() const { return roots_.size(); }
    uint32_t reference_tracker::root(size_t r) const { return roots_[r].id; }
    size_t reference_tracker::objects() const { return objects_.size(); }
    uint32_t reference_tracker::object(size_t i) const { return objects_[i]; }
    uint32_t reference_tracker::generation(size_t r) const { return roots_[r].generations[roots_[r].at]; }

    void reference_tracker::observe(size_t r, const float* pose) {
        root_state& s = roots_[r];
        const size_t count = s.generations.size();
        const auto equal = [&](size_t i) { return std::memcmp(s.poses.data() + i * kPose, pose, kPose * sizeof(float)) == 0; };
        if (equal(s.at)) return;
        for (size_t i = s.at + 1; i < count; ++i)
            if (equal(i)) {
                s.at = i;
                ++remade_;
                return;
            }
        ++(s.at + 1 < count ? truncated_ : moved_);
        s.poses.resize((s.at + 1) * kPose);
        s.generations.resize(s.at + 1);
        s.poses.insert(s.poses.end(), pose, pose + kPose);
        s.generations.push_back(s.next++);
        s.at = s.generations.size() - 1;
        if (s.generations.size() >= 2 * kDepth) {   // amortized: the oldest kDepth at once
            s.poses.erase(s.poses.begin(), s.poses.begin() + static_cast<std::ptrdiff_t>(kDepth * kPose));
            s.generations.erase(s.generations.begin(), s.generations.begin() + static_cast<std::ptrdiff_t>(kDepth));
            s.at -= kDepth;
        }
    }

    bool reference_tracker::positions() const { return mode() == 2; }

    void reference_tracker::append(std::vector<uint8_t>& out) const {
        put16(out, roots_.size());
        out.push_back(positions() ? kHoldReferencePositions : uint8_t{0});
        for (size_t r = 0; r < roots_.size(); ++r) put32(out, generation(r));
        if (positions()) put16(out, objects_.size());
    }

    void reference_tracker::append_keep(std::vector<uint8_t>& out) const {
        put16(out, roots_.size());
        out.push_back(0);
        for (size_t r = 0; r < roots_.size(); ++r) put32(out, kReferenceKeep);
    }

    void reference_tracker::fault(const std::string& what) {
        if (fault_first_.empty()) fault_first_ = what;
    }

    bool reference_tracker::check(const uint8_t* section, size_t size, std::string& error) {
        carried_.clear();
        for (root_state& s: roots_) s.pending = kNone;
        hold_layout h;
        if (!parse_hold(section, size, h) || h.end != size || h.roots != roots_.size()
            || h.references != ((h.flags & kHoldReferencePositions) ? objects_.size() : 0u)) {
            error = "proximity hold section: " + std::to_string(h.roots) + " reference roots, this world has "
                  + std::to_string(roots_.size());
            return false;
        }
        for (size_t r = 0; r < roots_.size(); ++r) {
            root_state& s = roots_[r];
            // A move the world has not made again by now (the replay since
            // the last restore stopped early, or a writer outside it).
            if (s.at + 1 < s.generations.size()) {
                ++unmade_;
                fault("unmade: root " + std::to_string(s.id) + " at generation " + std::to_string(generation(r))
                      + " of " + std::to_string(s.generations.back()));
            }
            const uint32_t g = get32(section + h.roots_at + 3 + r * 4);
            if (g == kReferenceKeep || g == generation(r)) continue;
            const auto found = std::lower_bound(s.generations.begin(), s.generations.end(), g);
            if (found == s.generations.end() || *found != g) {
                ++refused_;
                fault("refused: root " + std::to_string(s.id) + " generation " + std::to_string(g) + " unknown");
                continue;
            }
            s.pending = static_cast<size_t>(found - s.generations.begin());
        }
        if (h.flags & kHoldReferencePositions)
            carried_.assign(section + h.references_at + 2, section + h.references_at + 2 + h.references * 12);
        return true;
    }

    const float* reference_tracker::pending(size_t r) const {
        const root_state& s = roots_[r];
        return s.pending == kNone ? nullptr : s.poses.data() + s.pending * kPose;
    }

    void reference_tracker::refuse_missing(size_t r) {
        ++refused_;
        fault("refused: root " + std::to_string(roots_[r].id) + " is gone");
        roots_[r].pending = kNone;
    }

    void reference_tracker::wrote(size_t r) {
        root_state& s = roots_[r];
        s.at = s.pending;
        s.pending = kNone;
        s.written = true;
        ++written_;
    }

    bool reference_tracker::carried(size_t i, uint32_t xyz[3]) const {
        if ((i + 1) * 12 > carried_.size()) return false;
        for (int c = 0; c < 3; ++c) xyz[c] = get32(carried_.data() + i * 12 + c * 4);
        return true;
    }

    void reference_tracker::compared(bool equal) {
        ++verified_;
        if (!equal) ++differing_;
    }

    bool reference_tracker::take_written(size_t r) {
        const bool w = roots_[r].written;
        roots_[r].written = false;
        return w;
    }

    void reference_tracker::bodied(const char* name) {
        ++bodied_;
        if (bodied_first_.empty() && name) bodied_first_ = name;
    }

    void reference_tracker::read_object(size_t i, uint32_t read) {
        if (i >= objects_.size() || objects_[i] == read) return;
        ++misread_;
        fault("misread: reference " + std::to_string(i) + " tracked " + std::to_string(objects_[i]) + ", read "
              + std::to_string(read));
    }

    std::string reference_tracker::line() const {
        static const char* const kModes[] = {"off", "on", "verify"};
        return std::string("refs=") + kModes[mode()] + "/" + std::to_string(roots_.size()) + " moved=" + std::to_string(moved_)
             + " written=" + std::to_string(written_) + " remade=" + std::to_string(remade_) + " truncated="
             + std::to_string(truncated_) + " unmade=" + std::to_string(unmade_) + " bodied=" + std::to_string(bodied_)
             + (bodied_first_.empty() ? std::string() : " (" + bodied_first_ + ")") + " verified=" + std::to_string(verified_)
             + " differing=" + std::to_string(differing_) + " refused=" + std::to_string(refused_)
             + " misread=" + std::to_string(misread_) + (fault_first_.empty() ? std::string() : " (" + fault_first_ + ")");
    }

}
