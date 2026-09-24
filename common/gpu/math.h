#pragma once

namespace gpu {
inline constexpr float kPi = 3.14159265358979323846f;
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Color {
    float r, g, b, a = 1;
};
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
struct Mat4 {
    float v[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, float scale);
Mat4 operator*(const Mat4& a, const Mat4& b);
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 value);
Vec3 unit(Vec3 value);
bool contains(Rect rect, float x, float y);
Mat4 translation(Vec3 position);
Mat4 scale(Vec3 size);
Mat4 rotate_x(float angle);
Mat4 rotate_y(float angle);
Mat4 rotate_z(float angle);
Mat4 transform(Vec3 position, Vec3 size, float roll = 0);
Mat4 between(Vec3 a, Vec3 b, float radius);
Mat4 perspective(float fov, float aspect, float near, float far);
Mat4 ortho(float left, float right, float bottom, float top, float near, float far);
Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up = {0, 1, 0});
}  // namespace gpu
