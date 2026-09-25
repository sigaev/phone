#version 450

layout(location = 0) in vec2 uv;
layout(set = 0, binding = 1) uniform sampler2D u_scene;
layout(set = 0, binding = 2) uniform sampler2D u_bloom;
layout(location = 0) out vec4 frag;

void main() {
  vec3 c = texture(u_scene, uv).rgb + texture(u_bloom, uv).rgb * .20;
  c = c * (2.51 * c + .03) / (c * (2.43 * c + .59) + .14);
  c = pow(clamp(c, 0., 1.), vec3(1. / 2.2));
  c *= 1. - .18 * dot(uv - .5, uv - .5);
  frag = vec4(c, 1.);
}
