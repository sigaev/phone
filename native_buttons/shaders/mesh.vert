#version 450
layout(set = 0, binding = 0, std140) uniform Globals {
    mat4 u_view;
    mat4 u_light;
    vec4 u_eye_time;
    vec4 u_size;
};
#define u_time u_eye_time.w
#define u_eye u_eye_time.xyz
#define u_shadow_texel u_size.y
#define u_scale u_size.x

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in mat4 a_model;
layout(location = 6) in vec4 a_color;
layout(location = 7) in vec4 a_material;

layout(location = 0) out vec3 v_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec4 v_material;
layout(location = 4) out vec4 v_shadow;
layout(push_constant) uniform Pass {
    vec4 params;
};
void main() {
    vec4 p = a_model * vec4(a_position, 1.);
    if (a_material.w > 3.5 && a_material.w < 4.5) {
        float bend = smoothstep(2.12, 3.12, p.y);
        p.x += .065 * sin(u_time * 1.4) * bend;
        p.z += .035 * sin(u_time * .9) * bend;
    }
    if (a_material.w > 4.5 && a_material.w < 5.5) {
        float t = -a_position.x / .84;
        p.y += .09 * sin(u_time * 5. - t * 6. + p.z * 5.) * t;
        p.z += .04 * sin(u_time * 4. - t * 3.) * t;
    }
    mat3 m = mat3(a_model);
    vec3 s = vec3(dot(m[0], m[0]), dot(m[1], m[1]), dot(m[2], m[2]));
    v_normal = normalize(m * (a_normal / max(s, vec3(.000001))));
    v_position = p.xyz;
    v_color = a_color;
    v_material = a_material;
    v_shadow = u_light * p;
    gl_Position = (params.x > .5 ? u_light : u_view) * p;
}
