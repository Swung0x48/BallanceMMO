// Calls the RETAIL VxMath.dll by address, resolved with GetProcAddress on the
// decorated names.  Loading it dynamically (rather than linking the import
// library) keeps its symbols out of this image, so the fork's identically
// named inline maths can live in the same executable.
//
// VxMatrix is 16 floats, VxVector 3, VxQuaternion 4, and every parameter here
// is a reference or pointer, so raw float arrays are an exact stand-in for the
// real types at the ABI level.
#include <windows.h>
#include <cstdio>

namespace {

HMODULE g_vxmath;

FARPROC need(const char *decorated) {
    FARPROC fn = GetProcAddress(g_vxmath, decorated);
    if (!fn) std::printf("!! retail VxMath.dll does not export %s\n", decorated);
    return fn;
}

typedef void(__cdecl *MatMatMat)(float *, const float *, const float *);
typedef void(__cdecl *MatMat)(float *, const float *);
typedef void(__cdecl *VecMatVec)(float *, const float *, const float *);
typedef void(__cdecl *MatAxisAngle)(float *, const float *, float);
typedef void(__cdecl *Decompose)(const float *, float *, float *, float *);
typedef void(__cdecl *Interpolate)(float, float *, const float *, const float *);

MatMatMat p_mul, p_mul4;
MatMat p_inverse;
VecMatVec p_mulvec, p_rotatevec;
MatAxisAngle p_from_rotation;
Decompose p_decompose;
Interpolate p_interpolate;

// __thiscall members: taking the address through a member-function pointer is
// the only portable way to get MSVC to emit the right calling convention.
struct FakeVector { void Normalize(); };
struct FakeQuat { void FromMatrix(const float *, int, int); };
union NormalizePun { FARPROC raw; void (FakeVector::*fn)(); };
union FromMatrixPun { FARPROC raw; void (FakeQuat::*fn)(const float *, int, int); };
NormalizePun g_normalize;
FromMatrixPun g_from_matrix;

}  // namespace

extern "C" int retail_load(const char *path) {
    g_vxmath = LoadLibraryA(path);
    if (!g_vxmath) {
        std::printf("!! cannot load %s (error %lu)\n", path, GetLastError());
        return 0;
    }
    p_mul = (MatMatMat)need("?Vx3DMultiplyMatrix@@YAXAAVVxMatrix@@ABV1@1@Z");
    p_mul4 = (MatMatMat)need("?Vx3DMultiplyMatrix4@@YAXAAVVxMatrix@@ABV1@1@Z");
    p_inverse = (MatMat)need("?Vx3DInverseMatrix@@YAXAAVVxMatrix@@ABV1@@Z");
    p_mulvec = (VecMatVec)need("?Vx3DMultiplyMatrixVector@@YAXPAUVxVector@@ABVVxMatrix@@PBU1@@Z");
    p_rotatevec = (VecMatVec)need("?Vx3DRotateVector@@YAXPAUVxVector@@ABVVxMatrix@@PBU1@@Z");
    p_from_rotation = (MatAxisAngle)need("?Vx3DMatrixFromRotation@@YAXAAVVxMatrix@@ABUVxVector@@M@Z");
    p_decompose = (Decompose)need("?Vx3DDecomposeMatrix@@YAXABVVxMatrix@@AAUVxQuaternion@@AAUVxVector@@2@Z");
    p_interpolate = (Interpolate)need("?Vx3DInterpolateMatrix@@YAXMAAVVxMatrix@@ABV1@1@Z");
    g_normalize.raw = need("?Normalize@VxVector@@QAEXXZ");
    g_from_matrix.raw = need("?FromMatrix@VxQuaternion@@QAEXABVVxMatrix@@HH@Z");
    return p_mul && p_mul4 && p_inverse && p_mulvec && p_rotatevec && p_from_rotation
           && p_decompose && p_interpolate && g_normalize.raw && g_from_matrix.raw;
}

extern "C" {

void retail_mul(float *out, const float *a, const float *b) { p_mul(out, a, b); }
void retail_mul4(float *out, const float *a, const float *b) { p_mul4(out, a, b); }
void retail_inverse(float *out, const float *a) { p_inverse(out, a); }
void retail_mulvec(float *out, const float *m, const float *v) { p_mulvec(out, m, v); }
void retail_rotatevec(float *out, const float *m, const float *v) { p_rotatevec(out, m, v); }
void retail_matrix_from_rotation(float *out, const float *axis, float angle) {
    p_from_rotation(out, axis, angle);
}
void retail_decompose(float *quat, float *pos, float *scale, const float *m) {
    p_decompose(m, quat, pos, scale);
}
void retail_interpolate(float *out, float step, const float *a, const float *b) {
    for (int i = 0; i < 16; ++i) out[i] = a[i];
    p_interpolate(step, out, a, b);
}

void retail_normalize(float *out, const float *v) {
    float local[3] = {v[0], v[1], v[2]};
    (((FakeVector *)local)->*g_normalize.fn)();
    for (int i = 0; i < 3; ++i) out[i] = local[i];
}

// the retail default arguments are (MatIsUnit = TRUE, RestoreMat = TRUE)
void retail_quat_from_matrix(float *out, const float *m) {
    float q[4] = {0, 0, 0, 1};
    (((FakeQuat *)q)->*g_from_matrix.fn)(m, 1, 1);
    for (int i = 0; i < 4; ++i) out[i] = q[i];
}

}  // extern "C"
