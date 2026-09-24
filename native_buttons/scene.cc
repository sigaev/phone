#include "native_buttons/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <vector>

#include "common/gpu/renderer.h"
#include "native_buttons/controls.h"

namespace native_buttons {
using namespace gpu;
using common::Error;
using common::Owner;
using common::Result;
struct Scene {
    Renderer* renderer;
    MeshId neck, scarf;
    Controls controls;
};
namespace {
constexpr Color kWhite{.91f, .93f, .87f}, kFeather{.57f, .66f, .69f}, kInk{.012f, .022f, .033f};
constexpr Color kOrange{1.f, .46f, .09f}, kPouch{1.f, .66f, .22f}, kTeal{.03f, .53f, .43f};
constexpr Color kChrome{.73f, .81f, .83f}, kCoral{.94f, .18f, .13f};
constexpr Vec3 kCrank{-.10f, .75f, 0};
constexpr float kPedalSpeed = 3.1f;
constexpr float kOrbitSpeed = 2 * kPi / 60.f;

Vec3 pedal_position(float time, int side) {
    // Forward travel is +X: the cranks turn clockwise when viewed from +Z.
    float angle = -time * kPedalSpeed + (side < 0 ? kPi : 0);
    return kCrank + Vec3{std::cos(angle) * .28f, std::sin(angle) * .28f, side * .25f};
}

void ball(Renderer& r, Vec3 p, Vec3 s, Color color, float roll = 0, float rough = .5f,
          float metal = 0) {
    Shape shape = std::max({s.x, s.y, s.z}) < .18f ? Shape::kLowSphere : Shape::kSphere;
    float kind = color.r > .45f && color.g > .55f && color.b > .45f && metal == 0 ? 3.f : 0.f;
    add(r, shape, transform(p, s, roll), color, rough, metal, 0, kind);
}
void tube(Renderer& r, Vec3 a, Vec3 b, float radius, Color color, float metal = 0) {
    add(r, Shape::kCylinder, between(a, b, radius), color, .3f, metal);
}
void leaf(Renderer& r, Vec3 a, Vec3 b, float width, Color color) {
    add(r, Shape::kFeather, between(a, b, width) * scale({1, .5f, 1}), color, .65f, 0, 0, 3);
}
void palm(Renderer& r, float x, float z, float height, float time) {
    Vec3 base{x, -.03f, z}, tip = base;
    for (int i = 0; i < 8; ++i) {
        float f = (i + 1) / 8.f;
        Vec3 next{x + .23f * f * f, height * f, z};
        tube(r, tip, next, .07f * (1 - f * .4f), {.22f, .12f, .07f});
        tip = next;
    }
    for (int i = 0; i < 9; ++i) {
        float a = i * 2 * kPi / 9;
        Vec3 prev = tip;
        for (int j = 1; j <= 4; ++j) {
            float f = j / 4.f;
            Vec3 next = tip + Vec3{std::cos(a) * f * 1.05f, .36f * std::sin(f * kPi) - f * .35f,
                                   std::sin(a) * f * 1.05f};
            next.y += .06f * std::sin(time * 1.7f + a + f * 2) * f;
            leaf(r, prev, next, .16f * (1 - f * .65f),
                 i % 2 ? Color{.025f, .20f, .095f} : Color{.05f, .34f, .16f});
            prev = next;
        }
    }
    for (int i = 0; i < 3; ++i)
        ball(r, tip + Vec3{std::cos(i * 2.f) * .12f, -.1f, std::sin(i * 2.f) * .12f},
             {.10f, .12f, .10f}, {.31f, .18f, .07f});
}
void wheel(Renderer& r, float x, float spin) {
    Vec3 center{x, .62f, 0};
    add(r, Shape::kTorus, transform(center, {.57f, .57f, .84f}), kInk, .87f);
    add(r, Shape::kTorus, transform(center, {.512f, .512f, .18f}), kChrome, .19f, .88f);
    add(r, Shape::kTorus, transform(center, {.49f, .49f, .14f}), {.88f, .77f, .54f}, .8f);
    tube(r, center + Vec3{0, 0, -.13f}, center + Vec3{0, 0, .13f}, .064f, kChrome, .85f);
    for (int side : {-1, 1})
        for (int i = 0; i < 20; ++i) {
            float a = i * 2 * kPi / 20 + spin;
            Vec3 rim = center + Vec3{std::cos(a) * .49f, std::sin(a) * .49f, 0};
            Vec3 hub = center + Vec3{0, 0, side * .072f};
            tube(r, hub, rim, .007f, {.68f, .77f, .82f}, .82f);
        }
    for (int i = 0; i < 6; ++i) {
        float a = spin + i * kPi / 3;
        add(r, Shape::kSphere,
            transform(center + Vec3{std::cos(a) * .56f, std::sin(a) * .56f, .056f},
                      {.026f, .012f, .015f}, a),
            {.16f, .21f, .24f}, .75f);
    }
}
void bicycle(Renderer& r, float time) {
    float spin = -time * 4.7f;
    wheel(r, -1.06f, spin);
    wheel(r, 1.06f, spin);
    Vec3 back{-1.06f, .62f, 0}, crank = kCrank, seat{-.47f, 1.43f, 0}, head{.64f, 1.44f, 0},
                                front{1.06f, .62f, 0};
    tube(r, seat, crank, .043f, kTeal, .52f);
    tube(r, seat, head, .040f, kTeal, .52f);
    tube(r, head, crank, .046f, kTeal, .52f);
    for (int side : {-1, 1}) {
        Vec3 offset{0, 0, side * .075f};
        tube(r, back + offset, seat, .025f, kTeal, .5f);
        tube(r, back + offset, crank, .026f, kTeal, .5f);
        tube(r, front + offset, head + Vec3{.04f, -.20f, side * .07f}, .026f, kTeal, .5f);
        tube(r, head + Vec3{.04f, -.20f, side * .07f}, head, .025f, kTeal, .5f);
    }
    tube(r, seat, seat + Vec3{-.04f, .13f, 0}, .028f, kChrome, .8f);
    ball(r, seat + Vec3{-.07f, .14f, 0}, {.26f, .075f, .17f}, kInk);
    Vec3 stem{.72f, 1.69f, 0};
    tube(r, head, stem, .03f, kChrome, .8f);
    tube(r, stem, stem + Vec3{.12f, .02f, 0}, .028f, kChrome, .8f);
    tube(r, {.84f, 1.71f, -.39f}, {.84f, 1.71f, .39f}, .025f, kChrome, .8f);
    for (int side : {-1, 1})
        tube(r, {.84f, 1.71f, side * .26f}, {.84f, 1.71f, side * .43f}, .038f, kInk);
    add(r, Shape::kTorus, transform(crank + Vec3{0, 0, .11f}, {.16f, .16f, .20f}), kChrome, .2f,
        .9f);
    tube(r, back + Vec3{0, -.075f, .11f}, crank + Vec3{0, -.16f, .11f}, .013f, kInk, .8f);
    tube(r, back + Vec3{0, .075f, .11f}, crank + Vec3{0, .16f, .11f}, .013f, kInk, .8f);
    for (int side : {-1, 1}) {
        Vec3 pedal = pedal_position(time, side);
        tube(r, crank + Vec3{0, 0, side * .13f}, pedal, .022f, kChrome, .8f);
        ball(r, pedal, {.12f, .03f, .12f}, kInk);
    }
    // Small mechanical details: bottle, headlight, rear rack, bell and brake cables.
    tube(r, {-.20f, .93f, .05f}, {-.30f, 1.16f, .05f}, .063f, kCoral);
    ball(r, {-.30f, 1.18f, .05f}, {.043f, .04f, .043f}, kWhite);
    ball(r, {.83f, 1.47f, 0}, {.07f, .08f, .09f}, kInk);
    add(r, Shape::kSphere, transform({.89f, 1.48f, 0}, {.025f, .055f, .060f}), {1.f, .88f, .48f},
        .15f, .1f, 2.5f);
    tube(r, {-1.29f, 1.24f, -.14f}, {-.74f, 1.24f, -.14f}, .018f, kChrome, .8f);
    tube(r, {-1.29f, 1.24f, .14f}, {-.74f, 1.24f, .14f}, .018f, kChrome, .8f);
    for (int side : {-1, 1})
        tube(r, {-1.24f, 1.24f, side * .14f}, back + Vec3{0, 0, side * .11f}, .015f, kChrome, .8f);
    ball(r, {.78f, 1.77f, .18f}, {.065f, .04f, .06f}, {.9f, .65f, .18f}, 0, .2f, .8f);
    Vec3 prev{.82f, 1.68f, .20f};
    for (int i = 1; i <= 8; ++i) {
        float f = i / 8.f;
        Vec3 next{.82f - .21f * std::sin(f * kPi), 1.68f - f * .47f, .2f * (1 - f)};
        tube(r, prev, next, .007f, kInk);
        prev = next;
    }
}
void pelican(Scene& scene, float time) {
    Renderer& r = *scene.renderer;
    float bob = std::sin(time * 6.2f) * .025f;
    float breath = 1.f + .012f * std::sin(time * 2.2f);
    float flex = .065f * std::sin(time * 1.4f);
    ball(r, {-.38f, 1.98f + bob, 0}, {.61f, .65f * breath, .40f * breath}, kWhite, -.18f, .65f);
    ball(r, {-.52f, 2.10f + bob, 0}, {.51f, .42f, .40f}, {.78f, .85f, .85f}, -.20f, .6f);
    // Layered flight feathers and a long, unmistakable pelican neck and bill.
    for (int side : {-1, 1}) {
        ball(r, {-.36f, 2.01f + bob, side * (.34f + .012f * std::sin(time * 3.1f))},
             {.56f, .37f, .115f}, kFeather, -.32f);
        for (int i = 0; i < 9; ++i) {
            float f = i / 8.f;
            ball(r,
                 {-.74f + f * .47f, 1.77f + f * .34f + bob,
                  side * (.38f + .02f * std::sin(float(i)))},
                 {.24f, .073f, .045f}, i < 3 ? Color{.19f, .27f, .32f} : kWhite, -.62f + f * .38f,
                 .7f);
        }
        Vec3 shoulder{-.08f, 2.12f + bob, side * .37f},
            elbow{.34f, 1.89f + bob, side * (.42f + .035f * std::sin(time * 3.1f))},
            tip{.84f, 1.74f, side * .35f};
        add(r, Shape::kSphere, between(shoulder, elbow, .13f) * scale({1, .65f, .7f}), kWhite, .65f,
            0, 0, 3);
        add(r, Shape::kSphere, between(elbow, tip, .105f) * scale({1, .65f, .65f}), kWhite, .65f, 0,
            0, 3);
        for (int i = 0; i < 3; ++i)
            ball(r, {.79f + i * .035f, 1.75f, side * (.31f + i * .025f)}, {.085f, .030f, .035f},
                 kWhite, -.15f);
    }
    for (int i = 0; i < 7; ++i)
        ball(r, {-.87f - i * .07f, 1.91f + i * .025f + bob, (i - 3) * .043f}, {.27f, .067f, .072f},
             i % 2 ? kWhite : kFeather, .23f);
    add(r, scene.neck, translation({0, bob, 0}), kWhite, .65f, 0, 0, 4);
    set_transform(r, translation({flex, 0, .035f * std::sin(time * .9f)}));
    ball(r, {.48f, 3.10f + bob, 0}, {.32f, .29f, .26f}, kWhite);
    ball(r, {1.07f, 2.94f + bob, 0}, {.57f, .17f, .145f}, kPouch, -.09f, .6f);
    ball(r, {1.14f, 3.062f + bob, 0}, {.62f, .049f, .16f}, kOrange, -.052f, .36f);
    ball(r, {1.69f, 3.015f + bob, 0}, {.10f, .047f, .049f}, {.86f, .28f, .045f}, -.18f);
    tube(r, {.71f, 3.03f + bob, .147f}, {1.63f, 3.015f + bob, .044f}, .006f, {.57f, .23f, .02f});
    float blink_phase = std::fmod(time + 1.7f, 4.8f);
    float eye_open = std::clamp(std::fabs(blink_phase - .12f) / .10f, .08f, 1.f);
    for (int side : {-1, 1}) {
        ball(r, {.58f, 3.17f + bob, side * .225f}, {.095f, .10f * eye_open, .027f},
             {1.f, .77f, .32f});
        ball(r, {.605f, 3.182f + bob, side * .25f}, {.050f, .060f * eye_open, .021f}, kInk, 0,
             .18f);
        ball(r, {.615f, 3.205f + bob, side * .268f}, {.014f, .018f * eye_open, .009f}, kWhite);
    }
    ball(r, {.40f, 3.32f + bob, 0}, {.30f, .14f, .265f}, kCoral, -.08f, .35f);
    for (int i = -2; i <= 2; ++i)
        ball(r, {.40f, 3.437f + bob, i * .065f}, {.18f, .014f, .014f}, {.31f, .06f, .065f}, -.10f);
    for (int side : {-1, 1})
        tube(r, {.34f, 3.29f + bob, side * .235f}, {.22f, 2.92f + bob, side * .145f}, .014f, kInk);
    set_transform(r, Mat4{});
    // Continuous cloth strips deform in the vertex shader with travelling waves.
    add(r, Shape::kTorus,
        translation({-.025f, 2.70f + bob, 0}) * rotate_x(kPi * .5f) * scale({.20f, .20f, .80f}),
        {.97f, .59f, .09f}, .8f);
    for (int tail = 0; tail < 2; ++tail) {
        add(r, scene.scarf, translation({-.10f, 2.70f + bob, tail ? .13f : -.13f}),
            {.97f, .59f, .09f}, .8f, 0, 0, 5);
    }
    for (int side : {-1, 1}) {
        Vec3 foot = pedal_position(time, side) + Vec3{0, .05f, 0};
        Vec3 hip{-.33f, 1.64f + bob, side * .22f};
        Vec3 d = foot - hip, mid = (foot + hip) * .5f;
        float distance = length(d),
              bend = std::sqrt(std::max(0.f, .60f * .60f - distance * distance * .25f));
        Vec3 knee = mid + unit(Vec3{-d.y, d.x, 0}) * bend;
        tube(r, hip, knee, .047f, kOrange);
        ball(r, knee, {.067f, .067f, .06f}, kOrange);
        tube(r, knee, foot, .036f, kOrange);
        ball(r, foot + Vec3{.08f, 0, 0}, {.19f, .055f, .105f}, kOrange, .03f);
        for (int i = -1; i <= 1; ++i)
            tube(r, foot + Vec3{.02f, .01f, i * .025f}, foot + Vec3{.24f, -.006f, i * .07f}, .02f,
                 kOrange);
    }
}
}  // namespace

void build_scene(Scene& scene, float time, bool maximum) {
    Renderer& r = *scene.renderer;
    clear_instances(r);
    add(r, Shape::kPlane, transform({0, -.15f, 0}, {55, 1, 55}), {.02f, .2f, .24f}, .2f, .4f, 0, 1);
    add(r, Shape::kPlane, transform({0, -.018f, 0}, {22, 1, 1.16f}), {.27f, .32f, .32f}, .88f);
    add(r, Shape::kPlane, transform({0, .015f, 0}, {22, 1, 1.00f}), {.052f, .081f, .11f}, .83f, 0,
        0, 2);
    for (int side : {-1, 1}) {
        tube(r, {-22, -.01f, side * 1.16f}, {22, -.01f, side * 1.16f}, .035f, {.63f, .66f, .58f});
        for (int i = 0; i < 12; ++i) {
            float x = wrap(i * 3.7f - time * 2.6f + 220.f, 44.f) - 22.f;
            ball(r, {x, -.11f, side * 1.35f}, {.35f, .11f, .22f}, {.21f, .27f, .26f}, i * .7f, .9f);
            if (side < 0) {
                tube(r, {x, .02f, -1.04f}, {x, .42f, -1.04f}, .025f, {.18f, .26f, .28f});
                add(r, Shape::kSphere, transform({x, .43f, -1.04f}, {.046f, .037f, .045f}),
                    {1.f, .66f, .22f}, .4f, 0, 2);
            }
        }
    }
    int palms = maximum ? 16 : 9;
    for (int i = 0; i < palms; ++i) {
        float x = wrap(i * 4.7f - time * 2.6f + 200.f, 44.f) - 22.f, z = -3.4f - (i % 3) * 2.3f;
        ball(r, {x, -.23f, z}, {1.4f, .31f, 1.2f}, {.37f, .32f, .18f}, 0, .9f);
        palm(r, x, z, 1.65f + (i % 4) * .24f, time + i);
    }
    for (int i = 0; i < 7; ++i)
        ball(r, {-20.f + i * 7, -.5f, -24.f - i % 2 * 5}, {5.f, .65f + (i % 3) * .3f, 3.f},
             {.025f, .06f, .065f}, 0, .9f);
    for (int i = 0; i < 6; ++i) {
        float x = std::sin(time * .12f + i) * 9, z = -11.f - i;
        Vec3 p{x, 4.f + (i % 3) * .4f, z};
        for (int side : {-1, 1})
            leaf(r, p, p + Vec3{side * .22f, .09f * std::sin(time * 3 + i), 0}, .033f,
                 {.64f, .78f, .80f});
    }
    bicycle(r, time);
    pelican(scene, time);
}

Vec3 camera(float time, float yaw, float aspect) {
    float angle = yaw + time * kOrbitSpeed;
    float distance = std::max(7.2f, 5.15f / std::max(aspect, .35f));
    distance *= 1.f + .025f * std::sin(time * .19f);
    float elevation = .29f + .045f * std::sin(time * .14f);
    return {std::sin(angle) * distance, 1.3f + distance * elevation, std::cos(angle) * distance};
}

Controls draw_overlay(Renderer& r, Rect safe, int count, int pressed, float fps, bool maximum,
                      bool paused, bool saved) {
    float s = std::min(safe.w / 400.f, safe.h / 720.f), cx = safe.x + safe.w * .5f,
          top = safe.y + 22 * s, bottom = safe.y + safe.h;
    Color text{.92f, .96f, .97f}, muted{.58f, .72f, .77f}, mint{.49f, .94f, .76f};
    draw_rect(r, {cx - 170 * s, top, 91 * s, 23 * s}, 11 * s, {.08f, .22f, .23f, .93f});
    draw_text(r, "LIVE / 3D", cx - 124.5f * s, top + 16 * s, 14 * s, mint, true);
    draw_text(r, "Coasting.", cx - 170 * s, top + 72 * s, 51 * s, text);
    draw_text(r, "A pelican. A bicycle. The long way home.", cx - 170 * s, top + 101 * s, 17 * s,
              muted);
    char stats[128];
    std::snprintf(stats, sizeof(stats), "%.0f FPS", fps);
    draw_text(r, stats, cx + 130 * s, top + 18 * s, 20 * s, text, true);
    if (get_stats(r).gpu_ms > 0)
        std::snprintf(stats, sizeof(stats), "GPU %.1f ms", get_stats(r).gpu_ms);
    else
        std::snprintf(stats, sizeof(stats), "%s", get_device(r).data());
    draw_text(r, stats, cx + 123 * s, top + 37 * s, 12 * s, muted, true);
    Controls controls = layout_controls(safe);
    draw_rect(r, controls.quality, 16 * s,
              pressed == 3 ? Color{.18f, .36f, .36f, .95f} : Color{.06f, .16f, .20f, .86f});
    draw_text(r, maximum ? "ULTRA DETAIL" : "HIGH DETAIL", cx - 113 * s, top + 141 * s, 13 * s,
              mint, true);
    draw_rect(r, controls.pause, 16 * s,
              pressed == 4 ? Color{.18f, .36f, .36f, .95f} : Color{.06f, .16f, .20f, .86f});
    draw_text(r, paused ? "RESUME" : "PAUSE", cx - 4 * s, top + 141 * s, 13 * s, text, true);
    draw_text(r, "Auto orbit / drag to look around", cx, bottom - 196 * s, 14 * s, muted, true);
    Rect panel{cx - 180 * s, bottom - 176 * s, 360 * s, 156 * s};
    draw_rect(r, panel, 24 * s, {.025f, .075f, .105f, .94f});
    draw_text(r, "YOUR COUNT", cx - 160 * s, panel.y + 28 * s, 12 * s, muted);
    char number[24];
    std::snprintf(number, sizeof(number), "%d", count);
    draw_text(r, number, cx - 160 * s, panel.y + 70 * s, 43 * s, text);
    draw_text(r, saved ? "Saved automatically" : "Not saved - try again", cx + 91 * s,
              panel.y + 36 * s, 13 * s, saved ? muted : Color{1.f, .65f, .4f}, true);
    std::snprintf(stats, sizeof(stats), "%dx MSAA / %s", get_stats(r).samples,
                  maximum ? "130%" : "100%");
    draw_text(r, stats, cx + 91 * s, panel.y + 57 * s, 12 * s, muted, true);
    draw_rect(r, controls.add, 16 * s, pressed == 1 ? Color{.28f, .70f, .56f} : mint);
    draw_text(r, "+  Add one", cx - 50 * s, panel.y + 119 * s, 22 * s, {.035f, .15f, .14f}, true);
    draw_rect(r, controls.reset, 16 * s,
              pressed == 2 ? Color{.17f, .30f, .35f} : Color{.10f, .20f, .25f});
    draw_text(r, "Reset", cx + 116 * s, panel.y + 118 * s, 18 * s, text, true);
    return controls;
}

namespace {
Vec3 neck_center(float t) {
    constexpr Vec3 kPoints[] = {{-.26f, 2.0f, 0},  {-.20f, 2.12f, 0}, {-.04f, 2.42f, 0},
                                {-.06f, 2.72f, 0}, {.10f, 2.97f, 0},  {.43f, 3.12f, 0},
                                {.56f, 3.14f, 0}};
    float segment = std::clamp(t, 0.f, 1.f) * 4;
    int i = std::min(3, int(segment));
    float u = segment - i, u2 = u * u, u3 = u2 * u;
    Vec3 a = kPoints[i], b = kPoints[i + 1], c = kPoints[i + 2], d = kPoints[i + 3];
    return (b * 2 + (c - a) * u + (a * 2 - b * 5 + c * 4 - d) * u2 + (b * 3 - a - c * 3 + d) * u3) *
           .5f;
}
Result<MeshId> create_scarf(Renderer& r) {
    std::vector<Vertex> vertices;
    std::vector<unsigned> indices;
    constexpr int kSegments = 48;
    for (int i = 0; i <= kSegments; ++i) {
        float t = float(i) / kSegments;
        for (int side : {-1, 1}) {
            vertices.push_back({{-.84f * t, side * .06f * (1 - .25f * t), 0}, {0, 0, 1}});
        }
        if (i < kSegments) {
            unsigned a = i * 2;
            indices.insert(indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
    return create_mesh(r, vertices, indices);
}
Result<MeshId> create_neck(Renderer& r) {
    std::vector<Vertex> vertices;
    std::vector<unsigned> indices;
    constexpr int kRings = 64, kSides = 32;
    for (int ring = 0; ring <= kRings; ++ring) {
        float t = float(ring) / kRings;
        Vec3 center = neck_center(t);
        Vec3 tangent =
            unit(neck_center(std::min(1.f, t + .001f)) - neck_center(std::max(0.f, t - .001f)));
        Vec3 across = unit(cross(tangent, {0, 0, 1}));
        float radius = .17f + .14f * std::pow(1 - t, 3.f) + .075f * std::pow(t, 4.f);
        for (int side = 0; side <= kSides; ++side) {
            float angle = float(side) / kSides * 2 * kPi;
            Vec3 normal = across * std::cos(angle) + Vec3{0, 0, std::sin(angle)};
            vertices.push_back({center + normal * radius, normal});
        }
    }
    for (int i = 0; i < kRings; ++i)
        for (int j = 0; j < kSides; ++j) {
            unsigned a = i * (kSides + 1) + j;
            indices.insert(indices.end(),
                           {a, a + 1, a + kSides + 1, a + 1, a + kSides + 2, a + kSides + 1});
        }
    return create_mesh(r, vertices, indices);
}
}  // namespace

Result<Owner<Scene>> create_scene(Renderer& renderer) {
    auto neck = create_neck(renderer);
    if (!neck)
        return std::unexpected(neck.error());
    auto scarf = create_scarf(renderer);
    if (!scarf)
        return std::unexpected(scarf.error());
    Owner<Scene> scene(new (std::nothrow) Scene{&renderer, *neck, *scarf, {}});
    if (!scene)
        return std::unexpected(Error{"Cannot allocate scene state"});
    return scene;
}
void destroy(Scene* scene) noexcept {
    delete scene;
}
int hit_test(const Scene& scene, float x, float y) {
    return static_cast<int>(hit_test(scene.controls, x, y));
}
Result<void> render_scene(Scene& scene, float time, float yaw, bool maximum, int count, int pressed,
                          float fps, bool paused, Rect safe, bool saved, bool overlay) {
    Renderer& r = *scene.renderer;
    if (auto result = prepare_frame(r, maximum); !result)
        return result;
    build_scene(scene, time, maximum);
    auto stats = get_stats(r);
    safe = safe_area(safe, stats.width, stats.height);
    if (auto result = render(r, camera(time, yaw, float(stats.width) / stats.height), {0, 1.25f, 0},
                             time, maximum);
        !result)
        return std::unexpected(result.error());
    scene.controls = overlay ? draw_overlay(r, safe, count, pressed, fps, maximum, paused, saved)
                             : layout_controls(safe);
    return present(r);
}
}  // namespace native_buttons
