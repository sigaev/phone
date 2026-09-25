#version 450

layout(location = 0) in vec2 uv;
layout(set = 0, binding = 1) uniform sampler2D u_image;

layout(push_constant) uniform Pass { vec4 params; };

#define u_direction params.xy
#define u_extract int(params.z)
layout(location = 0) out vec4 frag;

vec3 sample_at(vec2 p) {
  vec3 c = texture(u_image, p).rgb;
  return u_extract == 1 ? c * smoothstep(.8, 1.6, max(c.r, max(c.g, c.b))) : c;
}

void main() {
  vec3 c = sample_at(uv) * .227027;
  c += (sample_at(uv + u_direction * 1.384615) + sample_at(uv - u_direction * 1.384615)) * .316216;
  c += (sample_at(uv + u_direction * 3.230769) + sample_at(uv - u_direction * 3.230769)) * .070270;
  frag = vec4(c, 1.);
}
