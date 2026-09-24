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

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec4 v_material;
layout(location = 4) in vec4 v_shadow;
layout(set = 0, binding = 1) uniform sampler2DShadow u_shadow;

layout(location = 0) out vec4 frag;
const vec3 kSun = normalize(vec3(-.5, .85, .45));
const float kPi = 3.14159265;
vec3 environment(vec3 d) {
    vec3 c = mix(vec3(.06, .21, .25), vec3(.12, .17, .34), smoothstep(-.1, .8, d.y));
    c += vec3(2., 1.4, .72) * pow(max(dot(d, kSun), 0.), 120.);
    return c;
}
float shadow(vec3 n) {
    vec3 p = v_shadow.xyz / v_shadow.w;
    p.xy = p.xy * .5 + .5;
    if (any(lessThan(p, vec3(0.))) || any(greaterThan(p, vec3(1.))))
        return 1.;
    float bias = max(.0003, .0016 * (1. - dot(n, kSun))), s = 0.;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            s += texture(u_shadow, vec3(p.xy + vec2(x, y) * u_shadow_texel, p.z - bias));
    return s / 9.;
}
void main() {
    vec3 n = normalize(v_normal), base = v_color.rgb;
    float rough = clamp(v_material.x, .06, 1.), metal = v_material.y;
    if (v_material.w > .5 && v_material.w < 1.5) {
        vec2 p = v_position.xz;
        vec2 slope = vec2(0.);
        for (int k = 0; k < 8; ++k) {
            float f = float(k) + 1., a = f * 2.399;
            vec2 d = vec2(cos(a), sin(a));
            slope += d * cos(dot(p, d) * (f * .72) + u_time * (.7 + f * .23)) * (.16 / f);
        }
        n = normalize(vec3(slope.x, 1., slope.y));
        base = mix(vec3(.012, .13, .17), vec3(.025, .32, .34), .5 + .5 * sin(p.x * .7 + p.y * .4));
        rough = .14;
        metal = .58;
    } else if (v_material.w > 1.5 && v_material.w < 2.5) {
        float lane = (1. - smoothstep(.025, .045, abs(v_position.z))) *
                     step(.32, fract((v_position.x + u_time * 2.6) * .35));
        float edge = 1. - smoothstep(.013, .027, abs(abs(v_position.z) - .88));
        base = mix(base, vec3(.93, .74, .34), max(lane, edge * .7));
    } else if (v_material.w > 2.5) {
        float grain = sin((v_position.y + v_position.x * .12) * 420.);
        base *= .975 + .025 * grain;
        rough = max(rough, .56);
    }
    vec3 view = normalize(u_eye - v_position), halfway = normalize(kSun + view);
    float nl = max(dot(n, kSun), 0.), nv = max(dot(n, view), .001), nh = max(dot(n, halfway), 0.);
    float alpha = rough * rough, a2 = alpha * alpha, denom = nh * nh * (a2 - 1.) + 1.;
    float distribution = a2 / (kPi * denom * denom + .00001);
    float k = (rough + 1.) * (rough + 1.) / 8.;
    float visibility = (nl / (nl * (1. - k) + k)) * (nv / (nv * (1. - k) + k));
    vec3 f0 = mix(vec3(.04), base, metal);
    vec3 fresnel = f0 + (1. - f0) * pow(1. - max(dot(halfway, view), 0.), 5.);
    vec3 spec = distribution * visibility * fresnel / (4. * nl * nv + .0001);
    float shade = shadow(n);
    vec3 diffuse = base * (1. - metal) / kPi;
    vec3 color = (diffuse + spec) * vec3(4.4, 3.6, 2.8) * nl * shade;
    vec3 ambient = mix(vec3(.10, .15, .22), vec3(.36, .52, .66), n.y * .5 + .5);
    color += base * ambient * (1. - metal * .65);
    vec3 reflected = reflect(-view, n);
    vec3 f = f0 + (1. - f0) * pow(1. - nv, 5.);
    color += environment(reflected) * f * (1. - rough * .65);
    color += base * v_material.z;
    float fog = 1. - exp(-length(u_eye - v_position) * .012);
    color = mix(color, vec3(.06, .15, .24), fog);
    frag = vec4(color, 1.);
}
