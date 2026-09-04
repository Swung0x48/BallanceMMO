// Same entry points, backed by the Ballanced fork's VxMath (the maths the
// headless engine and the BallanceMMO physics plugin actually run).
#include "VxMath.h"

extern "C" {

void fork_mul(float *out, const float *a, const float *b) {
    VxMatrix ma, mb, mo;
    for (int i = 0; i < 16; ++i) { ((float *)&ma)[i] = a[i]; ((float *)&mb)[i] = b[i]; }
    Vx3DMultiplyMatrix(mo, ma, mb);
    for (int i = 0; i < 16; ++i) out[i] = ((float *)&mo)[i];
}

void fork_mul4(float *out, const float *a, const float *b) {
    VxMatrix ma, mb, mo;
    for (int i = 0; i < 16; ++i) { ((float *)&ma)[i] = a[i]; ((float *)&mb)[i] = b[i]; }
    Vx3DMultiplyMatrix4(mo, ma, mb);
    for (int i = 0; i < 16; ++i) out[i] = ((float *)&mo)[i];
}

void fork_inverse(float *out, const float *a) {
    VxMatrix ma, mo;
    for (int i = 0; i < 16; ++i) ((float *)&ma)[i] = a[i];
    Vx3DInverseMatrix(mo, ma);
    for (int i = 0; i < 16; ++i) out[i] = ((float *)&mo)[i];
}

void fork_mulvec(float *out, const float *m, const float *v) {
    VxMatrix ma; VxVector vi, vo;
    for (int i = 0; i < 16; ++i) ((float *)&ma)[i] = m[i];
    for (int i = 0; i < 3; ++i) ((float *)&vi)[i] = v[i];
    Vx3DMultiplyMatrixVector(&vo, ma, &vi);
    for (int i = 0; i < 3; ++i) out[i] = ((float *)&vo)[i];
}

void fork_rotatevec(float *out, const float *m, const float *v) {
    VxMatrix ma; VxVector vi, vo;
    for (int i = 0; i < 16; ++i) ((float *)&ma)[i] = m[i];
    for (int i = 0; i < 3; ++i) ((float *)&vi)[i] = v[i];
    Vx3DRotateVector(&vo, ma, &vi);
    for (int i = 0; i < 3; ++i) out[i] = ((float *)&vo)[i];
}

void fork_quat_from_matrix(float *out, const float *m) {
    VxMatrix ma; VxQuaternion q;
    for (int i = 0; i < 16; ++i) ((float *)&ma)[i] = m[i];
    q.FromMatrix(ma, 1, 1);
    for (int i = 0; i < 4; ++i) out[i] = ((float *)&q)[i];
}

void fork_matrix_from_rotation(float *out, const float *axis, float angle) {
    VxMatrix mo; VxVector ax;
    for (int i = 0; i < 3; ++i) ((float *)&ax)[i] = axis[i];
    Vx3DMatrixFromRotation(mo, ax, angle);
    for (int i = 0; i < 16; ++i) out[i] = ((float *)&mo)[i];
}

void fork_decompose(float *quat, float *pos, float *scale, const float *m) {
    VxMatrix ma; VxQuaternion q; VxVector p, s;
    for (int i = 0; i < 16; ++i) ((float *)&ma)[i] = m[i];
    Vx3DDecomposeMatrix(ma, q, p, s);
    for (int i = 0; i < 4; ++i) quat[i] = ((float *)&q)[i];
    for (int i = 0; i < 3; ++i) { pos[i] = ((float *)&p)[i]; scale[i] = ((float *)&s)[i]; }
}

void fork_normalize(float *out, const float *v) {
    VxVector a;
    for (int i = 0; i < 3; ++i) ((float *)&a)[i] = v[i];
    a.Normalize();
    for (int i = 0; i < 3; ++i) out[i] = ((float *)&a)[i];
}

void fork_interpolate(float *out, float step, const float *a, const float *b) {
    VxMatrix ma, mb, mo;
    for (int i = 0; i < 16; ++i) { ((float *)&ma)[i] = a[i]; ((float *)&mb)[i] = b[i]; }
    mo = ma;
    Vx3DInterpolateMatrix(step, mo, ma, mb);
    for (int i = 0; i < 16; ++i) out[i] = ((float *)&mo)[i];
}

}  // extern "C"
