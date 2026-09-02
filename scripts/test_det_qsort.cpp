// Fuzz test: XDeterministicQSort must produce exactly the permutation the
// Microsoft C runtime's qsort produces (the retail game's reference order).
//
//   cl /O2 /EHsc /I submodule\Ballanced\Source\VxMath\include scripts\test_det_qsort.cpp
//   test_det_qsort.exe [iterations]
//
// Elements carry a key with heavy duplication plus a unique tag, so equal keys
// are distinguishable and any ordering difference between the two sorts shows.
// On non-Microsoft platforms the test only checks that the output is sorted.
#include "XDeterministicSort.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct elem {
    int key;
    int tag;
    char pad[9]; // odd width exercises the byte-wise swap
};

static int compare_key(const void* a, const void* b) {
    const elem* x = static_cast<const elem*>(a);
    const elem* y = static_cast<const elem*>(b);
    return (x->key > y->key) - (x->key < y->key);
}

static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
static unsigned rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return static_cast<unsigned>(rng_state >> 11);
}

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 200000;
    int failures = 0;
    for (int it = 0; it < iterations; ++it) {
        const size_t n = rnd() % (it < 1000 ? 12 : 400);
        const int key_range = 1 + static_cast<int>(rnd() % (it % 3 == 0 ? 3 : 50));
        std::vector<elem> a(n);
        for (size_t i = 0; i < n; ++i) {
            a[i].key = static_cast<int>(rnd() % key_range);
            a[i].tag = static_cast<int>(i);
            std::memset(a[i].pad, static_cast<int>(i & 0xff), sizeof a[i].pad);
        }
        // Sorted / reversed runs too.
        if (it % 7 == 1) for (size_t i = 0; i + 1 < n; ++i) if (a[i].key > a[i + 1].key) { elem t = a[i]; a[i] = a[i + 1]; a[i + 1] = t; i = 0; }
        std::vector<elem> b = a;
        XDeterministicQSort(a.data(), n, sizeof(elem), compare_key);
        for (size_t i = 0; i + 1 < n; ++i) {
            if (a[i].key > a[i + 1].key) { std::printf("iteration %d: output not sorted\n", it); return 1; }
        }
#if defined(_MSC_VER)
        std::qsort(b.data(), n, sizeof(elem), compare_key);
        if (std::memcmp(a.data(), b.data(), n * sizeof(elem)) != 0) {
            if (failures < 5) {
                std::printf("iteration %d (n=%zu, keys<%d): permutation differs from MSVC qsort\n", it, n, key_range);
                for (size_t i = 0; i < n; ++i)
                    if (a[i].tag != b[i].tag) { std::printf("  first difference at %zu: det=%d/%d msvc=%d/%d\n", i, a[i].key, a[i].tag, b[i].key, b[i].tag); break; }
            }
            ++failures;
        }
#endif
    }
#if defined(_MSC_VER)
    std::printf("%d iterations, %d permutation mismatches against MSVC qsort\n", iterations, failures);
    return failures ? 1 : 0;
#else
    std::printf("%d iterations, all outputs sorted (no MSVC reference on this platform)\n", iterations);
    return 0;
#endif
}
