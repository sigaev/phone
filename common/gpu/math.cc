#include "common/gpu/math.h"

#include <cmath>
#include <numbers>

namespace gpu {
Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, float scale) {
    return {a.x * scale, a.y * scale, a.z * scale};
}
float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(Vec3 value) {
    return std::sqrt(dot(value, value));
}
Vec3 unit(Vec3 value) {
    return value * (1.f / std::fmax(length(value), .00001f));
}
bool contains(Rect rect, float x, float y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}
float wrap(float value, float period) {
    float result = std::fmod(value, period);
    if (result < 0)
        result += period;
    return result < period ? result : 0.f;
}
double wrap(double value, double period) {
    double result = std::fmod(value, period);
    if (result < 0)
        result += period;
    return result < period ? result : 0.;
}
float oscillation_time(double seconds) {
    return static_cast<float>(wrap(seconds, 200 * std::numbers::pi));
}
Mat4 translation(Vec3 p) {
    Mat4 m;
    m.v[12] = p.x;
    m.v[13] = p.y;
    m.v[14] = p.z;
    return m;
}
Mat4 scale(Vec3 s) {
    Mat4 m;
    m.v[0] = s.x;
    m.v[5] = s.y;
    m.v[10] = s.z;
    return m;
}
Mat4 rotate_x(float a) {
    Mat4 m;
    float c = std::cos(a), s = std::sin(a);
    m.v[5] = c;
    m.v[6] = s;
    m.v[9] = -s;
    m.v[10] = c;
    return m;
}
Mat4 rotate_y(float a) {
    Mat4 m;
    float c = std::cos(a), s = std::sin(a);
    m.v[0] = c;
    m.v[2] = -s;
    m.v[8] = s;
    m.v[10] = c;
    return m;
}
Mat4 rotate_z(float a) {
    Mat4 m;
    float c = std::cos(a), s = std::sin(a);
    m.v[0] = c;
    m.v[1] = s;
    m.v[4] = -s;
    m.v[5] = c;
    return m;
}
Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            m.v[c * 4 + r] = 0;
            for (int k = 0; k < 4; ++k)
                m.v[c * 4 + r] += a.v[k * 4 + r] * b.v[c * 4 + k];
        }
    return m;
}
Mat4 transform(Vec3 p, Vec3 s, float roll) {
    return translation(p) * rotate_z(roll) * scale(s);
}
Mat4 between(Vec3 a, Vec3 b, float radius) {
    Vec3 y = b - a, n = unit(y),
         x = unit(cross(n, std::fabs(n.y) > .98f ? Vec3{1, 0, 0} : Vec3{0, 1, 0})), z = cross(x, n),
         p = (a + b) * .5f;
    Mat4 m;
    m.v[0] = x.x * radius;
    m.v[1] = x.y * radius;
    m.v[2] = x.z * radius;
    m.v[4] = y.x;
    m.v[5] = y.y;
    m.v[6] = y.z;
    m.v[8] = z.x * radius;
    m.v[9] = z.y * radius;
    m.v[10] = z.z * radius;
    m.v[12] = p.x;
    m.v[13] = p.y;
    m.v[14] = p.z;
    return m;
}
Mat4 perspective(float fov, float aspect, float near, float far) {
    Mat4 m;
    for (float& value : m.v)
        value = 0;
    float f = 1 / std::tan(fov * .5f);
    m.v[0] = f / aspect;
    m.v[5] = -f;
    m.v[10] = far / (near - far);
    m.v[11] = -1;
    m.v[14] = far * near / (near - far);
    return m;
}
Mat4 ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m;
    m.v[0] = 2 / (r - l);
    m.v[5] = -2 / (t - b);
    m.v[10] = -1 / (f - n);
    m.v[12] = -(r + l) / (r - l);
    m.v[13] = (t + b) / (t - b);
    m.v[14] = -n / (f - n);
    return m;
}
Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = unit(center - eye), s = unit(cross(f, up)), u = cross(s, f);
    Mat4 m;
    m.v[0] = s.x;
    m.v[4] = s.y;
    m.v[8] = s.z;
    m.v[1] = u.x;
    m.v[5] = u.y;
    m.v[9] = u.z;
    m.v[2] = -f.x;
    m.v[6] = -f.y;
    m.v[10] = -f.z;
    m.v[12] = -dot(s, eye);
    m.v[13] = -dot(u, eye);
    m.v[14] = dot(f, eye);
    return m;
}
}  // namespace gpu
