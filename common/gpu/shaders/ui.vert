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

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
void main() {
    uv = a_uv;
    color = a_color;
    gl_Position = vec4(a_position / u_size.zw * 2. - 1., 0., 1.);
}
