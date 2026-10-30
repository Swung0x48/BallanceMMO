#pragma once

// The savestate's controller alias for every mechanism force block outside a
// unit and a trafo set (world-agreement-gaps-design.md §4.3 and the §2.9 SPD
// row, "WAD-A"; FULL step WFA).
//
// A SetPhysicsForce block names its IVP controller after itself,
// "rt:force/<block id>" (engine change #18), and a block id is this
// process's CK_ID.  The retail process allocates a level's CK_IDs later than
// a headless world does (RT1 measured +1062 on Level 11: P_Modul_26_Sack
// 46734 on the server, 47796 in the game), so a blob that crosses worlds - a
// resync's or a late join's, which is the server's - named controllers no
// live one matched ("no live controller 'rt:force/46734'"), and a held force
// was restored onto nothing.  The units (§4.3, "unit:<owner>/<n>/S<k>") and
// the trafo sets ("trafo:<player>/<k>") had their own aliases already; this
// table gives every other force block the same kind of name: its root
// script's owner and its position n in the pre-order walk of that root (the
// root is 0; the walk of proximity_union.hpp, slot copies left out so n is
// the pre-copy position in every world), "force:<owner>/<n>".
//
// Header-only and free of CK and IVP: the hosts fill it from their scene
// (game/force_block_alias.hpp), the unit tests by hand.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bmmo::physics::force_alias {

    constexpr char kRawPrefix[] = "rt:force/";
    constexpr char kAliasPrefix[] = "force:";

    // "rt:force/<decimal id>" -> id; false for every other name.
    inline bool parse_raw_name(const char* name, uint32_t& id) {
        if (!name || std::strncmp(name, kRawPrefix, sizeof(kRawPrefix) - 1) != 0) return false;
        const char* digits = name + sizeof(kRawPrefix) - 1;
        if (*digits < '0' || *digits > '9') return false;
        char* end = nullptr;
        const unsigned long value = std::strtoul(digits, &end, 10);
        if (!end || *end) return false;
        id = static_cast<uint32_t>(value);
        return true;
    }

    // One force block as a host's scene census found it.
    struct block {
        uint32_t id = 0;
        std::string owner;      // the root script's owner ("" = none: never named)
        int walk = -1;          // pre-order position in the root script, slot copies left out
        bool outside = false;   // Gameplay_Ingame's blocks (9.3 (b) names them by their own names)
    };

    class table {
    public:
        // Every block with an owner and a walk position, not `outside`,
        // gets "force:<owner>/<walk>"; a name two blocks would share is given
        // to neither (fail closed: a wrong match would restore one block's
        // force onto another's controller).
        void build(const std::vector<block>& blocks) {
            clear();
            std::map<std::string, std::vector<uint32_t>> by_name;
            for (const block& b: blocks) {
                if (b.outside) {
                    ++outside_;
                    continue;
                }
                if (b.owner.empty() || b.walk < 0) {
                    ++unnamed_;
                    continue;
                }
                by_name[std::string(kAliasPrefix) + b.owner + "/" + std::to_string(b.walk)].push_back(b.id);
            }
            for (const auto& [name, ids]: by_name) {
                if (ids.size() != 1) {
                    duplicates_ += ids.size();
                    duplicate_names_.push_back(name);
                    continue;
                }
                names_[ids.front()] = name;
            }
            built_ = true;
        }
        void clear() {
            names_.clear();
            duplicate_names_.clear();
            outside_ = unnamed_ = duplicates_ = 0;
            built_ = false;
        }
        bool built() const { return built_; }
        // The name every world gives the controller of `name`'s block, or
        // false to keep the own name.
        bool alias(const char* name, std::string& out) const {
            uint32_t id = 0;
            if (names_.empty() || !parse_raw_name(name, id)) return false;
            const auto it = names_.find(id);
            if (it == names_.end()) return false;
            out = it->second;
            return true;
        }
        const std::map<uint32_t, std::string>& names() const { return names_; }
        const std::vector<std::string>& duplicate_names() const { return duplicate_names_; }
        size_t named() const { return names_.size(); }
        size_t outside() const { return outside_; }
        size_t unnamed() const { return unnamed_; }
        size_t duplicates() const { return duplicates_; }
        // "force block aliases: N named, D duplicated, O outside, U unnamed".
        std::string summary() const {
            std::string text = "force block aliases: " + std::to_string(names_.size()) + " named, " + std::to_string(duplicates_)
                + " duplicated, " + std::to_string(outside_) + " outside (Gameplay_Ingame), " + std::to_string(unnamed_) + " unnamed";
            for (const std::string& name: duplicate_names_) text += " [dup " + name + "]";
            return text;
        }

    private:
        std::map<uint32_t, std::string> names_;
        std::vector<std::string> duplicate_names_;
        size_t outside_ = 0, unnamed_ = 0, duplicates_ = 0;
        bool built_ = false;
    };
}
