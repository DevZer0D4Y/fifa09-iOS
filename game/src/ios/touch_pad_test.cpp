// Desktop test of the on-screen gamepad's logic (no phone needed): hit testing, sticks,
// sprint-on-push, button sliding, multi-touch, the controller hand-off, and that no two
// controls overlap or leave the screen on any phone / iPad shape and size.
// Build (Linux or macOS, against the SDK source tree with its submodules):
//   clang++ -std=c++23 -DFMT_HEADER_ONLY -I$SDK/include -I$SDK/thirdparty/fmt/include \
//     -I$SDK/thirdparty/spdlog/include -I$SDK/thirdparty/imgui -I$SDK/thirdparty/sdl3/include \
//     -I$SDK/thirdparty/xxHash -I$SDK/thirdparty/simde touch_pad_test.cpp \
//     $SDK/thirdparty/imgui/imgui*.cpp -o touch_pad_test && ./touch_pad_test
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "touch_pad.cpp"
#include <cassert>
#include <cstdio>

// Link stubs: the cvar registry and ImGuiDialog live in the SDK runtime.
namespace rex::cvar {
std::optional<size_t> RegisterFlag(FlagEntry) { return std::nullopt; }
void UnregisterFlag(std::string_view) {}
}  // namespace rex::cvar
namespace rex::ui {
ImGuiDialog::ImGuiDialog(ImGuiDrawer* d) : imgui_drawer_(d) {}
ImGuiDialog::~ImGuiDialog() {}
}  // namespace rex::ui

static uint64_t g_fake_ticks = 0;
static int g_fake_pads = 0;
extern "C" {
bool SDL_AddEventWatch(SDL_EventFilter, void*) { return true; }
SDL_JoystickID* SDL_GetGamepads(int* count) { *count = g_fake_pads; return nullptr; }
void SDL_free(void*) {}
Uint64 SDL_GetTicks(void) { return g_fake_ticks; }
}

using namespace rexios::touch;
static const float W = 2340, H = 1080;  // iPhone 13-ish landscape

static void Ev(Uint32 type, SDL_FingerID id, float px, float py) {
  SDL_Event e{};
  e.type = type;
  e.tfinger.fingerID = id;
  e.tfinger.x = px / W;
  e.tfinger.y = py / H;
  OnEvent(nullptr, &e);
}
static X_INPUT_GAMEPAD Pad() {
  X_INPUT_GAMEPAD p{};
  Fill(&p);
  return p;
}
static ImVec2 C(const char* letter) {
  for (int i = 0; i < kControlCount; i++)
    if (!std::strcmp(kControls[i].letter, letter)) return ImVec2(CenterX(i, W, H), CenterY(i, H));
  assert(false);
  return {};
}

static int LayoutProblems() {
  int bad = 0;
  for (float aspect : {2.16f, 1.78f, 1.43f, 1.33f})
    for (double scale : {0.6, 0.8, 1.0, 1.15, 1.3})
      for (double inset : {0.0, 0.10, 0.2}) {
        REXCVAR_SET(touch_scale, scale);
        REXCVAR_SET(touch_inset, inset);
        float H = 1000, W = H * aspect;
        for (int i = 0; i < kControlCount; i++) {
          float x = CenterX(i, W, H), y = CenterY(i, H), r = Radius(i, H);
          if (x - r < 0 || x + r > W) { std::printf("offscreen %s a=%.2f s=%.1f i=%.2f\n", kControls[i].letter, aspect, scale, inset); bad++; }
          for (int j = i + 1; j < kControlCount; j++) {
            float dx = x - CenterX(j, W, H), dy = y - CenterY(j, H);
            if (std::sqrt(dx * dx + dy * dy) < r + Radius(j, H)) {
              std::printf("overlap %s/%s a=%.2f s=%.1f i=%.2f\n", kControls[i].letter, kControls[j].letter, aspect, scale, inset);
              bad++;
            }
          }
        }
      }
  return bad;
}

int main() {
  if (int bad = LayoutProblems()) {
    std::printf("%d layout problems\n", bad);
    return 1;
  }
  REXCVAR_SET(touch_scale, 1.0);
  REXCVAR_SET(touch_inset, 0.10);
  REXCVAR_SET(touch_controls, true);
  g_w = W, g_h = H;
  // No two controls overlap (with the hit slack only between different groups' neighbours).
  for (int i = 0; i < kControlCount; i++)
    for (int j = i + 1; j < kControlCount; j++) {
      float dx = CenterX(i, W, H) - CenterX(j, W, H), dy = CenterY(i, H) - CenterY(j, H);
      float d = std::sqrt(dx * dx + dy * dy);
      if (d < Radius(i, H) + Radius(j, H)) {
        std::printf("overlap %s %s\n", kControls[i].letter, kControls[j].letter);
        return 1;
      }
    }
  // Every control is on screen.
  for (int i = 0; i < kControlCount; i++) {
    float x = CenterX(i, W, H), y = CenterY(i, H), r = Radius(i, H);
    assert(x - r >= 0 && x + r <= W && y - r >= 0 && y + r <= H);
  }
  // Tap A.
  auto a = C("A");
  Ev(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
  assert(Pad().buttons == X_INPUT_GAMEPAD_A);
  // Slide onto X: X replaces A.
  auto x = C("X");
  Ev(SDL_EVENT_FINGER_MOTION, 1, (a.x + x.x) / 2, (a.y + x.y) / 2);  // between: still A
  assert(Pad().buttons == X_INPUT_GAMEPAD_A);
  Ev(SDL_EVENT_FINGER_MOTION, 1, x.x, x.y);
  assert(Pad().buttons == X_INPUT_GAMEPAD_X);
  Ev(SDL_EVENT_FINGER_UP, 1, x.x, x.y);
  assert(Pad().buttons == 0);
  // Sprint button = RT.
  auto rt = C("RT");
  Ev(SDL_EVENT_FINGER_DOWN, 2, rt.x, rt.y);
  assert(Pad().right_trigger == 0xFF && Pad().buttons == 0);
  Ev(SDL_EVENT_FINGER_UP, 2, rt.x, rt.y);
  // Move stick: floating, centred where the thumb lands; up = +Y.
  const float R = kStickRadius * H;
  Ev(SDL_EVENT_FINGER_DOWN, 3, 400, 800);
  assert(Pad().thumb_lx == 0 && Pad().thumb_ly == 0);
  Ev(SDL_EVENT_FINGER_MOTION, 3, 400 + R * 0.5f, 800);
  assert(std::abs(Pad().thumb_lx - 16384) < 200 && Pad().right_trigger == 0);
  Ev(SDL_EVENT_FINGER_MOTION, 3, 400, 800 - R);
  assert(Pad().thumb_ly == 32767 && Pad().right_trigger == 0);
  // Past the ring: full deflection and sprint.
  Ev(SDL_EVENT_FINGER_MOTION, 3, 400, 800 - R * 1.4f);
  assert(Pad().thumb_ly == 32767 && Pad().right_trigger == 0xFF);
  // Resting wobble inside the deadzone reads as centred.
  Ev(SDL_EVENT_FINGER_MOTION, 3, 400 + R * 0.05f, 800);
  assert(Pad().thumb_lx == 0);
  // Second finger on Y while moving: both.
  auto y = C("Y");
  Ev(SDL_EVENT_FINGER_MOTION, 3, 400 - R, 800);
  Ev(SDL_EVENT_FINGER_DOWN, 4, y.x, y.y);
  assert(Pad().buttons == X_INPUT_GAMEPAD_Y && Pad().thumb_lx == -32767);
  // Third finger on free space on the right: skill stick.
  Ev(SDL_EVENT_FINGER_DOWN, 5, W * 0.62f, H * 0.45f);
  Ev(SDL_EVENT_FINGER_MOTION, 5, W * 0.62f, H * 0.45f + R);
  assert(Pad().thumb_ry == -32767);
  Ev(SDL_EVENT_FINGER_CANCELED, 3, 0, 0);
  Ev(SDL_EVENT_FINGER_UP, 4, 0, 0);
  Ev(SDL_EVENT_FINGER_UP, 5, 0, 0);
  X_INPUT_GAMEPAD p = Pad();
  assert(p.buttons == 0 && p.thumb_lx == 0 && p.thumb_ry == 0 && p.right_trigger == 0);
  // A controller connected: touches are ignored.
  g_fake_pads = 1;
  g_fake_ticks = 1000;
  assert(!Visible());
  Ev(SDL_EVENT_FINGER_DOWN, 6, a.x, a.y);
  assert(Pad().buttons == 0);
  std::printf("touch pad tests passed\n");
  return 0;
}
