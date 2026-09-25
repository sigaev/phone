#version 450
layout(set = 0, binding = 0, std140) uniform Globals {
    mat4 u_view;
    mat4 u_light;
    vec4 u_eye_time;
    vec4 u_size;
    vec4 u_animation_clock;
};
#define u_time u_eye_time.w
#define u_eye u_eye_time.xyz
#define u_shadow_texel u_size.y
#define u_scale u_size.x

layout(location = 0) in vec4 a_point;

layout(location = 0) out float intensity;
void main() {
    vec4 p = a_point;
    gl_Position = u_view * vec4(p.xyz, 1.);
    gl_PointSize = clamp((9. + p.w * 12.) * u_scale / gl_Position.w, 1., 5.);
    intensity = .04 + p.w * .1;
}
