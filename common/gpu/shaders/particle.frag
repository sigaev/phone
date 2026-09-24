#version 450

layout(location = 0) in float intensity;
layout(location = 0) out vec4 frag;
void main() {
    float r = length(gl_PointCoord - .5) * 2.;
    frag = vec4(.38, .8, 1., intensity * (1. - smoothstep(.05, 1., r)));
}
