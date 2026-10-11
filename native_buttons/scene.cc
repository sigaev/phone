#include "native_buttons/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <new>
#include <span>
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
  MeshId pelican_neck, flamingo_neck, eagle_neck, scarf, lettering;
  Controls controls;
};

namespace {
constexpr Color kWhite{.91f, .93f, .87f}, kInk{.012f, .022f, .033f};
constexpr Color kFeather{.57f, .66f, .69f}, kOrange{1.f, .46f, .09f}, kPouch{1.f, .66f, .22f};
constexpr Color kPink{1.f, .25f, .46f}, kBlush{1.f, .48f, .62f}, kRose{.86f, .075f, .27f};
constexpr Color kLegs{.92f, .23f, .33f}, kMint{.10f, .74f, .56f}, kTeal{.03f, .53f, .43f};
constexpr Color kChrome{.73f, .81f, .83f}, kCoral{.94f, .18f, .13f};
constexpr Color kSnow{.96f, .95f, .91f}, kBrown{.20f, .11f, .05f}, kUmber{.11f, .06f, .03f};
constexpr Color kGold{1.f, .60f, .04f}, kShirt{.95f, .96f, .95f}, kKelly{.02f, .46f, .16f};
// The eagle's shirt torso. Its lettering mesh is projected onto this ellipsoid.
constexpr Vec3 kShirtCenter{-.36f, 2.f, 0}, kShirtSize{.58f, .60f, .48f};
constexpr float kShirtRoll = -.14f;
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
  add(r, Shape::kTorus, transform(crank + Vec3{0, 0, .11f}, {.16f, .16f, .20f}), kChrome, .2f, .9f);
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

float eye_openness(float phase) {
  auto blink = [&](float center, float width) {
    float t = std::clamp(1.f - std::abs(phase - center) / width, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
  };
  // A soft blink followed by a smaller flutter, with a fully open interval
  // across the phase wrap. The same scene time freezes every part of the face.
  return 1.f - .96f * std::max(blink(.18f, .13f), .72f * blink(.48f, .10f));
}

void pelican(Scene& scene, float time, float blink_phase) {
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
      ball(r, {-.74f + f * .47f, 1.77f + f * .34f + bob, side * (.38f + .02f * std::sin(float(i)))},
           {.24f, .073f, .045f}, i < 3 ? Color{.19f, .27f, .32f} : kWhite, -.62f + f * .38f, .7f);
    }
    Vec3 shoulder{-.08f, 2.12f + bob, side * .37f},
        elbow{.34f, 1.89f + bob, side * (.42f + .035f * std::sin(time * 3.1f))},
        tip{.84f, 1.74f, side * .35f};
    add(r, Shape::kSphere, between(shoulder, elbow, .13f) * scale({1, .65f, .7f}), kWhite, .65f, 0,
        0, 3);
    add(r, Shape::kSphere, between(elbow, tip, .105f) * scale({1, .65f, .65f}), kWhite, .65f, 0, 0,
        3);
    for (int i = 0; i < 3; ++i)
      ball(r, {.79f + i * .035f, 1.75f, side * (.31f + i * .025f)}, {.085f, .030f, .035f}, kWhite,
           -.15f);
  }
  for (int i = 0; i < 7; ++i)
    ball(r, {-.87f - i * .07f, 1.91f + i * .025f + bob, (i - 3) * .043f}, {.27f, .067f, .072f},
         i % 2 ? kWhite : kFeather, .23f);
  add(r, scene.pelican_neck, translation({0, bob, 0}), kWhite, .65f, 0, 0, 4);
  set_transform(r, translation({flex, 0, .035f * std::sin(time * .9f)}));
  ball(r, {.48f, 3.10f + bob, 0}, {.32f, .29f, .26f}, kWhite);
  ball(r, {1.07f, 2.94f + bob, 0}, {.57f, .17f, .145f}, kPouch, -.09f, .6f);
  ball(r, {1.14f, 3.062f + bob, 0}, {.62f, .049f, .16f}, kOrange, -.052f, .36f);
  ball(r, {1.69f, 3.015f + bob, 0}, {.10f, .047f, .049f}, {.86f, .28f, .045f}, -.18f);
  tube(r, {.71f, 3.03f + bob, .147f}, {1.63f, 3.015f + bob, .044f}, .006f, {.57f, .23f, .02f});
  float eye_open = std::clamp(std::fabs(blink_phase - .12f) / .10f, .08f, 1.f);
  for (int side : {-1, 1}) {
    ball(r, {.58f, 3.17f + bob, side * .225f}, {.095f, .10f * eye_open, .027f}, {1.f, .77f, .32f});
    ball(r, {.605f, 3.182f + bob, side * .25f}, {.050f, .060f * eye_open, .021f}, kInk, 0, .18f);
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
    add(r, scene.scarf, translation({-.10f, 2.70f + bob, tail ? .13f : -.13f}), {.97f, .59f, .09f},
        .8f, 0, 0, 5);
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

void flamingo(Scene& scene, float time, float blink_phase) {
  Renderer& r = *scene.renderer;
  float bob = std::sin(time * 6.2f) * .025f;
  float breath = 1.f + .012f * std::sin(time * 2.2f);
  float flex = .065f * std::sin(time * 1.4f);
  float sway = .035f * std::sin(time * .9f);
  // A soft pear-shaped body and overlapping rose flight feathers.
  ball(r, {-.40f, 1.99f + bob, 0}, {.57f, .56f * breath, .37f * breath}, kPink, -.20f, .65f);
  ball(r, {-.16f, 1.91f + bob, 0}, {.35f, .43f * breath, .345f}, kBlush, -.22f, .7f);
  for (int side : {-1, 1}) {
    ball(r, {-.48f, 2.05f + bob, side * (.31f + .012f * std::sin(time * 3.1f))}, {.49f, .31f, .12f},
         kRose, -.28f, .7f);
    for (int i = 0; i < 7; ++i) {
      float f = i / 6.f;
      ball(r, {-.81f + f * .48f, 1.89f + f * .26f + bob, side * .38f}, {.22f, .070f, .046f},
           i < 2 ? Color{.56f, .045f, .19f} : kPink, -.55f + f * .30f, .7f);
    }
    Vec3 shoulder{-.08f, 2.12f + bob, side * .32f},
        elbow{.34f, 1.89f + bob, side * (.42f + .035f * std::sin(time * 3.1f))},
        tip{.84f, 1.74f, side * .35f};
    add(r, Shape::kSphere, between(shoulder, elbow, .115f) * scale({1, .65f, .7f}), kPink, .65f, 0,
        0, 3);
    add(r, Shape::kSphere, between(elbow, tip, .085f) * scale({1, .65f, .65f}), kBlush, .65f, 0, 0,
        3);
    for (int i = 0; i < 3; ++i)
      ball(r, {.79f + i * .035f, 1.75f, side * (.31f + i * .025f)}, {.085f, .030f, .035f}, kBlush,
           -.15f);
  }
  for (int i = -2; i <= 2; ++i)
    ball(r, {-1.01f - .025f * std::abs(i), 2.08f + bob, i * .059f}, {.29f, .062f, .065f},
         i % 2 ? kRose : kPink, .34f, .7f);

  add(r, scene.flamingo_neck, translation({0, bob, 0}), kBlush, .65f, 0, 0, 6);
  // The head follows the neck tip; all facial features share its gentle tilt.
  set_transform(
      r, translation({.45f + flex, 3.62f + bob, sway}) * rotate_z(.035f * std::sin(time * .8f)));
  ball(r, {0, 0, 0}, {.34f, .32f, .285f}, kBlush, 0, .62f);
  ball(r, {.06f, -.13f, 0}, {.28f, .17f, .27f}, kPink, 0, .7f);
  // A compact, downturned bill: pale pink at the root with a charcoal tip.
  ball(r, {.35f, -.075f, 0}, {.20f, .10f, .125f}, {1.f, .71f, .64f}, -.13f, .4f);
  ball(r, {.51f, -.145f, 0}, {.105f, .12f, .098f}, {.085f, .042f, .07f}, -.40f, .33f);
  for (int side : {-1, 1}) {
    tube(r, {.30f, -.12f, side * .115f}, {.46f, -.14f, side * .09f}, .006f, {.37f, .10f, .16f});
    ball(r, {.37f, -.04f, side * .121f}, {.015f, .008f, .005f}, kRose);
    ball(r, {.02f, -.115f, side * .268f}, {.105f, .061f, .021f}, {1.f, .17f, .34f}, -.15f, .8f);

    float open = eye_openness(blink_phase);
    float glance_x = .023f * std::sin(time * .73f);
    float glance_y = .012f * std::sin(time * 1.13f + .7f);
    Vec3 eye{.14f, .075f, side * .25f};
    // Smooth glossy eyes, large irises, and two small catchlights. Scaling
    // both the lid opening and pupil keeps the glance inside a closing eye.
    add(r, Shape::kSphere, transform(eye, {.135f, .155f * open, .070f}), {1.f, .96f, .91f}, .24f);
    Vec3 iris{eye.x + .014f + glance_x, eye.y + glance_y * open, side * .312f};
    add(r, Shape::kSphere, transform(iris, {.078f, .090f * open, .023f}), {.18f, .075f, .17f},
        .22f);
    Vec3 pupil = iris + Vec3{.008f, 0, side * .018f};
    add(r, Shape::kSphere, transform(pupil, {.047f, .065f * open, .014f}), kInk, .12f);
    ball(r, pupil + Vec3{-.017f, .027f * open, side * .014f}, {.018f, .021f * open, .009f},
         {1.f, .98f, .95f});
    ball(r, pupil + Vec3{.020f, -.021f * open, side * .013f}, {.008f, .010f * open, .006f},
         {1.f, .90f, .85f});
    float brow = .015f * std::sin(time * 1.4f + side * .3f);
    ball(r, {.12f, .26f + brow, side * .215f}, {.11f, .018f, .023f}, {.47f, .065f, .20f},
         -.10f + brow);
  }
  // A little mint cycling helmet and chin straps frame the pink face.
  ball(r, {-.055f, .265f, 0}, {.335f, .13f, .29f}, kMint, -.08f, .38f);
  for (int i = -2; i <= 2; ++i)
    ball(r, {-.055f, .382f, i * .069f}, {.19f, .012f, .012f}, kTeal, -.10f);
  for (int side : {-1, 1})
    tube(r, {-.14f, .24f, side * .25f}, {-.18f, -.20f, side * .16f}, .012f, kRose);
  set_transform(r, Mat4{});

  // A small matching scarf leaves the slender S-shaped neck visible.
  add(r, Shape::kTorus,
      translation({.15f, 2.62f + bob, 0}) * rotate_x(kPi * .5f) * scale({.145f, .145f, .75f}),
      kMint, .8f);
  for (int tail = 0; tail < 2; ++tail)
    add(r, scene.scarf, translation({.10f, 2.62f + bob, tail ? .10f : -.10f}), kMint, .8f, 0, 0, 5);
  for (int side : {-1, 1}) {
    Vec3 foot = pedal_position(time, side) + Vec3{0, .04f, 0};
    Vec3 hip{-.33f, 1.70f + bob, side * .20f};
    Vec3 d = foot - hip, mid = (foot + hip) * .5f;
    float distance = length(d),
          bend = std::sqrt(std::max(0.f, .66f * .66f - distance * distance * .25f));
    Vec3 knee = mid + unit(Vec3{-d.y, d.x, 0}) * bend;
    tube(r, hip, knee, .028f, kLegs);
    ball(r, knee, {.042f, .045f, .040f}, kLegs);
    tube(r, knee, foot, .023f, kLegs);
    ball(r, foot + Vec3{.07f, 0, 0}, {.16f, .040f, .09f}, kPink, .03f);
    for (int i = -1; i <= 1; ++i)
      tube(r, foot + Vec3{.02f, .008f, i * .022f}, foot + Vec3{.20f, -.006f, i * .062f}, .014f,
           kPink);
  }
}

void eagle(Scene& scene, float time, float blink_phase) {
  Renderer& r = *scene.renderer;
  float bob = std::sin(time * 6.2f) * .025f;
  float breath = 1.f + .012f * std::sin(time * 2.2f);
  float flex = .065f * std::sin(time * 1.4f);
  float sway = .035f * std::sin(time * .9f);
  // A white ringer T-shirt with green trim and HARKER printed across the chest.
  Vec3 chest = kShirtCenter + Vec3{0, bob, 0};
  ball(r, chest, {kShirtSize.x, kShirtSize.y * breath, kShirtSize.z * breath}, kShirt, kShirtRoll,
       .85f);
  ball(r, {-.50f, 2.14f + bob, 0}, {.46f, .40f, .45f}, kShirt, -.20f, .85f);
  add(r, scene.lettering, translation(chest) * rotate_z(kShirtRoll) * scale({1, breath, breath}),
      kKelly, .7f);
  add(r, Shape::kCylinder,
      translation({-.14f, 2.53f + bob, 0}) * rotate_z(kShirtRoll) * scale({.23f, .05f, .21f}),
      kKelly, .7f);
  // Feathered brown hips below the hem and a fanned white tail.
  ball(r, {-.40f, 1.56f + bob, 0}, {.48f, .24f, .41f}, kBrown, -.10f, .8f);
  for (int i = -2; i <= 2; ++i)
    leaf(r, {-.80f, 1.70f + bob, i * .045f}, {-1.36f, 1.60f + bob + .02f * std::abs(i), i * .12f},
         .085f, kSnow);
  for (int side : {-1, 1}) {
    Vec3 shoulder{-.10f, 2.16f + bob, side * .42f},
        elbow{.32f, 1.90f + bob, side * (.48f + .035f * std::sin(time * 3.1f))},
        tip{.84f, 1.74f, side * .35f};
    Vec3 arm = elbow - shoulder;
    add(r, Shape::kSphere, between(shoulder, elbow, .105f) * scale({1, .65f, .7f}), kBrown, .7f, 0,
        0, 3);
    add(r, Shape::kSphere, between(elbow, tip, .095f) * scale({1, .65f, .65f}), kBrown, .7f, 0, 0,
        3);
    // Short sleeves with green cuffs; the brown wings emerge to grip the handlebar.
    add(r, Shape::kCylinder, between(shoulder - arm * .15f, shoulder + arm * .42f, .16f), kShirt,
        .85f, 0, 0, 3);
    add(r, Shape::kCylinder, between(shoulder + arm * .40f, shoulder + arm * .48f, .168f), kKelly,
        .7f);
    for (int i = 0; i < 4; ++i)
      ball(r, {.80f + i * .03f, 1.75f - i * .01f, side * (.30f + i * .03f)}, {.09f, .03f, .035f},
           kUmber, -.20f);
    // Overlapping dark coverts along each forearm read as folded wing feathers.
    for (int i = 0; i < 3; ++i) {
      Vec3 covert = elbow + (tip - elbow) * (.12f + i * .26f) + Vec3{-.03f, -.055f, side * .012f};
      ball(r, covert, {.15f, .036f, .045f}, i % 2 ? kBrown : kUmber, -.32f, .75f);
    }
  }

  add(r, scene.eagle_neck, translation({0, bob, 0}), kSnow, .7f, 0, 0, 7);
  // A broad white head with a deep hooked beak, golden eyes and a stern brow.
  set_transform(
      r, translation({.24f + flex, 2.80f + bob, sway}) * rotate_z(.03f * std::sin(time * .8f)));
  ball(r, {0, 0, 0}, {.30f, .25f, .25f}, kSnow, 0, .7f);
  ball(r, {.06f, -.10f, 0}, {.23f, .14f, .22f}, kSnow, 0, .75f);
  for (int i = 0; i < 3; ++i)
    ball(r, {-.23f - i * .03f, .02f - i * .07f, 0}, {.12f, .07f, .17f - i * .02f}, kSnow, .5f, .8f);
  // The upper mandible arches forward and tapers into a pointed hook.
  ball(r, {.24f, 0, 0}, {.10f, .085f, .095f}, kGold, 0, .4f);
  ball(r, {.34f, -.005f, 0}, {.12f, .075f, .075f}, kGold, -.15f, .35f);
  ball(r, {.44f, -.03f, 0}, {.08f, .065f, .06f}, kGold, -.50f, .35f);
  add(r, Shape::kCone, between({.48f, -.05f, 0}, {.51f, -.16f, 0}, .048f), kGold, .35f);
  ball(r, {.35f, -.10f, 0}, {.11f, .03f, .06f}, {.92f, .48f, .03f}, -.10f, .45f);
  float open = eye_openness(blink_phase);
  for (int side : {-1, 1}) {
    tube(r, {.20f, -.07f, side * .085f}, {.42f, -.11f, side * .045f}, .006f, {.40f, .18f, .02f});
    ball(r, {.27f, .03f, side * .075f}, {.02f, .012f, .008f}, {.35f, .16f, .02f});
    Vec3 eye{.18f, .05f, side * .19f};
    add(r, Shape::kSphere, transform(eye, {.066f, .07f * open, .03f}), {1.f, .86f, .30f}, .22f);
    Vec3 pupil = eye + Vec3{.012f + .008f * std::sin(time * .73f), 0, side * .022f};
    add(r, Shape::kSphere, transform(pupil, {.035f, .042f * open, .015f}), kInk, .12f);
    ball(r, pupil + Vec3{-.010f, .018f * open, side * .013f}, {.013f, .015f * open, .007f},
         {1.f, .98f, .95f});
    ball(r, {.17f, .145f, side * .18f}, {.11f, .028f, .055f}, kSnow, -.30f, .75f);
  }
  set_transform(r, Mat4{});

  for (int side : {-1, 1}) {
    Vec3 foot = pedal_position(time, side) + Vec3{0, .05f, 0};
    Vec3 hip{-.33f, 1.62f + bob, side * .22f};
    Vec3 d = foot - hip, mid = (foot + hip) * .5f;
    float distance = length(d),
          bend = std::sqrt(std::max(0.f, .60f * .60f - distance * distance * .25f));
    Vec3 knee = mid + unit(Vec3{-d.y, d.x, 0}) * bend;
    // Feathered trousers end above bare golden feet with black talons.
    Vec3 ankle = knee + (foot - knee) * .45f;
    add(r, Shape::kCylinder, between(hip, knee, .075f), kBrown, .8f, 0, 0, 3);
    ball(r, knee, {.095f, .09f, .085f}, kBrown, 0, .8f);
    add(r, Shape::kCylinder, between(knee, ankle, .062f), kBrown, .8f, 0, 0, 3);
    ball(r, ankle, {.07f, .05f, .07f}, kBrown, 0, .85f);
    tube(r, ankle, foot, .032f, kGold);
    ball(r, foot + Vec3{.06f, 0, 0}, {.11f, .045f, .085f}, kGold, .03f);
    for (int i = -1; i <= 1; ++i) {
      Vec3 toe = foot + Vec3{.21f, -.006f, i * .065f};
      tube(r, foot + Vec3{.02f, .01f, i * .025f}, toe, .021f, kGold);
      add(r, Shape::kCone, between(toe, toe + Vec3{.05f, -.035f, i * .01f}, .016f), kInk, .3f);
    }
    Vec3 hallux = foot + Vec3{-.13f, -.004f, 0};
    tube(r, foot, hallux, .021f, kGold);
    add(r, Shape::kCone, between(hallux, hallux + Vec3{-.045f, -.035f, 0}, .016f), kInk, .3f);
  }
}
}

void build_scene(Scene& scene, double time, bool maximum, Bird bird) {
  Renderer& r = *scene.renderer;
  float motion = oscillation_time(time);
  double travel = wrap(time, 44. / 2.6) * 2.6;
  clear_instances(r);
  add(r, Shape::kPlane, transform({0, -.15f, 0}, {55, 1, 55}), {.02f, .2f, .24f}, .2f, .4f, 0, 1);
  add(r, Shape::kPlane, transform({0, -.018f, 0}, {22, 1, 1.16f}), {.27f, .32f, .32f}, .88f);
  add(r, Shape::kPlane, transform({0, .015f, 0}, {22, 1, 1.00f}), {.052f, .081f, .11f}, .83f, 0, 0,
      2);
  for (int side : {-1, 1}) {
    tube(r, {-22, -.01f, side * 1.16f}, {22, -.01f, side * 1.16f}, .035f, {.63f, .66f, .58f});
    for (int i = 0; i < 12; ++i) {
      float x = wrap(i * 3.7 - travel + 220., 44.) - 22.;
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
    float x = wrap(i * 4.7 - travel + 200., 44.) - 22., z = -3.4f - (i % 3) * 2.3f;
    ball(r, {x, -.23f, z}, {1.4f, .31f, 1.2f}, {.37f, .32f, .18f}, 0, .9f);
    palm(r, x, z, 1.65f + (i % 4) * .24f, motion + i);
  }
  for (int i = 0; i < 7; ++i)
    ball(r, {-20.f + i * 7, -.5f, -24.f - i % 2 * 5}, {5.f, .65f + (i % 3) * .3f, 3.f},
         {.025f, .06f, .065f}, 0, .9f);
  for (int i = 0; i < 6; ++i) {
    float x = std::sin(motion * .12f + i) * 9, z = -11.f - i;
    Vec3 p{x, 4.f + (i % 3) * .4f, z};
    for (int side : {-1, 1})
      leaf(r, p, p + Vec3{side * .22f, .09f * std::sin(motion * 3 + i), 0}, .033f,
           {.64f, .78f, .80f});
  }
  bicycle(r, motion);
  float blink_phase = static_cast<float>(wrap(time + 1.7, 4.8));
  if (bird == Bird::kFlamingo) flamingo(scene, motion, blink_phase);
  else if (bird == Bird::kEagle) eagle(scene, motion, blink_phase);
  else pelican(scene, motion, blink_phase);
}

Vec3 camera(double time, float yaw, float aspect, float zoom, float target_height) {
  float angle = yaw + static_cast<float>(wrap(time, 60.)) * kOrbitSpeed;
  float motion = oscillation_time(time);
  float distance = std::max(7.2f, 5.15f / std::max(aspect, .35f));
  distance *= (1.f + .025f * std::sin(motion * .19f)) / zoom;
  float elevation = .29f + .045f * std::sin(motion * .14f);
  return {std::sin(angle) * distance, target_height - .25f + distance * elevation,
          std::cos(angle) * distance};
}

Controls draw_overlay(Renderer& r, Rect safe, int count, int pressed, float fps, bool maximum,
                      bool paused, bool saved, float density, Bird bird) {
  auto layout = layout_overlay(safe, density);
  float s = layout.density;
  Rect panel = layout.panel;
  const auto& controls = layout.controls;
  bool compact = layout.mode == OverlayMode::kCompact;
  bool landscape = layout.mode == OverlayMode::kLandscape;
  Color text{.92f, .96f, .97f}, muted{.74f, .84f, .87f}, mint{.49f, .94f, .76f};
  constexpr const char* kNames[] = {"Pelican", "Flamingo", "Eagle"};
  constexpr const char* kCaptions[] = {"A pelican. A bicycle. No hurry.",
                                       "A flamingo. A bicycle. No hurry.",
                                       "A bald eagle. A bicycle. No hurry."};
  const char* caption = kCaptions[static_cast<int>(bird)];
  char stats[128], number[24];
  if (landscape) {
    float left = safe.x + 24 * s;
    draw_text(r, "Coasting.", left, safe.y + 60 * s, 51 * s, text);
    std::snprintf(stats, sizeof(stats), "%.0f FPS  /  GPU %.1f ms", fps, get_stats(r).gpu_ms);
    if (safe.w >= 640 * s) {
      draw_text(r, caption, left, safe.y + 84 * s, 16 * s, muted);
      draw_text(r, stats, left, safe.y + 108 * s, 16 * s, muted);
    } else {
      draw_text(r, stats, left, safe.y + 84 * s, 16 * s, muted);
    }
  } else if (!compact) {
    float left = panel.x + 10 * s;
    float top = safe.y + 22 * s;
    draw_rect(r, {left, top, 91 * s, 23 * s}, 11 * s, {.08f, .22f, .23f, .48f});
    draw_text(r, "LIVE / 3D", left + 45.5f * s, top + 16 * s, 14 * s, mint, true);
    draw_text(r, "Coasting.", left, top + 72 * s, 51 * s, text);
    if (safe.w >= 340 * s) draw_text(r, caption, left, top + 101 * s, 17 * s, muted);
    std::snprintf(stats, sizeof(stats), "%.0f FPS", fps);
    float stats_x = panel.x + panel.w - 80 * s;
    float stats_y = top + 18 * s;
    draw_text(r, stats, stats_x, stats_y, 20 * s, text);
    if (get_stats(r).gpu_ms > 0)
      std::snprintf(stats, sizeof(stats), "GPU %.1f ms", get_stats(r).gpu_ms);
    else std::snprintf(stats, sizeof(stats), "%s", get_device(r).data());
    draw_text(r, stats, stats_x, stats_y + 19 * s, 12 * s, muted);
  }
  draw_rect(r, panel, 20 * s, {.025f, .075f, .105f, .40f});
  if (compact) {
    std::snprintf(number, sizeof(number), "Count: %d", count);
    bool tight_header = controls.quality.y - panel.y < 32 * s;
    draw_text(r, number, panel.x + 12 * s, panel.y + (tight_header ? 20 : 26) * s,
              (tight_header ? 16 : 20) * s, text);
    if (panel.w >= 300 * s)
      draw_text(r, saved ? "Saved" : "Not saved", panel.x + panel.w - 80 * s, panel.y + 25 * s,
                13 * s, saved ? muted : Color{1.f, .65f, .4f});
  } else {
    draw_text(r, "YOUR COUNT", panel.x + 20 * s, panel.y + 28 * s, 12 * s, muted);
    std::snprintf(number, sizeof(number), "%d", count);
    draw_text(r, number, panel.x + 20 * s, panel.y + 70 * s, 43 * s, text);
    float info_x = landscape ? panel.x + 20 * s : panel.x + panel.w * .75f;
    float info_y = panel.y + (landscape ? 94 : 36) * s;
    draw_text(r, saved ? "Saved automatically" : "Not saved - try again", info_x, info_y, 13 * s,
              saved ? muted : Color{1.f, .65f, .4f}, !landscape);
    std::snprintf(stats, sizeof(stats), "%dx MSAA / %s", get_stats(r).samples,
                  maximum ? "130%" : "100%");
    draw_text(r, stats, info_x, info_y + 21 * s, 12 * s, muted, !landscape);
  }
  auto button = [&](Rect bounds, Control control, const char* label, float font_size) {
    bool add = control == Control::kAdd;
    bool highlighted = pressed == static_cast<int>(control);
    Color background =
        add ? (highlighted ? Color{.28f, .70f, .56f, .82f} : Color{mint.r, mint.g, mint.b, .72f})
            : (highlighted ? Color{.18f, .36f, .36f, .62f} : Color{.06f, .16f, .20f, .38f});
    draw_rect(r, bounds, 16 * s, background);
    draw_text(r, label, bounds.x + bounds.w * .5f, bounds.y + bounds.h * .5f + font_size * s * .32f,
              font_size * s, add ? Color{.035f, .15f, .14f} : text, true);
  };
  button(controls.quality, Control::kQuality, maximum ? "Ultra" : "High", 16);
  button(controls.pause, Control::kPause, paused ? "Resume" : "Pause",
         std::min(18.f, (controls.pause.w / s - 12) / 3.6f));
  button(controls.bird, Control::kBird, kNames[static_cast<int>(bird)],
         std::min(16.f, (controls.bird.w / s - 12) / 4.3f));
  button(controls.add, Control::kAdd, compact ? "+" : "+  Add one", compact ? 24 : 22);
  button(controls.reset, Control::kReset, "Reset", 18);
  return controls;
}

namespace {
Vec3 neck_center(float t, Bird bird) {
  constexpr Vec3 kFlamingo[] = {{-.26f, 1.88f, 0}, {-.12f, 2.10f, 0}, {.20f, 2.42f, 0},
                                {.08f, 2.83f, 0},  {-.10f, 3.19f, 0}, {.04f, 3.53f, 0},
                                {.40f, 3.62f, 0},  {.59f, 3.59f, 0}};
  constexpr Vec3 kPelican[] = {{-.26f, 2.0f, 0},  {-.20f, 2.12f, 0}, {-.04f, 2.42f, 0},
                               {-.06f, 2.72f, 0}, {.10f, 2.97f, 0},  {.43f, 3.12f, 0},
                               {.56f, 3.14f, 0}};
  constexpr Vec3 kEagle[] = {{-.32f, 2.10f, 0}, {-.26f, 2.30f, 0}, {-.15f, 2.52f, 0},
                             {-.02f, 2.68f, 0}, {.12f, 2.77f, 0},  {.20f, 2.79f, 0},
                             {.30f, 2.80f, 0}};
  std::span<const Vec3> points = bird == Bird::kFlamingo ? std::span<const Vec3>{kFlamingo}
                                 : bird == Bird::kEagle  ? std::span<const Vec3>{kEagle}
                                                         : std::span<const Vec3>{kPelican};
  int segments = points.size() - 3;
  float segment = std::clamp(t, 0.f, 1.f) * segments;
  int i = std::min(segments - 1, int(segment));
  float u = segment - i, u2 = u * u, u3 = u2 * u;
  Vec3 a = points[i], b = points[i + 1], c = points[i + 2], d = points[i + 3];
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

// HARKER in block capitals on a 6-by-10 grid, each stroke a counterclockwise
// convex quad.
struct Stroke {
  float x[4], y[4];
};

constexpr Stroke kH[] = {
    {{0, 2, 2, 0}, {0, 0, 10, 10}}, {{4, 6, 6, 4}, {0, 0, 10, 10}}, {{2, 4, 4, 2}, {4, 4, 6, 6}}};
constexpr Stroke kA[] = {{{0, 2, 2, 0}, {0, 0, 8, 8}},
                         {{4, 6, 6, 4}, {0, 0, 8, 8}},
                         {{0, 6, 4.5f, 1.5f}, {8, 8, 10, 10}},
                         {{2, 4, 4, 2}, {3.5f, 3.5f, 5.5f, 5.5f}}};
constexpr Stroke kR[] = {
    {{0, 2, 2, 0}, {0, 0, 10, 10}},          {{2, 4, 4.5f, 2}, {8, 8, 10, 10}},
    {{4, 6, 6, 4.5f}, {8, 8, 8.5f, 10}},     {{4, 6, 6, 4}, {6, 6, 8, 8}},
    {{4, 4.5f, 6, 4}, {4.5f, 4.5f, 6, 6}},   {{2, 4, 4, 2}, {4.5f, 4.5f, 6.5f, 6.5f}},
    {{4, 6, 4.4f, 2.4f}, {0, 0, 4.5f, 4.5f}}};
constexpr Stroke kK[] = {{{0, 2, 2, 0}, {0, 0, 10, 10}},
                         {{2, 6, 3.6f, 2}, {4, 10, 10, 7}},
                         {{3.8f, 6, 4.2f, 2.4f}, {0, 0, 6.2f, 4.6f}}};
constexpr Stroke kE[] = {{{0, 2, 2, 0}, {0, 0, 10, 10}},
                         {{2, 6, 6, 2}, {8, 8, 10, 10}},
                         {{2, 5, 5, 2}, {4, 4, 6, 6}},
                         {{2, 6, 6, 2}, {0, 0, 2, 2}}};
constexpr std::span<const Stroke> kHarker[] = {kH, kA, kR, kK, kE, kR};

// Print the word onto the front of the shirt ellipsoid, in its unrotated frame.
// Subdivided strokes are projected along its forward axis, so the word keeps
// its shape from the front, and lifted along the surface normal.
Result<MeshId> create_lettering(Renderer& r) {
  std::vector<Vertex> vertices;
  std::vector<unsigned> indices;
  constexpr int kSteps = 3;
  constexpr float kUnit = .012f, kGap = 1.5f, kAdvance = 6 + kGap, kMiddle = .14f, kLift = .007f;
  constexpr float kLeft = -(std::size(kHarker) * kAdvance - kGap) * .5f;
  for (size_t letter = 0; letter < std::size(kHarker); ++letter)
    for (const Stroke& stroke : kHarker[letter]) {
      unsigned first = vertices.size();
      for (int i = 0; i <= kSteps; ++i)
        for (int j = 0; j <= kSteps; ++j) {
          float u = float(i) / kSteps, v = float(j) / kSteps;
          auto mix = [&](const float* corner) {
            return (corner[0] * (1 - u) + corner[1] * u) * (1 - v) +
                   (corner[3] * (1 - u) + corner[2] * u) * v;
          };
          // The word reads left to right from the front, toward -Z.
          float y = kMiddle + (mix(stroke.y) - 5) * kUnit;
          float z = -(kLeft + letter * kAdvance + mix(stroke.x)) * kUnit;
          Vec3 d{0, y / kShirtSize.y, z / kShirtSize.z};
          d.x = std::sqrt(std::max(0.f, 1 - d.y * d.y - d.z * d.z));
          Vec3 normal = unit({d.x / kShirtSize.x, d.y / kShirtSize.y, d.z / kShirtSize.z});
          vertices.push_back({Vec3{d.x * kShirtSize.x, y, z} + normal * kLift, normal});
        }
      for (int i = 0; i < kSteps; ++i)
        for (int j = 0; j < kSteps; ++j) {
          unsigned a = first + i * (kSteps + 1) + j, b = a + kSteps + 1;
          indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
  return create_mesh(r, vertices, indices);
}

Result<MeshId> create_neck(Renderer& r, Bird bird) {
  std::vector<Vertex> vertices;
  std::vector<unsigned> indices;
  constexpr int kRings = 64, kSides = 32;
  for (int ring = 0; ring <= kRings; ++ring) {
    float t = float(ring) / kRings;
    Vec3 center = neck_center(t, bird);
    Vec3 tangent = unit(neck_center(std::min(1.f, t + .001f), bird) -
                        neck_center(std::max(0.f, t - .001f), bird));
    Vec3 across = unit(cross(tangent, {0, 0, 1}));
    float radius =
        bird == Bird::kFlamingo ? .095f + .075f * std::pow(1 - t, 3.f) + .020f * std::pow(t, 4.f)
        : bird == Bird::kEagle  ? .17f + .06f * std::pow(1 - t, 3.f)
                                : .17f + .14f * std::pow(1 - t, 3.f) + .075f * std::pow(t, 4.f);
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
}

Result<Owner<Scene>> create_scene(Renderer& renderer) {
  auto pelican_neck = create_neck(renderer, Bird::kPelican);
  if (!pelican_neck) return std::unexpected(pelican_neck.error());
  auto flamingo_neck = create_neck(renderer, Bird::kFlamingo);
  if (!flamingo_neck) return std::unexpected(flamingo_neck.error());
  auto eagle_neck = create_neck(renderer, Bird::kEagle);
  if (!eagle_neck) return std::unexpected(eagle_neck.error());
  auto scarf = create_scarf(renderer);
  if (!scarf) return std::unexpected(scarf.error());
  auto lettering = create_lettering(renderer);
  if (!lettering) return std::unexpected(lettering.error());
  Owner<Scene> scene(new (std::nothrow) Scene{
      &renderer, *pelican_neck, *flamingo_neck, *eagle_neck, *scarf, *lettering, {}});
  if (!scene) return std::unexpected(Error{"Cannot allocate scene state"});
  return scene;
}

void destroy(Scene* scene) noexcept { delete scene; }

int hit_test(const Scene& scene, float x, float y) {
  return static_cast<int>(hit_test(scene.controls, x, y));
}

Result<bool> render_scene(Scene& scene, double time, float yaw, bool maximum, int count,
                          int pressed, float fps, bool paused, Rect content, bool saved,
                          bool overlay, float density, float zoom, Bird bird) {
  if (!std::isfinite(zoom) || zoom < kMinimumZoom || zoom > kMaximumZoom)
    return std::unexpected(Error{"Invalid camera zoom"});
  if (bird != Bird::kPelican && bird != Bird::kFlamingo && bird != Bird::kEagle)
    return std::unexpected(Error{"Invalid bird selection"});
  Renderer& r = *scene.renderer;
  if (auto result = prepare_frame(r, maximum); !result || !*result) return result;
  build_scene(scene, time, maximum, bird);
  auto stats = get_stats(r);
  Rect safe = safe_area(content, stats.width, stats.height);
  // Insets and control placement affect only the overlay. The camera always
  // uses the full window, so the world continues behind every control.
  float target_height = bird == Bird::kFlamingo ? 1.80f : bird == Bird::kEagle ? 1.60f : 1.55f;
  if (auto result =
          render(r, camera(time, yaw, float(stats.width) / stats.height, zoom, target_height),
                 {0, target_height, 0}, time, maximum);
      !result || !*result)
    return result;
  Controls controls =
      overlay ? draw_overlay(r, safe, count, pressed, fps, maximum, paused, saved, density, bird)
              : layout_controls(safe, density);
  auto presented = present(r);
  if (presented && *presented) scene.controls = controls;
  return presented;
}
}
