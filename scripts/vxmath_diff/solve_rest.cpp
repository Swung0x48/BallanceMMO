// Same recovery technique as solve_mul, applied to the other VxMath routines
// CK3dEntity leans on: the two vector transforms and VxVector::Normalize.
// Each is a short sum of products, so exhausting the evaluation orders against
// the retail DLL pins down exactly what the shipped game computes.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>
#include <cstdlib>
#include <cmath>

extern "C" {
int retail_load(const char *);
void retail_mulvec(float *, const float *, const float *);
void retail_rotatevec(float *, const float *, const float *);
void retail_normalize(float *, const float *);
}

static uint32_t rng = 0x13579bdu;
static uint32_t next_u32() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float next_float(float lo, float hi) {
    return lo + (hi - lo) * (float)(next_u32() >> 8) / (float)(1u << 24);
}

static const int PERM4[24][4] = {
    {0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},
    {1,0,2,3},{1,0,3,2},{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},
    {2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},{2,3,0,1},{2,3,1,0},
    {3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}};
static const char *SHAPE4[3] = {"((a+b)+c)+d", "(a+b)+(c+d)", "a+(b+(c+d))"};
static float sum4(const float *p, const int *o, int shape) {
    const float a = p[o[0]], b = p[o[1]], c = p[o[2]], d = p[o[3]];
    if (shape == 0) return ((a + b) + c) + d;
    if (shape == 1) return (a + b) + (c + d);
    return a + (b + (c + d));
}

static const int PERM3[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
static const char *SHAPE3[2] = {"(a+b)+c", "a+(b+c)"};
static float sum3(const float *p, const int *o, int shape) {
    const float a = p[o[0]], b = p[o[1]], c = p[o[2]];
    return shape == 0 ? (a + b) + c : a + (b + c);
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1]
                               : "C:/Users/geekerwan/Downloads/Ballance-MMOTestCopy3/Bin/VxMath.dll";
    if (!retail_load(dll)) return 2;
    unsigned old = 0;
    _controlfp_s(&old, _PC_24, _MCW_PC);

    bool mv_alive[24][3], rot_alive[6][2];
    for (int p = 0; p < 24; ++p) for (int s = 0; s < 3; ++s) mv_alive[p][s] = true;
    for (int p = 0; p < 6; ++p) for (int s = 0; s < 2; ++s) rot_alive[p][s] = true;
    // Normalize: 3 ways to sum the squares x 2 ways to apply the length
    bool norm_alive[6][2];
    for (int p = 0; p < 6; ++p) for (int s = 0; s < 2; ++s) norm_alive[p][s] = true;

    const int ROUNDS = 40000;
    for (int round = 0; round < ROUNDS; ++round) {
        float M[16], v[3], out[3];
        for (int i = 0; i < 16; ++i) M[i] = next_float(-4.f, 4.f);
        M[3] = M[7] = M[11] = 0.f; M[15] = 1.f;
        for (int i = 0; i < 3; ++i) v[i] = next_float(-50.f, 50.f);

        retail_mulvec(out, M, v);
        for (int j = 0; j < 3; ++j) {
            const float prod[4] = {v[0]*M[0*4+j], v[1]*M[1*4+j], v[2]*M[2*4+j], M[3*4+j]};
            for (int p = 0; p < 24; ++p)
                for (int s = 0; s < 3; ++s)
                    if (mv_alive[p][s]) {
                        const float got = sum4(prod, PERM4[p], s);
                        if (std::memcmp(&got, &out[j], 4)) mv_alive[p][s] = false;
                    }
        }

        retail_rotatevec(out, M, v);
        for (int j = 0; j < 3; ++j) {
            const float prod[3] = {v[0]*M[0*4+j], v[1]*M[1*4+j], v[2]*M[2*4+j]};
            for (int p = 0; p < 6; ++p)
                for (int s = 0; s < 2; ++s)
                    if (rot_alive[p][s]) {
                        const float got = sum3(prod, PERM3[p], s);
                        if (std::memcmp(&got, &out[j], 4)) rot_alive[p][s] = false;
                    }
        }

        retail_normalize(out, v);
        {
            const float sq[3] = {v[0]*v[0], v[1]*v[1], v[2]*v[2]};
            for (int p = 0; p < 6; ++p)
                for (int s = 0; s < 2; ++s) {
                    if (!norm_alive[p][s]) continue;
                    const float len = std::sqrt(sum3(sq, PERM3[p], 0));
                    float got[3];
                    if (s == 0) { for (int k = 0; k < 3; ++k) got[k] = v[k] / len; }
                    else { const float inv = 1.0f / len; for (int k = 0; k < 3; ++k) got[k] = v[k] * inv; }
                    if (std::memcmp(got, out, 12)) norm_alive[p][s] = false;
                }
        }
    }

    std::printf("Vx3DMultiplyMatrixVector: orderings matching retail\n");
    int n = 0;
    for (int p = 0; p < 24; ++p) for (int s = 0; s < 3; ++s) if (mv_alive[p][s]) {
        std::printf("   (%d,%d,%d,%d) %s\n", PERM4[p][0], PERM4[p][1], PERM4[p][2], PERM4[p][3], SHAPE4[s]);
        n++;
    }
    if (!n) std::printf("   none\n");

    std::printf("Vx3DRotateVector: orderings matching retail\n");
    n = 0;
    for (int p = 0; p < 6; ++p) for (int s = 0; s < 2; ++s) if (rot_alive[p][s]) {
        std::printf("   (%d,%d,%d) %s\n", PERM3[p][0], PERM3[p][1], PERM3[p][2], SHAPE3[s]);
        n++;
    }
    if (!n) std::printf("   none\n");

    std::printf("VxVector::Normalize: variants matching retail\n");
    n = 0;
    for (int p = 0; p < 6; ++p) for (int s = 0; s < 2; ++s) if (norm_alive[p][s]) {
        std::printf("   squares summed (%d,%d,%d) then %s\n", PERM3[p][0], PERM3[p][1], PERM3[p][2],
                    s == 0 ? "v / len" : "v * (1/len)");
        n++;
    }
    if (!n) std::printf("   none\n");
    return 0;
}
