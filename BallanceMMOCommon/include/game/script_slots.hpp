#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bmmo::game {
    class script_slots {
    public:
        void clear() { blocks_.clear(); bound_ = ~uint64_t{0}; learned_ = 0; }
        void add(uint32_t id, uint8_t slot, std::vector<uint8_t> anchor) {
            blocks_.emplace(id, block{uint64_t{1} << slot, std::move(anchor), true});
        }
        void select(uint64_t bound) { bound_ = bound | learned_; }
        void select_record(uint64_t bound) { bound_ = bound; }
        void learn_mask(uint64_t bound) { learned_ |= bound; }
        uint64_t mask() const { return bound_; }
        bool bound(uint8_t slot) const { return (bound_ & (uint64_t{1} << slot)) != 0; }
        bool omit(uint32_t id) {
            const auto found = blocks_.find(id);
            if (found == blocks_.end()) return false;
            if (!(bound_ & found->second.slot)) return true;
            found->second.at_anchor = false;
            return false;
        }
        void recorded(uint32_t id) {
            const auto found = blocks_.find(id);
            if (found != blocks_.end()) found->second.at_anchor = false;
        }
        // A block of a slot the selection leaves out of the record.
        bool unbound(uint32_t id) const {
            const auto found = blocks_.find(id);
            return found != blocks_.end() && !(bound_ & found->second.slot);
        }
        // The live image of a block a capture leaves out: what a restore of a
        // record without its slot puts back.  The copy-step image is the
        // state before the module's script ever ran, which an unbound copy
        // does not keep: the script activates it with the original (its
        // graphs active since the sector's start), so a rewind across a late
        // join left the joiner's copies inactive for good (R1).
        void refresh(uint32_t id, std::vector<uint8_t> image) {
            const auto found = blocks_.find(id);
            if (found == blocks_.end()) return;
            found->second.anchor = std::move(image);
            found->second.at_anchor = true;
        }
        template <typename Write> bool restore_anchor(uint32_t id, Write&& write) {
            const auto found = blocks_.find(id);
            if (found == blocks_.end()) return false;
            auto& saved = found->second;
            if (!saved.at_anchor && !write(saved.anchor)) return false;
            saved.at_anchor = true;
            return true;
        }
    private:
        struct block {
            uint64_t slot;
            std::vector<uint8_t> anchor;
            bool at_anchor;
        };
        std::unordered_map<uint32_t, block> blocks_;
        uint64_t bound_ = ~uint64_t{0}, learned_ = 0;
    };
}
