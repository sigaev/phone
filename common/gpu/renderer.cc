#include "common/gpu/renderer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace gpu {

using common::Error;
using common::Owner;
using common::Result;
struct Instance {
    Mat4 model;
    Color color;
    float material[4];
};
struct Mesh {
    GLuint vao = 0, vbo = 0, ebo = 0, instances = 0;
    GLsizei indices = 0;
    std::vector<Instance> items;
};
struct UiVertex {
    float x, y, u, v;
    Color color;
};
struct Glyph {
    float x0, y0, x1, y1, xoff, yoff, advance;
};
struct Renderer {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool offscreen = false, maximum = false, timer_supported = false;
    int width = 0, height = 0, render_width = 0, render_height = 0, samples = 0, shadow_size = 0,
        particle_count = 0;
    GLuint scene_program = 0, shadow_program = 0, sky_program = 0, blur_program = 0,
           post_program = 0;
    GLuint particle_program = 0, compute_program = 0, ui_program = 0, empty_vao = 0;
    GLuint hdr_fbo = 0, hdr_tex = 0, ms_fbo = 0, ms_color = 0, ms_depth = 0, shadow_fbo = 0,
           shadow_tex = 0;
    GLuint bloom_fbo[2]{}, bloom_tex[2]{}, particle_buffer = 0, particle_vao = 0, ui_vao = 0,
                                           ui_vbo = 0, font_tex = 0;
    GLuint queries[4]{};
    bool query_pending[4]{};
    unsigned query_index = 0, triangle_count = 0;
    float gpu_millis = 0;
    PFNGLGETQUERYOBJECTUI64VEXTPROC query_result = nullptr;
    std::vector<Mesh> meshes;
    Glyph glyphs[96]{};
    std::vector<UiVertex> ui;
    char device[128]{}, error[1024]{};
    Mat4 model_transform;
};
void close_renderer(Renderer& renderer) noexcept;
Result<void> ensure_targets(Renderer& renderer, bool maximum);
Result<void> create_font(Renderer& renderer);

namespace {
constexpr int kAtlasSize = 1024;
constexpr char kShadowFragment[] = R"GLSL(
#version 320 es
    precision highp float;
    void main() {
    }
)GLSL";
constexpr char kFullVertex[] = R"GLSL(
#version 320 es
    precision highp float;
    out vec2 uv;
    void main() {
        vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
        uv = p;
        gl_Position = vec4(p * 2. - 1., 0., 1.);
    }
)GLSL";
constexpr char kBlurFragment[] = R"GLSL(
#version 320 es
    precision highp float;
    in vec2 uv;
    uniform sampler2D u_image;
    uniform vec2 u_direction;
    uniform int u_extract;
    out vec4 frag;
    vec3 sample_at(vec2 p) {
        vec3 c = texture(u_image, p).rgb;
        return u_extract == 1 ? c * smoothstep(.8, 1.6, max(c.r, max(c.g, c.b))) : c;
    }
    void main() {
        vec3 c = sample_at(uv) * .227027;
        c += (sample_at(uv + u_direction * 1.384615) + sample_at(uv - u_direction * 1.384615)) *
             .316216;
        c += (sample_at(uv + u_direction * 3.230769) + sample_at(uv - u_direction * 3.230769)) *
             .070270;
        frag = vec4(c, 1.);
    }
)GLSL";
constexpr char kPostFragment[] = R"GLSL(
#version 320 es
    precision highp float;
    in vec2 uv;
    uniform sampler2D u_scene, u_bloom;
    out vec4 frag;
    void main() {
        vec3 c = texture(u_scene, uv).rgb + texture(u_bloom, uv).rgb * .20;
        c = c * (2.51 * c + .03) / (c * (2.43 * c + .59) + .14);
        c = pow(clamp(c, 0., 1.), vec3(1. / 2.2));
        c *= 1. - .18 * dot(uv - .5, uv - .5);
        frag = vec4(c, 1.);
    }
)GLSL";
constexpr char kParticlesCompute[] =
    R"GLSL(
#version 320 es
    precision highp float;
    layout(local_size_x = 128) in;
    layout(std430, binding = 0) buffer Particles {
        vec4 points[];
    };
    uniform float u_time;
    uint hash(uint x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        return x ^ (x >> 16);
    }
    float rnd(uint x) {
        return float(hash(x) & 0x00ffffffu) / 16777215.;
    }
    void main() {
        uint i = gl_GlobalInvocationID.x;
        vec3 seed = vec3(rnd(i * 3u + 1u), rnd(i * 3u + 2u), rnd(i * 3u + 3u));
        vec3 p = vec3(mod(seed.x * 38. - u_time * (.12 + seed.z * .24), 38.) - 19.,
                      seed.y * 5.8 + .25, -3. - seed.z * 17.);
        p.y += sin(u_time * .65 + seed.x * 40.) * .18;
        p.z += sin(u_time * .35 + seed.y * 20.) * .20;
        points[i] = vec4(p, seed.x);
    }
    )GLSL";
constexpr char kParticleVertex[] = R"GLSL(
#version 320 es
    precision highp float;
    layout(location = 0) in vec4 a_point;
    uniform mat4 u_view;
    uniform float u_scale;
    out float intensity;
    void main() {
        vec4 p = a_point;
        gl_Position = u_view * vec4(p.xyz, 1.);
        gl_PointSize = clamp((9. + p.w * 12.) * u_scale / gl_Position.w, 1., 5.);
        intensity = .04 + p.w * .1;
    }
)GLSL";
constexpr char kParticleFragment[] = R"GLSL(
#version 320 es
    precision highp float;
    in float intensity;
    out vec4 frag;
    void main() {
        float r = length(gl_PointCoord - .5) * 2.;
        frag = vec4(.38, .8, 1., intensity * (1. - smoothstep(.05, 1., r)));
    }
)GLSL";
constexpr char kUiVertex[] = R"GLSL(
#version 320 es
    precision highp float;
    layout(location = 0) in vec2 a_position;
    layout(location = 1) in vec2 a_uv;
    layout(location = 2) in vec4 a_color;
    uniform vec2 u_size;
    out vec2 uv;
    out vec4 color;
    void main() {
        uv = a_uv;
        color = a_color;
        gl_Position = vec4(a_position / u_size * vec2(2., -2.) + vec2(-1., 1.), 0., 1.);
    }
)GLSL";
constexpr char kUiFragment[] = R"GLSL(
#version 320 es
    precision highp float;
    in vec2 uv;
    in vec4 color;
    uniform sampler2D u_font;
    out vec4 frag;
    void main() {
        frag = vec4(color.rgb, color.a * texture(u_font, uv).r);
    }
)GLSL";

void tex_parameters() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
void matrix(GLuint p, const char* name, const Mat4& m) {
    glUniformMatrix4fv(glGetUniformLocation(p, name), 1, GL_FALSE, m.v);
}
}  // namespace

std::unexpected<Error> fail(Renderer& renderer, const char* message) {
    if (message != renderer.error)
        std::snprintf(renderer.error, sizeof(renderer.error), "%s", message);
    __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "GPU: %s", renderer.error);
    return std::unexpected(Error{renderer.error});
}

Result<GLuint> create_program(Renderer& renderer, const char* vertex, const char* fragment,
                              const char* compute) {
    GLuint p = glCreateProgram();
    const char* sources[] = {vertex, fragment, compute};
    GLenum types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER};
    for (int i = 0; i < 3; ++i)
        if (sources[i]) {
            GLuint s = glCreateShader(types[i]);
            // Multiline raw strings start with a newline. This driver requires
            // #version on the first line, even before otherwise legal whitespace.
            const char* source = sources[i];
            source += std::strspn(source, " \t\r\n");
            glShaderSource(s, 1, &source, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                glGetShaderInfoLog(s, sizeof(renderer.error), nullptr, renderer.error);
                glDeleteShader(s);
                glDeleteProgram(p);
                return fail(renderer, renderer.error);
            }
            glAttachShader(p, s);
            glDeleteShader(s);
        }
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof(renderer.error), nullptr, renderer.error);
        glDeleteProgram(p);
        return fail(renderer, renderer.error);
    }
    return p;
}

Result<void> initialize_renderer(Renderer& renderer, ANativeWindow* window, SceneShaders shaders,
                                 int offscreen_width, int offscreen_height) {
    close_renderer(renderer);
    renderer.error[0] = 0;
    renderer.offscreen = !window;
    renderer.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (renderer.display == EGL_NO_DISPLAY || !eglInitialize(renderer.display, nullptr, nullptr))
        return fail(renderer, "Cannot initialize EGL");
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT_KHR,
                            EGL_SURFACE_TYPE,
                            window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
                            EGL_RED_SIZE,
                            8,
                            EGL_GREEN_SIZE,
                            8,
                            EGL_BLUE_SIZE,
                            8,
                            EGL_ALPHA_SIZE,
                            8,
                            EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(renderer.display, attrs, &config, 1, &count) || !count)
        return fail(renderer, "No GLES 3 EGL configuration");
    EGLint visual;
    eglGetConfigAttrib(renderer.display, config, EGL_NATIVE_VISUAL_ID, &visual);
    if (window)
        ANativeWindow_setBuffersGeometry(window, 0, 0, visual);
    const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR,
                                    2, EGL_NONE};
    renderer.context = eglCreateContext(renderer.display, config, EGL_NO_CONTEXT, context_attrs);
    if (renderer.context == EGL_NO_CONTEXT)
        return fail(renderer, "This scene requires hardware OpenGL ES 3.2");
    if (window)
        renderer.surface = eglCreateWindowSurface(renderer.display, config, window, nullptr);
    else {
        const EGLint pb[] = {EGL_WIDTH, offscreen_width, EGL_HEIGHT, offscreen_height, EGL_NONE};
        renderer.surface = eglCreatePbufferSurface(renderer.display, config, pb);
    }
    if (renderer.surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(renderer.display, renderer.surface, renderer.surface, renderer.context))
        return fail(renderer, "Cannot attach the GPU surface");
    eglSwapInterval(renderer.display, 1);
    eglQuerySurface(renderer.display, renderer.surface, EGL_WIDTH, &renderer.width);
    eglQuerySurface(renderer.display, renderer.surface, EGL_HEIGHT, &renderer.height);
    const char* renderer_name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::snprintf(renderer.device, sizeof(renderer.device), "%s",
                  renderer_name ? renderer_name : "Unknown renderer");
    if (std::strstr(renderer.device, "SwiftShader") || std::strstr(renderer.device, "llvmpipe") ||
        std::strstr(renderer.device, "Software"))
        return fail(renderer, "Software rendering is not supported");
    __android_log_print(ANDROID_LOG_INFO, "native_buttons", "GPU %s; %s", renderer.device,
                        glGetString(GL_VERSION));
    {
        auto compiled = create_program(renderer, shaders.vertex, shaders.fragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.scene_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, shaders.vertex, kShadowFragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.shadow_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, kFullVertex, shaders.sky, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.sky_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, kFullVertex, kBlurFragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.blur_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, kFullVertex, kPostFragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.post_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, kParticleVertex, kParticleFragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.particle_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, nullptr, nullptr, kParticlesCompute);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.compute_program = *compiled;
    }
    {
        auto compiled = create_program(renderer, kUiVertex, kUiFragment, nullptr);
        if (!compiled)
            return std::unexpected(compiled.error());
        renderer.ui_program = *compiled;
    }
    glGenVertexArrays(1, &renderer.empty_vao);
    for (int type = 0; type < static_cast<int>(Shape::kCount); ++type) {
        std::vector<Vertex> vertices;
        std::vector<unsigned> indices;
        int rows = 40, cols = 64;
        Shape shape = static_cast<Shape>(type);
        if (shape == Shape::kLowSphere) {
            rows = 12;
            cols = 16;
        }
        if (shape == Shape::kFeather) {
            rows = 16;
            cols = 8;
        }
        if (shape == Shape::kTorus)
            rows = 20;
        if (shape == Shape::kCylinder || shape == Shape::kCone)
            rows = 1;
        if (shape == Shape::kPlane) {
            rows = 1;
            cols = 1;
        }
        for (int i = 0; i <= rows; ++i)
            for (int j = 0; j <= cols; ++j) {
                float u = float(j) / cols, v = float(i) / rows, a = u * 2 * kPi, b = v * kPi;
                Vertex vert;
                if (shape == Shape::kSphere || shape == Shape::kLowSphere) {
                    vert.normal = {std::sin(b) * std::cos(a), std::cos(b),
                                   std::sin(b) * std::sin(a)};
                    vert.position = vert.normal;
                } else if (shape == Shape::kFeather) {
                    float x = (u * 2 - 1),
                          profile = std::pow(std::max(0.f, std::sin(v * kPi)), .72f);
                    vert.position = {x * profile, v * 2 - 1,
                                     .16f * (1 - x * x) * profile + .20f * v * v};
                    vert.normal = unit({x * .32f, .08f - v * .20f, 1});
                } else if (shape == Shape::kTorus) {
                    b = v * 2 * kPi;
                    vert.normal = {std::cos(b) * std::cos(a), std::cos(b) * std::sin(a),
                                   std::sin(b)};
                    vert.position = {std::cos(a) * (1 + .075f * std::cos(b)),
                                     std::sin(a) * (1 + .075f * std::cos(b)), .075f * std::sin(b)};
                } else if (shape == Shape::kPlane) {
                    vert.position = {u * 2 - 1, 0, v * 2 - 1};
                    vert.normal = {0, 1, 0};
                } else {
                    float radius = shape == Shape::kCone ? 1 - v : 1;
                    vert.position = {std::cos(a) * radius, v - .5f, std::sin(a) * radius};
                    vert.normal =
                        unit({std::cos(a), shape == Shape::kCone ? 1.f : 0.f, std::sin(a)});
                }
                vertices.push_back(vert);
            }
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                unsigned k = i * (cols + 1) + j;
                indices.insert(indices.end(), {k, k + unsigned(cols) + 1, k + 1, k + 1,
                                               k + unsigned(cols) + 1, k + unsigned(cols) + 2});
            }
        if (shape == Shape::kCylinder || shape == Shape::kCone)
            for (int end = 0; end < 2; ++end) {
                if (shape == Shape::kCone && end == 1)
                    continue;
                unsigned center = vertices.size();
                vertices.push_back({{0, end - .5f, 0}, {0, end ? 1.f : -1.f, 0}});
                for (int j = 0; j <= cols; ++j) {
                    float a = float(j) / cols * 2 * kPi;
                    vertices.push_back(
                        {{std::cos(a), end - .5f, std::sin(a)}, {0, end ? 1.f : -1.f, 0}});
                }
                for (int j = 0; j < cols; ++j)
                    indices.insert(indices.end(),
                                   {center, center + 1 + unsigned(j), center + 2 + unsigned(j)});
            }
        auto uploaded = create_mesh(renderer, vertices, indices);
        if (!uploaded)
            return std::unexpected(uploaded.error());
    }
    glGenBuffers(1, &renderer.particle_buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer.particle_buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 65536 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &renderer.particle_vao);
    glBindVertexArray(renderer.particle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.particle_buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glGenVertexArrays(1, &renderer.ui_vao);
    glBindVertexArray(renderer.ui_vao);
    glGenBuffers(1, &renderer.ui_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.ui_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                          reinterpret_cast<void*>(4 * sizeof(float)));
    renderer.ui.reserve(12000);
    if (auto result = create_font(renderer); !result)
        return std::unexpected(result.error());
    GLint extension_count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
    for (GLint i = 0; i < extension_count; ++i)
        if (!std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)),
                         "GL_EXT_disjoint_timer_query"))
            renderer.timer_supported = true;
    renderer.query_result = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(
        eglGetProcAddress("glGetQueryObjectui64vEXT"));
    renderer.timer_supported = renderer.timer_supported && renderer.query_result;
    if (renderer.timer_supported)
        glGenQueries(4, renderer.queries);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    return ensure_targets(renderer, false);
}

Result<void> create_font(Renderer& renderer) {
    const char* paths[] = {"/system/fonts/RobotoStatic-Regular.ttf",
                           "/system/fonts/Roboto-Regular.ttf"};
    std::vector<unsigned char> data;
    for (const char* path : paths) {
        FILE* f = std::fopen(path, "rb");
        if (!f)
            continue;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::rewind(f);
        if (n > 0 && n < 16000000) {
            data.resize(n);
            if (std::fread(data.data(), 1, n, f) != size_t(n))
                data.clear();
        }
        std::fclose(f);
        if (!data.empty())
            break;
    }
    if (data.empty())
        return fail(renderer, "Cannot load the system font");
    std::vector<unsigned char> bitmap(kAtlasSize * kAtlasSize);
    stbtt_bakedchar baked[96];
    if (stbtt_BakeFontBitmap(data.data(), 0, 48, bitmap.data(), kAtlasSize, kAtlasSize, 32, 96,
                             baked) <= 0)
        return fail(renderer, "Font atlas overflow");
    for (int i = 0; i < 96; ++i) {
        auto& b = baked[i];
        renderer.glyphs[i] = {float(b.x0), float(b.y0), float(b.x1), float(b.y1),
                              b.xoff,      b.yoff,      b.xadvance};
    }
    // A white texel is shared by the solid UI geometry.
    bitmap[0] = bitmap[1] = bitmap[kAtlasSize] = bitmap[kAtlasSize + 1] = 255;
    glGenTextures(1, &renderer.font_tex);
    glBindTexture(GL_TEXTURE_2D, renderer.font_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasSize, kAtlasSize, 0, GL_RED, GL_UNSIGNED_BYTE,
                 bitmap.data());
    tex_parameters();
    return {};
}

void destroy_targets(Renderer& renderer) {
    glDeleteFramebuffers(1, &renderer.hdr_fbo);
    glDeleteFramebuffers(1, &renderer.ms_fbo);
    glDeleteFramebuffers(1, &renderer.shadow_fbo);
    glDeleteFramebuffers(2, renderer.bloom_fbo);
    glDeleteTextures(1, &renderer.hdr_tex);
    glDeleteTextures(1, &renderer.shadow_tex);
    glDeleteTextures(2, renderer.bloom_tex);
    glDeleteRenderbuffers(1, &renderer.ms_color);
    glDeleteRenderbuffers(1, &renderer.ms_depth);
    renderer.hdr_fbo = renderer.ms_fbo = renderer.shadow_fbo = renderer.hdr_tex =
        renderer.shadow_tex = renderer.ms_color = renderer.ms_depth = 0;
    for (int i = 0; i < 2; ++i)
        renderer.bloom_fbo[i] = renderer.bloom_tex[i] = 0;
    renderer.render_width = renderer.render_height = 0;
}

Result<void> ensure_targets(Renderer& renderer, bool maximum) {
    int w = 0, h = 0;
    eglQuerySurface(renderer.display, renderer.surface, EGL_WIDTH, &w);
    eglQuerySurface(renderer.display, renderer.surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0)
        return fail(renderer, "GPU surface has no size");
    if (w == renderer.width && h == renderer.height && maximum == renderer.maximum &&
        renderer.hdr_fbo)
        return {};
    destroy_targets(renderer);
    renderer.width = w;
    renderer.height = h;
    renderer.maximum = maximum;
    float scale = maximum ? 1.30f : 1.f;
    renderer.render_width = int(w * scale);
    renderer.render_height = int(h * scale);
    renderer.shadow_size = maximum ? 4096 : 2048;
    renderer.particle_count = maximum ? 65536 : 16384;
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    if (renderer.render_width > max_size || renderer.render_height > max_size)
        return fail(renderer, "Requested render size exceeds the GPU limit");
    GLint supported = 0;
    glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA16F, GL_NUM_SAMPLE_COUNTS, 1, &supported);
    std::vector<GLint> counts(std::max(supported, 1));
    if (supported)
        glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA16F, GL_SAMPLES, supported, counts.data());
    renderer.samples = 1;
    for (GLint n : counts)
        if (n <= 4)
            renderer.samples = std::max(renderer.samples, n);
    glGenTextures(1, &renderer.hdr_tex);
    glBindTexture(GL_TEXTURE_2D, renderer.hdr_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, renderer.render_width, renderer.render_height);
    tex_parameters();
    glGenFramebuffers(1, &renderer.hdr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.hdr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderer.hdr_tex,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(renderer, "GPU does not support HDR render targets");
    glGenFramebuffers(1, &renderer.ms_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.ms_fbo);
    glGenRenderbuffers(1, &renderer.ms_color);
    glBindRenderbuffer(GL_RENDERBUFFER, renderer.ms_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, renderer.samples, GL_RGBA16F,
                                     renderer.render_width, renderer.render_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              renderer.ms_color);
    glGenRenderbuffers(1, &renderer.ms_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, renderer.ms_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, renderer.samples, GL_DEPTH_COMPONENT24,
                                     renderer.render_width, renderer.render_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              renderer.ms_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(renderer, "GPU does not support the HDR/MSAA configuration");
    glGenTextures(1, &renderer.shadow_tex);
    glBindTexture(GL_TEXTURE_2D, renderer.shadow_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, renderer.shadow_size,
                   renderer.shadow_size);
    tex_parameters();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &renderer.shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, renderer.shadow_tex,
                           0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(renderer, "Cannot allocate the shadow map");
    glGenTextures(2, renderer.bloom_tex);
    glGenFramebuffers(2, renderer.bloom_fbo);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, renderer.bloom_tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, std::max(1, renderer.render_width / 4),
                       std::max(1, renderer.render_height / 4));
        tex_parameters();
        glBindFramebuffer(GL_FRAMEBUFFER, renderer.bloom_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               renderer.bloom_tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return fail(renderer, "Cannot allocate bloom buffers");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    __android_log_print(ANDROID_LOG_INFO, "native_buttons",
                        "Rendering %dx%d HDR, %dx MSAA, %d shadows, %d particles",
                        renderer.render_width, renderer.render_height, renderer.samples,
                        renderer.shadow_size, renderer.particle_count);
    if (glGetError() != GL_NO_ERROR)
        return fail(renderer, "GPU allocation failed");
    return {};
}

void clear_instances(Renderer& renderer) {
    renderer.model_transform = Mat4{};
    for (auto& m : renderer.meshes)
        m.items.clear();
    renderer.ui.clear();
    renderer.triangle_count = 0;
}
void add(Renderer& renderer, Shape shape, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
    renderer.meshes[int(shape)].items.push_back(
        {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}
void fullscreen(Renderer& renderer, GLuint p) {
    glUseProgram(p);
    glBindVertexArray(renderer.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
void draw_meshes(Renderer& renderer, GLuint p, const Mat4& view, const Mat4& light, Vec3 eye,
                 float time, bool shadow) {
    glUseProgram(p);
    matrix(p, "u_view", view);
    matrix(p, "u_light", light);
    glUniform3f(glGetUniformLocation(p, "u_eye"), eye.x, eye.y, eye.z);
    glUniform1f(glGetUniformLocation(p, "u_time"), time);
    glUniform1f(glGetUniformLocation(p, "u_shadow_texel"), 1.f / renderer.shadow_size);
    glUniform1i(glGetUniformLocation(p, "u_shadow"), 0);
    for (auto& m : renderer.meshes)
        if (!m.items.empty()) {
            glBindVertexArray(m.vao);
            glBindBuffer(GL_ARRAY_BUFFER, m.instances);
            // The shadow and color pass share this upload.
            if (shadow)
                glBufferData(GL_ARRAY_BUFFER, m.items.size() * sizeof(Instance), m.items.data(),
                             GL_STREAM_DRAW);
            glDrawElementsInstanced(GL_TRIANGLES, m.indices, GL_UNSIGNED_INT, nullptr,
                                    m.items.size());
            if (!shadow)
                renderer.triangle_count += m.indices / 3 * m.items.size();
        }
}

Result<void> render(Renderer& renderer, Vec3 eye, Vec3 target, float time, bool maximum) {
    if (auto result = ensure_targets(renderer, maximum); !result)
        return std::unexpected(result.error());
    if (renderer.timer_supported) {
        GLboolean disjoint = GL_FALSE;
        glGetBooleanv(GL_GPU_DISJOINT_EXT, &disjoint);
        for (unsigned i = 0; i < 4; ++i)
            if (renderer.query_pending[i]) {
                GLuint ready = 0;
                glGetQueryObjectuiv(renderer.queries[i], GL_QUERY_RESULT_AVAILABLE, &ready);
                if (ready) {
                    GLuint64 ns = 0;
                    renderer.query_result(renderer.queries[i], GL_QUERY_RESULT, &ns);
                    if (!disjoint) {
                        float value = ns / 1000000.f;
                        renderer.gpu_millis = renderer.gpu_millis == 0
                                                  ? value
                                                  : renderer.gpu_millis * .85f + value * .15f;
                    }
                    renderer.query_pending[i] = false;
                }
            }
        if (!renderer.query_pending[renderer.query_index])
            glBeginQuery(GL_TIME_ELAPSED_EXT, renderer.queries[renderer.query_index]);
        else
            renderer.timer_supported = false;  // Never block the UI waiting for an old query.
    }
    Mat4 view = perspective(42 * kPi / 180, float(renderer.width) / renderer.height, .15f, 80) *
                look_at(eye, target);
    Mat4 light = ortho(-8, 8, -8, 8, .1f, 32) * look_at({-8, 13, 8}, {0, 0, 0});
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.shadow_fbo);
    glViewport(0, 0, renderer.shadow_size, renderer.shadow_size);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2, 3);
    draw_meshes(renderer, renderer.shadow_program, light, light, eye, time, true);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.ms_fbo);
    glViewport(0, 0, renderer.render_width, renderer.render_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    fullscreen(renderer, renderer.sky_program);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer.shadow_tex);
    draw_meshes(renderer, renderer.scene_program, view, light, eye, time, false);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, renderer.particle_buffer);
    glUseProgram(renderer.compute_program);
    glUniform1f(glGetUniformLocation(renderer.compute_program, "u_time"), time);
    glDispatchCompute(renderer.particle_count / 128, 1, 1);
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    glUseProgram(renderer.particle_program);
    matrix(renderer.particle_program, "u_view", view);
    glUniform1f(glGetUniformLocation(renderer.particle_program, "u_scale"),
                renderer.render_height / 1000.f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glBindVertexArray(renderer.particle_vao);
    glDrawArrays(GL_POINTS, 0, renderer.particle_count);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, renderer.ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, renderer.hdr_fbo);
    glBlitFramebuffer(0, 0, renderer.render_width, renderer.render_height, 0, 0,
                      renderer.render_width, renderer.render_height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer.ms_fbo);
    const GLenum discard[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, discard);
    int bw = std::max(1, renderer.render_width / 4), bh = std::max(1, renderer.render_height / 4);
    glViewport(0, 0, bw, bh);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 6; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, renderer.bloom_fbo[i % 2]);
        glUseProgram(renderer.blur_program);
        glBindTexture(GL_TEXTURE_2D, i == 0 ? renderer.hdr_tex : renderer.bloom_tex[(i - 1) % 2]);
        glUniform1i(glGetUniformLocation(renderer.blur_program, "u_image"), 0);
        glUniform1i(glGetUniformLocation(renderer.blur_program, "u_extract"), i == 0);
        glUniform2f(glGetUniformLocation(renderer.blur_program, "u_direction"),
                    i % 2 == 0 ? 1.f / bw : 0, i % 2 ? 1.f / bh : 0);
        fullscreen(renderer, renderer.blur_program);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, renderer.width, renderer.height);
    glUseProgram(renderer.post_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer.hdr_tex);
    glUniform1i(glGetUniformLocation(renderer.post_program, "u_scene"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, renderer.bloom_tex[1]);
    glUniform1i(glGetUniformLocation(renderer.post_program, "u_bloom"), 1);
    fullscreen(renderer, renderer.post_program);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::snprintf(renderer.error, sizeof(renderer.error), "OpenGL error 0x%x", error);
        return fail(renderer, renderer.error);
    }
    return {};
}

void draw_rect(Renderer& renderer, Rect r, float radius, Color color) {
    radius = std::min(radius, std::min(r.w, r.h) * .5f);
    UiVertex center{r.x + r.w * .5f, r.y + r.h * .5f, .5f / kAtlasSize, .5f / kAtlasSize, color};
    std::vector<UiVertex> perimeter;
    perimeter.reserve(36);
    for (int corner = 0; corner < 4; ++corner)
        for (int j = 0; j <= 8; ++j) {
            float a = (-kPi * .5f + corner * kPi * .5f) + j * kPi / 16;
            float cx = corner < 2 ? r.x + r.w - radius : r.x + radius;
            float cy = corner == 0 || corner == 3 ? r.y + radius : r.y + r.h - radius;
            perimeter.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius,
                                 .5f / kAtlasSize, .5f / kAtlasSize, color});
        }
    for (size_t i = 0; i < perimeter.size(); ++i) {
        renderer.ui.push_back(center);
        renderer.ui.push_back(perimeter[i]);
        renderer.ui.push_back(perimeter[(i + 1) % perimeter.size()]);
    }
}
void draw_text(Renderer& renderer, const char* value, float x, float baseline, float height,
               Color color, bool centered) {
    float scale = height / 48;
    if (centered) {
        float width = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p >= 32 && *p < 128)
                width += renderer.glyphs[*p - 32].advance;
        x -= width * scale * .5f;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (*p >= 32 && *p < 128) {
            const Glyph& g = renderer.glyphs[*p - 32];
            float left = x + g.xoff * scale, top = baseline + g.yoff * scale,
                  right = left + (g.x1 - g.x0) * scale, bottom = top + (g.y1 - g.y0) * scale;
            UiVertex a{left, top, g.x0 / kAtlasSize, g.y0 / kAtlasSize, color},
                b{right, top, g.x1 / kAtlasSize, g.y0 / kAtlasSize, color},
                c{right, bottom, g.x1 / kAtlasSize, g.y1 / kAtlasSize, color},
                d{left, bottom, g.x0 / kAtlasSize, g.y1 / kAtlasSize, color};
            renderer.ui.insert(renderer.ui.end(), {a, b, c, a, c, d});
            x += g.advance * scale;
        }
}
Result<void> present(Renderer& renderer) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, renderer.width, renderer.height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(renderer.ui_program);
    glUniform2f(glGetUniformLocation(renderer.ui_program, "u_size"), renderer.width,
                renderer.height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderer.font_tex);
    glUniform1i(glGetUniformLocation(renderer.ui_program, "u_font"), 0);
    glBindVertexArray(renderer.ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer.ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, renderer.ui.size() * sizeof(UiVertex), renderer.ui.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, renderer.ui.size());
    glDisable(GL_BLEND);
    if (renderer.timer_supported) {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        renderer.query_pending[renderer.query_index] = true;
        renderer.query_index = (renderer.query_index + 1) % 4;
    }
    if (!renderer.offscreen) {
        if (!eglSwapBuffers(renderer.display, renderer.surface))
            return fail(renderer, "Cannot present the GPU frame");
    } else
        glFlush();
    return {};
}
Result<void> capture_frame(Renderer& renderer, const char* path) {
    std::vector<unsigned char> rgba(size_t(renderer.width) * renderer.height * 4);
    glReadPixels(0, 0, renderer.width, renderer.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (glGetError() != GL_NO_ERROR)
        return fail(renderer, "Cannot read the framebuffer");
    FILE* file = std::fopen(path, "wb");
    if (!file)
        return fail(renderer, "Cannot open the screenshot output");
    std::fprintf(file, "P6\n%d %d\n255\n", renderer.width, renderer.height);
    for (int y = renderer.height - 1; y >= 0; --y)
        for (int x = 0; x < renderer.width; ++x)
            std::fwrite(&rgba[(size_t(y) * renderer.width + x) * 4], 1, 3, file);
    if (std::fclose(file) != 0)
        return fail(renderer, "Cannot write the screenshot");
    return {};
}
void close_renderer(Renderer& renderer) noexcept {
    if (renderer.display != EGL_NO_DISPLAY && renderer.context != EGL_NO_CONTEXT &&
        renderer.surface != EGL_NO_SURFACE) {
        eglMakeCurrent(renderer.display, renderer.surface, renderer.surface, renderer.context);
        destroy_targets(renderer);
        for (auto& m : renderer.meshes) {
            glDeleteVertexArrays(1, &m.vao);
            glDeleteBuffers(1, &m.vbo);
            glDeleteBuffers(1, &m.ebo);
            glDeleteBuffers(1, &m.instances);
            m.vao = m.vbo = m.ebo = m.instances = 0;
            m.items.clear();
        }
        for (GLuint p : {renderer.scene_program, renderer.shadow_program, renderer.sky_program,
                         renderer.blur_program, renderer.post_program, renderer.particle_program,
                         renderer.compute_program, renderer.ui_program})
            glDeleteProgram(p);
        glDeleteBuffers(1, &renderer.particle_buffer);
        glDeleteVertexArrays(1, &renderer.particle_vao);
        glDeleteBuffers(1, &renderer.ui_vbo);
        glDeleteVertexArrays(1, &renderer.ui_vao);
        glDeleteVertexArrays(1, &renderer.empty_vao);
        glDeleteTextures(1, &renderer.font_tex);
        glDeleteQueries(4, renderer.queries);
        eglMakeCurrent(renderer.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (renderer.display != EGL_NO_DISPLAY) {
        if (renderer.surface != EGL_NO_SURFACE)
            eglDestroySurface(renderer.display, renderer.surface);
        if (renderer.context != EGL_NO_CONTEXT)
            eglDestroyContext(renderer.display, renderer.context);
        eglTerminate(renderer.display);
    }
    renderer.display = EGL_NO_DISPLAY;
    renderer.surface = EGL_NO_SURFACE;
    renderer.context = EGL_NO_CONTEXT;
    renderer.scene_program = renderer.shadow_program = renderer.sky_program =
        renderer.blur_program = renderer.post_program = renderer.particle_program =
            renderer.compute_program = renderer.ui_program = 0;
    renderer.particle_buffer = renderer.particle_vao = renderer.ui_vbo = renderer.ui_vao =
        renderer.empty_vao = renderer.font_tex = 0;
    for (int i = 0; i < 4; ++i) {
        renderer.queries[i] = 0;
        renderer.query_pending[i] = false;
    }
    renderer.query_index = 0;
    renderer.gpu_millis = 0;
    renderer.timer_supported = false;
    renderer.width = renderer.height = 0;
}

Result<MeshId> create_mesh(Renderer& renderer, std::span<const Vertex> vertices,
                           std::span<const unsigned> indices) {
    if (vertices.empty() || indices.empty())
        return fail(renderer, "A mesh must contain vertices and indices");
    renderer.meshes.emplace_back();
    Mesh& m = renderer.meshes.back();
    m.indices = indices.size();
    m.items.reserve(1024);
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(sizeof(Vec3)));
    glGenBuffers(1, &m.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), indices.data(),
                 GL_STATIC_DRAW);
    glGenBuffers(1, &m.instances);
    glBindBuffer(GL_ARRAY_BUFFER, m.instances);
    for (int k = 0; k < 6; ++k) {
        GLuint location = 2 + k;
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                              reinterpret_cast<void*>(k * 4 * sizeof(float)));
        glVertexAttribDivisor(location, 1);
    }
    if (glGetError() != GL_NO_ERROR)
        return fail(renderer, "Cannot allocate a GPU mesh");
    return static_cast<MeshId>(renderer.meshes.size() - 1);
}
void add(Renderer& renderer, MeshId mesh, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
    renderer.meshes[mesh].items.push_back(
        {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}
void set_transform(Renderer& renderer, Mat4 transform) {
    renderer.model_transform = transform;
}

Result<Owner<Renderer>> create_renderer(ANativeWindow* window, SceneShaders shaders,
                                        int offscreen_width, int offscreen_height) {
    Owner<Renderer> renderer(new (std::nothrow) Renderer);
    if (!renderer)
        return std::unexpected(Error{"Cannot allocate renderer state"});
    if (auto result =
            initialize_renderer(*renderer, window, shaders, offscreen_width, offscreen_height);
        !result)
        return std::unexpected(result.error());
    return renderer;
}
void destroy(Renderer* renderer) noexcept {
    if (renderer) {
        close_renderer(*renderer);
        delete renderer;
    }
}
RenderStats get_stats(const Renderer& r) {
    return {r.width,          r.height,         r.render_width, r.render_height,  r.samples,
            r.particle_count, r.triangle_count, r.gpu_millis,   r.timer_supported};
}
std::string_view get_device(const Renderer& renderer) {
    return renderer.device;
}
}  // namespace gpu
