#version 450

layout(push_constant) uniform Constants {
  vec4 u_parameters;
  mat4 u_view;
  mat4 u_light;
  vec4 u_eye_time;
  vec4 u_size;
  vec4 u_animation_clock;
};

layout(set = 0, binding = 0) uniform sampler2D wallpaper;
layout(location = 0) out vec4 frag;

void main() {
  vec2 image_size = u_size.xy, screen = u_size.zw;
  vec2 p = (gl_FragCoord.xy - screen * .5) / min(screen.x, screen.y);
  float t = u_eye_time.w;
  // Gentle, overlapping swells take 14-27 seconds to travel a full cycle.
  // Rates are multiples of .01, so the bounded CPU clock wraps seamlessly.
  vec2 slope = vec2(.85, .30) * cos(dot(p, vec2(3.2, 10.4)) + .45 * t) +
               vec2(-.35, .75) * cos(dot(p, vec2(-5.8, 7.1)) - .31 * t + 1.4) +
               vec2(.18, .30) * cos(dot(p, vec2(9.5, 5.6)) + .23 * t + 2.7);
  vec2 displacement = slope * min(screen.x, screen.y) * .006;
  // Center-crop with a small overscan to keep every displaced sample in the image.
  float scale = max(screen.x / image_size.x, screen.y / image_size.y) * 1.035;
  vec2 uv = (gl_FragCoord.xy + displacement - screen * .5) / (image_size * scale) + .5;
  // Preserve the image's colors and premultiplied alpha without any screen-wide tint.
  frag = texture(wallpaper, uv);
}
