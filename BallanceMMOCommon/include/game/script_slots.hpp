#pragma once

// The per-player unit copies of a record (step SLOT, R1): a record carries
// the full state of the copies of its bound slots; a copy of an unbound slot
// is not walked but described against its BASE - its image at the copy step,
// never replaced - by a delta in the record's slot image section
// (script_replay kSectionSlotImages).  An unbound copy runs with its module's
// script (its activation words and its sensors' countdowns change every few
// ticks), so a restore writes base + the record's delta for every copy the
// record leaves out, whatever was restored before.  CK-free.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bmmo::game {
    class script_slots {
    public:
        void clear() { blocks_.clear(); bound_ = ~uint64_t{0}; learned_ = 0; }
        // A re-copy of the same block id (another rewire) replaces its base.
        void add(uint32_t id, uint8_t slot, std::vector<uint8_t> base) {
            const uint32_t h = hash(base);
            blocks_.insert_or_assign(id, block{uint64_t{1} << slot, std::move(base), h});
        }
        void select(uint64_t bound) { bound_ = bound | learned_; }
        void select_record(uint64_t bound) { bound_ = bound; }
        void learn_mask(uint64_t bound) { learned_ |= bound; }
        uint64_t mask() const { return bound_; }
        bool bound(uint8_t slot) const { return (bound_ & (uint64_t{1} << slot)) != 0; }
        // A copy block whose slot the selected mask leaves out.
        bool omit(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found != blocks_.end() && !(bound_ & found->second.slot);
        }
        const std::vector<uint8_t>* base(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found == blocks_.end() ? nullptr : &found->second.base;
        }
        uint32_t base_hash(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found == blocks_.end() ? 0u : found->second.base_hash;
        }

        // FNV-1a 32 of a base (the section names the base it was made from).
        static uint32_t hash(const std::vector<uint8_t>& bytes) {
            uint32_t h = 2166136261u;
            for (uint8_t b: bytes) h = (h ^ b) * 16777619u;
            return h;
        }

        // `image` against `base`, appended to `out`; false (nothing written)
        // when they are equal.  Same size: u8 0, u16 run count, each run u32
        // offset, u16 length, the bytes; another size (or too many runs):
        // u8 1, u32 size, the whole image.
        static bool put_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& image, std::vector<uint8_t>& out) {
            if (image == base) return false;
            const auto u8 = [&out](uint8_t v) { out.push_back(v); };
            const auto u16 = [&out](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
            const auto u32 = [&out](uint32_t v) { for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k))); };
            const auto whole = [&] {
                u8(1);
                u32(static_cast<uint32_t>(image.size()));
                out.insert(out.end(), image.begin(), image.end());
                return true;
            };
            if (image.size() != base.size()) return whole();
            // Runs of differing bytes; two runs closer than a run header
            // (6 bytes) are one.
            std::vector<std::pair<size_t, size_t>> runs;   // [begin, end)
            for (size_t k = 0; k < image.size();) {
                if (image[k] == base[k]) { ++k; continue; }
                size_t end = k + 1;
                while (end < image.size() && image[end] != base[end]) ++end;
                if (!runs.empty() && k - runs.back().second < 6 && end - runs.back().first <= 0xffffu) runs.back().second = end;
                else runs.emplace_back(k, end);
                k = end;
            }
            size_t bytes = 3;
            for (auto& r: runs) bytes += 6 + (r.second - r.first);
            if (runs.size() > 0xffffu || bytes >= image.size() + 5) return whole();
            for (const auto& r: runs)
                if (r.second - r.first > 0xffffu) return whole();
            u8(0);
            u16(static_cast<uint16_t>(runs.size()));
            for (const auto& r: runs) {
                u32(static_cast<uint32_t>(r.first));
                u16(static_cast<uint16_t>(r.second - r.first));
                out.insert(out.end(), image.begin() + static_cast<std::ptrdiff_t>(r.first),
                           image.begin() + static_cast<std::ptrdiff_t>(r.second));
            }
            return true;
        }
        // The image a delta at `pos` describes against `base`; false on a
        // truncated or malformed delta.
        static bool get_delta(const std::vector<uint8_t>& base, const std::vector<uint8_t>& in, size_t& pos,
                              std::vector<uint8_t>& image) {
            const auto need = [&](size_t n) { return pos + n <= in.size(); };
            const auto u16 = [&]() { uint16_t v = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8)); pos += 2; return v; };
            const auto u32 = [&]() {
                uint32_t v = 0;
                for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(in[pos + static_cast<size_t>(k)]) << (8 * k);
                pos += 4;
                return v;
            };
            if (!need(1)) return false;
            const uint8_t kind = in[pos++];
            if (kind == 1) {
                if (!need(4)) return false;
                const uint32_t size = u32();
                if (!need(size)) return false;
                image.assign(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + size));
                pos += size;
                return true;
            }
            if (kind != 0 || !need(2)) return false;
            const uint16_t runs = u16();
            image = base;
            for (uint16_t i = 0; i < runs; ++i) {
                if (!need(6)) return false;
                const uint32_t offset = u32();
                const uint16_t length = u16();
                if (!need(length) || static_cast<size_t>(offset) + length > image.size()) return false;
                std::memcpy(image.data() + offset, in.data() + pos, length);
                pos += length;
            }
            return true;
        }
        // Past a delta (no base needed).
        static bool skip_delta(const std::vector<uint8_t>& in, size_t& pos) {
            if (pos + 1 > in.size()) return false;
            const uint8_t kind = in[pos++];
            if (kind == 1) {
                if (pos + 4 > in.size()) return false;
                uint32_t size = 0;
                for (int k = 0; k < 4; ++k) size |= static_cast<uint32_t>(in[pos + static_cast<size_t>(k)]) << (8 * k);
                pos += 4;
                if (pos + size > in.size()) return false;
                pos += size;
                return true;
            }
            if (kind != 0 || pos + 2 > in.size()) return false;
            const uint16_t runs = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
            pos += 2;
            for (uint16_t i = 0; i < runs; ++i) {
                if (pos + 6 > in.size()) return false;
                const uint16_t length = static_cast<uint16_t>(in[pos + 4] | (in[pos + 5] << 8));
                pos += 6;
                if (pos + length > in.size()) return false;
                pos += length;
            }
            return true;
        }
    private:
        struct block {
            uint64_t slot;
            std::vector<uint8_t> base;
            uint32_t base_hash;
        };
        std::unordered_map<uint32_t, block> blocks_;
        uint64_t bound_ = ~uint64_t{0}, learned_ = 0;
    };
}
