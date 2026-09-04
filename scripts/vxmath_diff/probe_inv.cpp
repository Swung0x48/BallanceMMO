// Characterise retail Vx3DInverseMatrix instead of guessing at it.
//
// Prints, for a few structured matrices, what the shipped DLL returns next to
// the two textbook candidates: the cofactor inverse and the "affine" inverse
// that transposes the rotation and divides each column by its row's squared
// length.  Whichever it tracks tells us which family to search in.
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdint>

extern "C" {
int retail_load(const char *);
void retail_inverse(float *, const float *);
void retail_mul(float *, const float *, const float *);
}

static void show(const char *label, const float *m) {
    std::printf("  %-22s", label);
    for (int i = 0; i < 4; ++i) {
        std::printf("\n     %10.6f %10.6f %10.6f %10.6f", m[i*4], m[i*4+1], m[i*4+2], m[i*4+3]);
    }
    std::printf("\n");
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1]
                               : "C:/Users/geekerwan/Downloads/Ballance-MMOTestCopy3/Bin/VxMath.dll";
    if (!retail_load(dll)) return 2;
    unsigned old = 0;
    _controlfp_s(&old, _PC_24, _MCW_PC);

    // A rotation about Y by 0.7 rad, uniformly scaled by 2, translated.
    const float c = std::cos(0.7f), s = std::sin(0.7f), k = 2.0f;
    float M[16] = {
        c*k, 0, -s*k, 0,
        0,   k, 0,    0,
        s*k, 0, c*k,  0,
        10,  20, 30,  1};
    float R[16];
    retail_inverse(R, M);
    std::printf("input (rot 0.7 about Y, scale 2, translation 10/20/30)\n");
    show("retail inverse", R);

    // affine candidate: inv[j][i] = M[i][j] / |row i|^2
    float A[16] = {0};
    for (int i = 0; i < 3; ++i) {
        const float len2 = M[i*4]*M[i*4] + M[i*4+1]*M[i*4+1] + M[i*4+2]*M[i*4+2];
        for (int j = 0; j < 3; ++j) A[j*4+i] = M[i*4+j] / len2;
    }
    A[3] = A[7] = A[11] = 0.f; A[15] = 1.f;
    for (int j = 0; j < 3; ++j)
        A[12+j] = -(A[0*4+j]*M[12] + A[1*4+j]*M[13] + A[2*4+j]*M[14]);
    show("affine transpose/|r|^2", A);

    // does retail's answer actually invert the matrix?
    float P[16];
    retail_mul(P, M, R);
    show("M * retail_inverse(M)", P);
    retail_mul(P, R, M);
    show("retail_inverse(M) * M", P);

    // and a non-uniformly scaled, skewed matrix
    float N[16] = {
        1.3f, 0.2f, -0.1f, 0,
        0.05f, 0.7f, 0.4f, 0,
        -0.3f, 0.1f, 1.9f, 0,
        5, -7, 11, 1};
    retail_inverse(R, N);
    std::printf("\ninput (skewed, non-uniform scale)\n");
    show("retail inverse", R);
    retail_mul(P, N, R);
    show("N * retail_inverse(N)", P);
    return 0;
}
