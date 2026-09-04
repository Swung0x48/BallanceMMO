// Recover the exact evaluation order retail VxMath.dll uses for
// Vx3DMultiplyMatrix.
//
// Both engines compute the same four products for each output element; if they
// disagree only in the order the four are summed, then one candidate ordering
// will reproduce retail bit-for-bit on every input, and porting that ordering
// into the fork's VxMath removes the divergence without replacing any DLL.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>
#include <cstdlib>

extern "C" {
int retail_load(const char *);
void retail_mul(float *, const float *, const float *);
}

static uint32_t rng = 0x2468aceu;
static uint32_t next_u32() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static float next_float(float lo, float hi) {
    return lo + (hi - lo) * (float)(next_u32() >> 8) / (float)(1u << 24);
}

// Every distinct way to sum four floats: 3 tree shapes over the 4! orderings,
// deduplicated by construction into a compact table of (permutation, shape).
struct Candidate { const char *name; int perm[4]; int shape; };

static float sum4(const float *p, const int *o, int shape) {
    const float a = p[o[0]], b = p[o[1]], c = p[o[2]], d = p[o[3]];
    switch (shape) {
        case 0: return ((a + b) + c) + d;      // strict left fold
        case 1: return (a + b) + (c + d);      // pairwise
        case 2: return a + (b + (c + d));      // right fold
        default: return ((a + b) + c) + d;
    }
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1]
                               : "C:/Users/geekerwan/Downloads/Ballance-MMOTestCopy3/Bin/VxMath.dll";
    if (!retail_load(dll)) return 2;
    if (!(argc > 2 && std::strcmp(argv[2], "--pc53") == 0)) {
        unsigned old = 0; _controlfp_s(&old, _PC_24, _MCW_PC);
    }

    static const int PERMS[24][4] = {
        {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},
        {1,0,2,3},{1,0,3,2},{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},
        {2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},{2,3,0,1},{2,3,1,0},
        {3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}};
    const char *SHAPES[3] = {"((a+b)+c)+d", "(a+b)+(c+d)", "a+(b+(c+d))"};

    // survivors[perm][shape] stays true while that ordering still matches
    bool alive[24][3];
    for (int p = 0; p < 24; ++p) for (int s = 0; s < 3; ++s) alive[p][s] = true;
    int alive_count = 24 * 3;

    const int ROUNDS = 60000;
    int round = 0;
    for (; round < ROUNDS && alive_count > 0; ++round) {
        float A[16], B[16], R[16];
        for (int i = 0; i < 16; ++i) { A[i] = next_float(-4.f, 4.f); B[i] = next_float(-4.f, 4.f); }
        A[3] = A[7] = A[11] = 0.f; A[15] = 1.f;
        B[3] = B[7] = B[11] = 0.f; B[15] = 1.f;
        retail_mul(R, A, B);

        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                if (j == 3) continue;            // the affine column is forced, not summed
                const float prod[4] = {A[0*4+j]*B[i*4+0], A[1*4+j]*B[i*4+1],
                                       A[2*4+j]*B[i*4+2], A[3*4+j]*B[i*4+3]};
                const float want = R[i*4+j];
                for (int p = 0; p < 24; ++p) {
                    for (int s = 0; s < 3; ++s) {
                        if (!alive[p][s]) continue;
                        const float got = sum4(prod, PERMS[p], s);
                        if (std::memcmp(&got, &want, 4) != 0) { alive[p][s] = false; alive_count--; }
                    }
                }
            }
        }
    }

    std::printf("tested %d random matrix pairs\n", round);
    std::printf("orderings that reproduce retail Vx3DMultiplyMatrix bit-for-bit: %d\n\n", alive_count);
    for (int p = 0; p < 24; ++p)
        for (int s = 0; s < 3; ++s)
            if (alive[p][s])
                std::printf("  sum order (%d,%d,%d,%d) shape %s\n", PERMS[p][0], PERMS[p][1],
                            PERMS[p][2], PERMS[p][3], SHAPES[s]);
    if (!alive_count)
        std::printf("  none - retail does not simply reorder the same four products.\n"
                    "  It computes something else (wider intermediates, a different\n"
                    "  factorisation, or a vectorised path with its own rounding).\n");
    return 0;
}
