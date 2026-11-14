#pragma once

// Which names a set holds, as a filter: `maybe` is false only for a name never
// added, so a "no" is certain and a "yes" is to be settled by the caller.
// Two bits per name in a power-of-two bit table at least 32 bits per name
// (a false "yes" about 0.4 % of the time when full).  Step DWB: the sector
// placement's rename check (game/sector_placement.hpp) settles a "yes" by
// CK's own scan, so its answer is the scan's whatever the filter holds.
// No allocation per name; the owner keeps it (no shared state).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmmo::utility {
    class name_filter {
    public:
        explicit name_filter(size_t expected) {
            size_t bits = 4096;
            while (bits < expected * 32) bits *= 2;
            words_.assign(bits / 64, 0);
            mask_ = bits - 1;
        }
        void add(const char* name) {
            if (!name) return;
            const uint64_t h = hash(name);
            set(h & mask_);
            set((h >> 32) & mask_);
        }
        bool maybe(const char* name) const {
            if (!name) return false;
            const uint64_t h = hash(name);
            return test(h & mask_) && test((h >> 32) & mask_);
        }

    private:
        // FNV-1a over the bytes up to the terminator, then a final mix so the
        // two halves are independent enough for two bit positions.
        static uint64_t hash(const char* name) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
                h ^= *p;
                h *= 0x100000001b3ull;
            }
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            return h;
        }
        void set(uint64_t bit) { words_[static_cast<size_t>(bit >> 6)] |= uint64_t(1) << (bit & 63); }
        bool test(uint64_t bit) const { return (words_[static_cast<size_t>(bit >> 6)] >> (bit & 63)) & 1; }

        std::vector<uint64_t> words_;
        uint64_t mask_ = 0;
    };
}
