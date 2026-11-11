#pragma once

#include <cmath>

// Minimal vector and matrix maths for the map. Matrices follow the row-vector convention (v' = v * M), the same
// as DirectXMath, and are uploaded as HLSL `row_major`.
namespace mc
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
    inline Vec3 normalize(Vec3 a) { return a * (1.0f / length(a)); }

    struct Mat4
    {
        float m[4][4] = {};

        static Mat4 identity()
        {
            Mat4 r;
            for (int i = 0; i < 4; ++i) {
                r.m[i][i] = 1.0f;
            }
            return r;
        }
    };

    inline Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        Mat4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
            }
        }
        return r;
    }

    // Left-handed look-at, as XMMatrixLookAtLH.
    inline Mat4 look_at_lh(Vec3 eye, Vec3 target, Vec3 up)
    {
        const Vec3 z = normalize(target - eye);
        const Vec3 x = normalize(cross(up, z));
        const Vec3 y = cross(z, x);
        Mat4 r;
        r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
        r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
        r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
        r.m[3][0] = -dot(x, eye); r.m[3][1] = -dot(y, eye); r.m[3][2] = -dot(z, eye);
        r.m[3][3] = 1.0f;
        return r;
    }

    // Left-handed perspective with depth 0 at `near_z` and 1 at `far_z`, as XMMatrixPerspectiveFovLH.
    inline Mat4 perspective_lh(float fov_y, float aspect, float near_z, float far_z)
    {
        const float ys = 1.0f / std::tan(fov_y * 0.5f);
        const float range = far_z / (far_z - near_z);
        Mat4 r;
        r.m[0][0] = ys / aspect;
        r.m[1][1] = ys;
        r.m[2][2] = range;
        r.m[2][3] = 1.0f;
        r.m[3][2] = -range * near_z;
        return r;
    }
}
