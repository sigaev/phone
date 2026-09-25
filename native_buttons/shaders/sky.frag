#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;

void main() {
  vec3 c = mix(vec3(.065, .24, .28), vec3(.012, .025, .065), smoothstep(.2, .95, (1. - uv.y)));
  c += vec3(.28, .12, .08) *
       exp(-length((vec2(uv.x, 1. - uv.y) - vec2(.12, .52)) * vec2(1.4, 1.)) * 5.);
  frag = vec4(c, 1.);
}
