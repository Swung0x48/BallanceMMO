// Differential test: retail VxMath.dll (what the shipped game runs) against
// the Ballanced fork's VxMath (what the headless server and the BallanceMMO
// physics plugin run).  Bit-exact comparison, so a mismatch names the exact
// function that has to be reconciled before the two engines can agree.
//
// Two input populations are reported separately.  "shaped" matrices look like
// the transforms Ballance actually stores (near-unit scale, level-sized
// translations); "random" ones are unconstrained and routinely produce
// catastrophic cancellation, where a one-bit difference shows up as a huge ulp
// gap.  Only the shaped column says anything about the game.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cfloat>
#include <cstdlib>

extern "C" {
void retail_mul(float *, const float *, const float *);
void fork_mul(float *, const float *, const float *);
void retail_mul4(float *, const float *, const float *);
void fork_mul4(float *, const float *, const float *);
void retail_inverse(float *, const float *);
void fork_inverse(float *, const float *);
void retail_mulvec(float *, const float *, const float *);
void fork_mulvec(float *, const float *, const float *);
void retail_rotatevec(float *, const float *, const float *);
void fork_rotatevec(float *, const float *, const float *);
void retail_quat_from_matrix(float *, const float *);
void fork_quat_from_matrix(float *, const float *);
void retail_matrix_from_rotation(float *, const float *, float);
void fork_matrix_from_rotation(float *, const float *, float);
void retail_decompose(float *, float *, float *, const float *);
void fork_decompose(float *, float *, float *, const float *);
void retail_normalize(float *, const float *);
void fork_normalize(float *, const float *);
int retail_load(const char *);
void retail_interpolate(float *, float, const float *, const float *);
void fork_interpolate(float *, float, const float *, const float *);
}

static uint32_t rng_state = 0x1234567u;
static uint32_t next_u32() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static float next_float(float lo, float hi) {
    return lo + (hi - lo) * (float)(next_u32() >> 8) / (float)(1u << 24);
}

static void make_ballance_matrix(float *m) {
    const float angle = next_float(-3.15f, 3.15f);
    float ax[3] = {next_float(-0.05f, 0.05f), 1.0f, next_float(-0.05f, 0.05f)};
    const float len = std::sqrt(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
    ax[0] /= len; ax[1] /= len; ax[2] /= len;
    fork_matrix_from_rotation(m, ax, angle);
    const float s[3] = {next_float(0.99998f, 1.00002f), next_float(0.99998f, 1.00002f),
                        next_float(0.99998f, 1.00002f)};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) m[r * 4 + c] *= s[r];
    m[12] = next_float(-120.0f, 120.0f);
    m[13] = next_float(-20.0f, 60.0f);
    m[14] = next_float(-120.0f, 120.0f);
    m[3] = m[7] = m[11] = 0.0f; m[15] = 1.0f;
}

static void make_random_matrix(float *m) {
    for (int i = 0; i < 16; ++i) m[i] = next_float(-4.0f, 4.0f);
    m[3] = m[7] = m[11] = 0.0f; m[15] = 1.0f;
}

struct Stat {
    int checked = 0, differing = 0, worst_ulp = 0;
    double worst_relative = 0.0;
};

static int ulp_gap(float a, float b) {
    if (std::memcmp(&a, &b, 4) == 0) return 0;
    if (std::isnan(a) || std::isnan(b)) return 1 << 30;
    int32_t ia, ib;
    std::memcpy(&ia, &a, 4); std::memcpy(&ib, &b, 4);
    if (ia < 0) ia = (int32_t)0x80000000 - ia;
    if (ib < 0) ib = (int32_t)0x80000000 - ib;
    const int gap = ia - ib;
    return gap < 0 ? -gap : gap;
}

static const char *NAMES[] = {
    "Vx3DMultiplyMatrix", "Vx3DMultiplyMatrix4", "Vx3DInverseMatrix",
    "Vx3DMultiplyMatrixVector", "Vx3DRotateVector", "VxQuaternion::FromMatrix",
    "Vx3DMatrixFromRotation", "Vx3DDecomposeMatrix", "VxVector::Normalize",
    "Vx3DInterpolateMatrix"};
enum { N_FUNCS = 10 };

static Stat stats[2][N_FUNCS];
static int shown[N_FUNCS];

static void compare(int kind, int which, const float *a, const float *b, int n) {
    Stat &st = stats[kind][which];
    st.checked++;
    int worst = 0;
    double worst_rel = 0.0;
    for (int i = 0; i < n; ++i) {
        const int gap = ulp_gap(a[i], b[i]);
        if (gap > worst) worst = gap;
        const double scale = std::fabs((double)a[i]) + std::fabs((double)b[i]);
        if (scale > 1e-12) {
            const double rel = std::fabs((double)a[i] - (double)b[i]) / scale;
            if (rel > worst_rel) worst_rel = rel;
        }
    }
    if (!worst) return;
    st.differing++;
    if (worst > st.worst_ulp) st.worst_ulp = worst;
    if (worst_rel > st.worst_relative) st.worst_relative = worst_rel;
    if (kind == 1 && shown[which] < 1) {
        shown[which]++;
        std::printf("  first shaped-input mismatch in %s (%d ulp):\n", NAMES[which], worst);
        for (int i = 0; i < n && i < 16; ++i)
            if (ulp_gap(a[i], b[i]))
                std::printf("     [%2d] retail %-16.9g %08x   fork %-16.9g %08x\n", i,
                            a[i], *(const uint32_t *)&a[i], b[i], *(const uint32_t *)&b[i]);
    }
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1]
                               : "C:/Users/geekerwan/Downloads/Ballance-MMOTestCopy3/Bin/VxMath.dll";
    if (!retail_load(dll)) { std::printf("could not bind retail VxMath\n"); return 2; }
    std::printf("retail VxMath: %s\n", dll);

    // The retail client runs the x87 unit at 24-bit precision (observed control
    // word 000a001f), so retail VxMath's arithmetic rounds every step to float
    // there.  Reproduce that, or the comparison is against a configuration the
    // game never runs.  Pass --pc53 to see the default instead.
    const bool pc53 = argc > 2 && std::strcmp(argv[2], "--pc53") == 0;
    if (!pc53) {
        unsigned old = 0, now = 0;
        _controlfp_s(&old, _PC_24, _MCW_PC);
        _controlfp_s(&now, 0, 0);
        std::printf("x87 precision control: 24-bit (control word %08x)\n", now);
    } else {
        std::printf("x87 precision control: compiler default\n");
    }

    {   // conventions must agree before any number below means anything
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float trans[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 7,11,13,1};
        float scale[16] = {2,0,0,0, 0,3,0,0, 0,0,5,0, 0,0,0,1};
        float out[16], vo[3], v[3] = {1, 2, 3};
        for (int side = 0; side < 2; ++side) {
            void (*mul)(float *, const float *, const float *) = side ? fork_mul : retail_mul;
            void (*mulvec)(float *, const float *, const float *) = side ? fork_mulvec : retail_mulvec;
            mul(out, trans, ident);
            const float t0 = out[12], t1 = out[13], t2 = out[14];
            mul(out, scale, trans);
            mulvec(vo, trans, v);
            std::printf("%-7s conventions: T*I=(%.0f %.0f %.0f)  S*T diag=(%.0f %.0f %.0f) "
                        "pos=(%.0f %.0f %.0f)  T*(1,2,3)=(%.0f %.0f %.0f)\n",
                        side ? "fork" : "retail", t0, t1, t2, out[0], out[5], out[10],
                        out[12], out[13], out[14], vo[0], vo[1], vo[2]);
        }
        std::printf("\n");
    }

    {   // spot check the three functions that differ structurally rather than
        // by rounding, on an input whose correct answer is known by hand
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        const float angle = 1.57079632679f;          // 90 degrees about +Y
        float r[16], f[16];
        retail_matrix_from_rotation(r, axis, angle);
        fork_matrix_from_rotation(f, axis, angle);
        std::printf("Vx3DMatrixFromRotation(+Y, 90 deg)\n");
        for (int row = 0; row < 4; ++row)
            std::printf("   retail %8.4f %8.4f %8.4f %8.4f    fork %8.4f %8.4f %8.4f %8.4f\n",
                        r[row*4], r[row*4+1], r[row*4+2], r[row*4+3],
                        f[row*4], f[row*4+1], f[row*4+2], f[row*4+3]);

        float rq[4], rp[3], rs[3], fq[4], fp[3], fs[3];
        retail_decompose(rq, rp, rs, r); fork_decompose(fq, fp, fs, r);
        std::printf("Vx3DDecomposeMatrix of that same retail matrix\n");
        std::printf("   retail quat %.4f %.4f %.4f %.4f  pos %.3f %.3f %.3f  scale %.4f %.4f %.4f\n",
                    rq[0], rq[1], rq[2], rq[3], rp[0], rp[1], rp[2], rs[0], rs[1], rs[2]);
        std::printf("   fork   quat %.4f %.4f %.4f %.4f  pos %.3f %.3f %.3f  scale %.4f %.4f %.4f\n",
                    fq[0], fq[1], fq[2], fq[3], fp[0], fp[1], fp[2], fs[0], fs[1], fs[2]);

        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float ri[16], fi[16];
        retail_interpolate(ri, 0.0f, ident, r);
        fork_interpolate(fi, 0.0f, ident, r);
        std::printf("Vx3DInterpolateMatrix(step=0, I, R) should return I\n");
        std::printf("   retail diag %.4f %.4f %.4f   fork diag %.4f %.4f %.4f\n\n",
                    ri[0], ri[5], ri[10], fi[0], fi[5], fi[10]);
    }

    const int ROUNDS = 200000;
    for (int round = 0; round < ROUNDS; ++round) {
        const int kind = round & 1;   // 0 = random, 1 = Ballance-shaped
        float a[16], b[16], r[16], f[16];
        if (kind) { make_ballance_matrix(a); make_ballance_matrix(b); }
        else      { make_random_matrix(a);   make_random_matrix(b); }

        retail_mul(r, a, b);       fork_mul(f, a, b);       compare(kind, 0, r, f, 16);
        retail_mul4(r, a, b);      fork_mul4(f, a, b);      compare(kind, 1, r, f, 16);
        retail_inverse(r, a);      fork_inverse(f, a);      compare(kind, 2, r, f, 16);

        float v[3] = {next_float(-50.f, 50.f), next_float(-50.f, 50.f), next_float(-50.f, 50.f)};
        retail_mulvec(r, a, v);    fork_mulvec(f, a, v);    compare(kind, 3, r, f, 3);
        retail_rotatevec(r, a, v); fork_rotatevec(f, a, v); compare(kind, 4, r, f, 3);
        retail_quat_from_matrix(r, a); fork_quat_from_matrix(f, a); compare(kind, 5, r, f, 4);

        float axis[3] = {next_float(-1.f, 1.f), next_float(-1.f, 1.f), next_float(-1.f, 1.f)};
        const float len = std::sqrt(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
        if (len > 1e-6f) { axis[0] /= len; axis[1] /= len; axis[2] /= len; }
        const float angle = next_float(-3.15f, 3.15f);
        retail_matrix_from_rotation(r, axis, angle);
        fork_matrix_from_rotation(f, axis, angle);
        compare(kind, 6, r, f, 16);

        float rq[4], rp[3], rs[3], fq[4], fp[3], fs[3], pr[10], pf[10];
        retail_decompose(rq, rp, rs, a); fork_decompose(fq, fp, fs, a);
        std::memcpy(pr, rq, 16); std::memcpy(pr + 4, rp, 12); std::memcpy(pr + 7, rs, 12);
        std::memcpy(pf, fq, 16); std::memcpy(pf + 4, fp, 12); std::memcpy(pf + 7, fs, 12);
        compare(kind, 7, pr, pf, 10);

        retail_normalize(r, v);    fork_normalize(f, v);    compare(kind, 8, r, f, 3);
        const float step = next_float(0.f, 1.f);
        retail_interpolate(r, step, a, b); fork_interpolate(f, step, a, b);
        compare(kind, 9, r, f, 16);
    }

    std::printf("\n%-26s | %-28s | %s\n", "", "Ballance-shaped inputs", "unconstrained inputs");
    std::printf("%-26s | %8s %8s %9s | %8s %8s\n", "function", "differ", "worst", "worst rel",
                "differ", "worst");
    int bad_shaped = 0;
    for (int i = 0; i < N_FUNCS; ++i) {
        const Stat &s = stats[1][i], &rnd = stats[0][i];
        std::printf("%-26s | %7d%% %8d %9.1e | %7d%% %8d%s\n", NAMES[i],
                    s.checked ? s.differing * 100 / s.checked : 0, s.worst_ulp, s.worst_relative,
                    rnd.checked ? rnd.differing * 100 / rnd.checked : 0, rnd.worst_ulp,
                    s.differing ? "   <-- DIFFERS" : "");
        if (s.differing) bad_shaped++;
    }
    std::printf("\n%d of %d functions differ on Ballance-shaped inputs\n", bad_shaped, N_FUNCS);
    return 0;
}
