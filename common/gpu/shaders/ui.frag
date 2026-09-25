#version 450

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(set = 0, binding = 1) uniform sampler2D u_font;
layout(location = 0) out vec4 frag;

void main() { frag = vec4(color.rgb, color.a * texture(u_font, uv).r); }
