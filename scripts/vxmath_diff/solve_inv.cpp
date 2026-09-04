// Recover how retail VxMath.dll computes Vx3DInverseMatrix.
//
// It is a cofactor inverse either way; what is unknown is where the arithmetic
// widens to double, whether the reciprocal is a double division or a float
// one, and the order the determinant's three terms are summed.  Enumerate
// those axes and see which combination reproduces retail bit-for-bit.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>
#include <cstdlib>
#include <cmath>

extern "C" {
int retail_load(const char *);
void retail_inverse(float *, const float *);
}

static uint32_t rng = 0xfeed1234u;
static uint32_t next_u32() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float next_float(float lo, float hi) {
    return lo + (hi - lo) * (float)(next_u32() >> 8) / (float)(1u << 24);
}

// det: 0 = float left fold, 1 = float pairwise, 2 = double accumulate
// recip: 0 = (float)(1.0 / (double)det), 1 = 1.0f / (float)det
// scale: 0 = float cofactor * float invdet, 1 = (float)((double)cofactor / det)
// cof:   0 = cofactors in float, 1 = cofactors in double
// det:   0 = (t0+t1)+t2 in float, 1 = t0+(t1+t2) in float, 2 = accumulated in double
// recip: 0 = (float)(1.0/(double)det), 1 = 1.0f/(float)det, 2 = keep 1.0/det in double
// scale: 0 = float multiply, 1 = double multiply then round once
struct Variant { int cof, det, recip, scale; };

static bool try_variant(const Variant &v, const float *m, const float *want) {
    const float a00=m[0], a01=m[1], a02=m[2];
    const float a10=m[4], a11=m[5], a12=m[6];
    const float a20=m[8], a21=m[9], a22=m[10];

    double c[9];
    if (v.cof == 0) {
        const float f[9] = {
            a11*a22 - a12*a21, a02*a21 - a01*a22, a01*a12 - a02*a11,
            a12*a20 - a10*a22, a00*a22 - a02*a20, a02*a10 - a00*a12,
            a10*a21 - a11*a20, a01*a20 - a00*a21, a00*a11 - a01*a10};
        for (int i = 0; i < 9; ++i) c[i] = f[i];
    } else {
        c[0] = (double)a11*a22 - (double)a12*a21;
        c[1] = (double)a02*a21 - (double)a01*a22;
        c[2] = (double)a01*a12 - (double)a02*a11;
        c[3] = (double)a12*a20 - (double)a10*a22;
        c[4] = (double)a00*a22 - (double)a02*a20;
        c[5] = (double)a02*a10 - (double)a00*a12;
        c[6] = (double)a10*a21 - (double)a11*a20;
        c[7] = (double)a01*a20 - (double)a00*a21;
        c[8] = (double)a00*a11 - (double)a01*a10;
    }

    double det;
    if (v.det == 2) det = (double)a00*c[0] + (double)a01*c[3] + (double)a02*c[6];
    else {
        const float t0 = a00*(float)c[0], t1 = a01*(float)c[3], t2 = a02*(float)c[6];
        det = (double)(v.det == 0 ? ((t0 + t1) + t2) : (t0 + (t1 + t2)));
    }
    if (std::fabs(det) < 1e-6) return true;      // both sides bail out here

    float out[16];
    const double invd = 1.0 / det;
    const float invf = v.recip == 0 ? (float)invd : (v.recip == 1 ? 1.0f / (float)det : (float)invd);
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            const int i = r * 3 + k;
            if (v.scale == 0) out[r*4+k] = (float)c[i] * invf;
            else out[r*4+k] = (float)(c[i] * (v.recip == 2 ? invd : (double)invf));
        }
    for (int k = 0; k < 9; ++k)
        if (std::memcmp(&out[(k/3)*4 + (k%3)], &want[(k/3)*4 + (k%3)], 4) != 0) return false;
    return true;
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1]
                               : "C:/Users/geekerwan/Downloads/Ballance-MMOTestCopy3/Bin/VxMath.dll";
    if (!retail_load(dll)) return 2;
    unsigned old = 0;
    _controlfp_s(&old, _PC_24, _MCW_PC);

    Variant variants[64];
    int n = 0;
    for (int cf = 0; cf < 2; ++cf)
        for (int d = 0; d < 3; ++d)
            for (int r = 0; r < 3; ++r)
                for (int s = 0; s < 2; ++s) variants[n++] = {cf, d, r, s};
    bool alive[64];
    for (int i = 0; i < n; ++i) alive[i] = true;

    // also learn the translation row separately, once the 3x3 is known
    int trans_left = 0, trans_pair = 0, trans_neg_inside = 0, trans_cases = 0;

    const int ROUNDS = 40000;
    for (int round = 0; round < ROUNDS; ++round) {
        float M[16], R[16];
        // rotation-like matrices, the shape entity world matrices actually take
        for (int i = 0; i < 16; ++i) M[i] = next_float(-2.f, 2.f);
        M[3] = M[7] = M[11] = 0.f; M[15] = 1.f;
        M[12] = next_float(-100.f, 100.f);
        M[13] = next_float(-100.f, 100.f);
        M[14] = next_float(-100.f, 100.f);
        retail_inverse(R, M);

        for (int i = 0; i < n; ++i)
            if (alive[i] && !try_variant(variants[i], M, R)) alive[i] = false;

        // translation row: -(inv[0][k]*tx + inv[1][k]*ty + inv[2][k]*tz)
        const float tx = M[12], ty = M[13], tz = M[14];
        bool ok_left = true, ok_pair = true, ok_neg = true;
        for (int k = 0; k < 3; ++k) {
            const float p0 = R[0*4+k]*tx, p1 = R[1*4+k]*ty, p2 = R[2*4+k]*tz;
            const float left = -((p0 + p1) + p2);
            const float pair = -(p0 + (p1 + p2));
            const float neg  = ((-p0) - p1) - p2;
            if (std::memcmp(&left, &R[12+k], 4)) ok_left = false;
            if (std::memcmp(&pair, &R[12+k], 4)) ok_pair = false;
            if (std::memcmp(&neg,  &R[12+k], 4)) ok_neg = false;
        }
        trans_cases++;
        trans_left += ok_left; trans_pair += ok_pair; trans_neg_inside += ok_neg;
    }

    const char *COF[2] = {"float cofactors", "double cofactors"};
    const char *DET[3] = {"float (t0+t1)+t2", "float t0+(t1+t2)", "double accumulate"};
    const char *REC[3] = {"(float)(1.0/det)", "1.0f/(float)det", "1.0/det kept double"};
    const char *SCL[2] = {"float multiply", "double multiply, round once"};
    std::printf("Vx3DInverseMatrix 3x3 part: variants reproducing retail\n");
    int hits = 0;
    for (int i = 0; i < n; ++i)
        if (alive[i]) {
            std::printf("   det=%-20s recip=%-26s scale=%s\n",
                        DET[variants[i].det], REC[variants[i].recip], SCL[variants[i].scale]);
            hits++;
        }
    if (!hits) std::printf("   none of the %d enumerated variants\n", n);

    std::printf("\ntranslation row over %d matrices (given retail's own 3x3):\n", trans_cases);
    std::printf("   -( (p0+p1)+p2 )   matched %d\n", trans_left);
    std::printf("   -( p0+(p1+p2) )   matched %d\n", trans_pair);
    std::printf("   ((-p0)-p1)-p2     matched %d\n", trans_neg_inside);
    return 0;
}
