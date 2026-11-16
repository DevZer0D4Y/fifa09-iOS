// On-screen football gamepad (see touch_pad.h). Built on the GTA V port's touch controls
// (gtav/src/native/touch_controls.cpp in gtav-recomp): same device model and finger tracking,
// with a football layout, sprint-on-push and button sliding.
#include "touch_pad.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/ui/imgui_dialog.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define REXIOS_TOUCH_DEFAULT true
#define REXIOS_TOUCH_INSET_DEFAULT 0.10  // rounded corners and the notch / Dynamic Island
#else
#define REXIOS_TOUCH_DEFAULT false
#define REXIOS_TOUCH_INSET_DEFAULT 0.0
#endif

REXCVAR_DEFINE_BOOL(touch_controls, REXIOS_TOUCH_DEFAULT, "Touch",
                    "Show the on-screen gamepad (hidden while a controller is connected)");
REXCVAR_DEFINE_DOUBLE(touch_scale, 1.0, "Touch", "On-screen gamepad: size of the controls (0.6-1.3)");
REXCVAR_DEFINE_DOUBLE(touch_inset, REXIOS_TOUCH_INSET_DEFAULT, "Touch",
                      "On-screen gamepad: extra distance from the left / right edges (screen heights)");
REXCVAR_DEFINE_DOUBLE(touch_opacity, 0.55, "Touch", "On-screen gamepad: opacity (0.1-1)");
REXCVAR_DEFINE_BOOL(touch_sprint_on_push, true, "Touch",
                    "On-screen gamepad: pushing the move stick past its ring also holds Sprint (RT)");
REXCVAR_DEFINE_BOOL(touch_button_slide, true, "Touch",
                    "On-screen gamepad: a finger sliding onto another face button presses that one");

namespace rexios::touch {
namespace {

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

enum Kind { kButton, kTrigger };
enum Group { kFace, kShoulder, kSystem, kDpad };

struct Control {
  const char* letter;
  Kind kind;
  Group group;
  uint16_t button;  // X_INPUT_GAMEPAD_* for kButton
  bool right;       // kTrigger: right trigger
  float x, y, r;    // centre and radius in units of h; x < 0 counts from the right edge
  ImU32 tint;       // face buttons: the Xbox colours
};

// Positions in units of the screen height; negative x is measured from the right edge.
// Back / Start x are set at runtime (around the centre).
const Control kControls[] = {
    {"A", kButton, kFace, X_INPUT_GAMEPAD_A, false, -0.40f, 0.85f, 0.078f, IM_COL32(96, 184, 64, 255)},
    {"B", kButton, kFace, X_INPUT_GAMEPAD_B, false, -0.26f, 0.71f, 0.078f, IM_COL32(222, 64, 56, 255)},
    {"X", kButton, kFace, X_INPUT_GAMEPAD_X, false, -0.54f, 0.71f, 0.078f, IM_COL32(64, 128, 232, 255)},
    {"Y", kButton, kFace, X_INPUT_GAMEPAD_Y, false, -0.40f, 0.57f, 0.078f, IM_COL32(240, 196, 40, 255)},
    {"RT", kTrigger, kShoulder, 0, true, -0.12f, 0.86f, 0.105f, IM_COL32(255, 255, 255, 255)},
    {"RB", kButton, kShoulder, X_INPUT_GAMEPAD_RIGHT_SHOULDER, false, -0.12f, 0.55f, 0.065f,
     IM_COL32(255, 255, 255, 255)},
    {"LB", kButton, kShoulder, X_INPUT_GAMEPAD_LEFT_SHOULDER, false, -0.26f, 0.42f, 0.060f,
     IM_COL32(255, 255, 255, 255)},
    {"LT", kTrigger, kShoulder, 0, false, -0.64f, 0.88f, 0.065f, IM_COL32(255, 255, 255, 255)},
    {"R3", kButton, kShoulder, X_INPUT_GAMEPAD_RIGHT_THUMB, false, -0.12f, 0.33f, 0.050f,
     IM_COL32(255, 255, 255, 255)},
    {"^", kButton, kDpad, X_INPUT_GAMEPAD_DPAD_UP, false, 0.30f, 0.21f, 0.048f, IM_COL32(255, 255, 255, 255)},
    {"v", kButton, kDpad, X_INPUT_GAMEPAD_DPAD_DOWN, false, 0.30f, 0.39f, 0.048f, IM_COL32(255, 255, 255, 255)},
    {"<", kButton, kDpad, X_INPUT_GAMEPAD_DPAD_LEFT, false, 0.21f, 0.30f, 0.048f, IM_COL32(255, 255, 255, 255)},
    {">", kButton, kDpad, X_INPUT_GAMEPAD_DPAD_RIGHT, false, 0.39f, 0.30f, 0.048f, IM_COL32(255, 255, 255, 255)},
    {"Back", kButton, kSystem, X_INPUT_GAMEPAD_BACK, false, 0.0f, 0.07f, 0.050f, IM_COL32(255, 255, 255, 255)},
    {"Start", kButton, kSystem, X_INPUT_GAMEPAD_START, false, 0.0f, 0.07f, 0.050f, IM_COL32(255, 255, 255, 255)},
};
constexpr int kControlCount = int(sizeof(kControls) / sizeof(kControls[0]));
constexpr float kStickRadius = 0.13f;     // full deflection, units of h
constexpr float kSprintPush = 1.25f;      // move stick pushed this far past full deflection: sprint
constexpr float kStickDeadzone = 0.08f;   // a resting thumb wobbles

enum Owner { kNone = -1, kLeftStick = -2, kRightStick = -3 };

struct Finger {
  int owner = kNone;  // control index, or a stick
  float x0 = 0, y0 = 0, x = 0, y = 0;  // pixels
};

std::mutex g_mutex;
std::unordered_map<SDL_FingerID, Finger> g_fingers;
std::atomic<float> g_w{0}, g_h{0};
std::atomic<uint32_t> g_packet{0};
std::atomic<bool> g_watch_added{false};
Labels g_labels;

float Scale() { return float(std::clamp(REXCVAR_GET(touch_scale), 0.6, 1.3)); }
// The side inset (notch / rounded corners) gives way on narrow screens (4:3 iPads), where the
// left and right groups would otherwise meet.
float Inset(float w, float h) {
  const float wanted = float(std::clamp(REXCVAR_GET(touch_inset), 0.0, 0.4));
  const float room = h > 0 ? (w / h - 1.2f * Scale()) * 0.5f : 0.0f;
  return std::clamp(room, 0.0f, wanted);
}
float Opacity() { return float(std::clamp(REXCVAR_GET(touch_opacity), 0.1, 1.0)); }

const char* Caption(int i) {
  const Control& c = kControls[i];
  if (c.kind == kTrigger) return c.right ? g_labels.rt : g_labels.lt;
  switch (c.button) {
    case X_INPUT_GAMEPAD_A: return g_labels.a;
    case X_INPUT_GAMEPAD_B: return g_labels.b;
    case X_INPUT_GAMEPAD_X: return g_labels.x;
    case X_INPUT_GAMEPAD_Y: return g_labels.y;
    case X_INPUT_GAMEPAD_LEFT_SHOULDER: return g_labels.lb;
    case X_INPUT_GAMEPAD_RIGHT_SHOULDER: return g_labels.rb;
    default: return nullptr;
  }
}

float CenterX(int i, float w, float h) {
  const Control& c = kControls[i];
  const float s = Scale();
  if (c.button == X_INPUT_GAMEPAD_BACK) return w * 0.5f - 0.11f * s * h;
  if (c.button == X_INPUT_GAMEPAD_START) return w * 0.5f + 0.11f * s * h;
  const float from_edge = (std::fabs(c.x) * s + Inset(w, h)) * h;
  return c.x < 0 ? w - from_edge : from_edge;
}

// The D-pad and Back / Start hang from the top edge, everything else stands on the bottom edge:
// scaled, each group grows from its own edge, so spacing grows with the buttons.
float CenterY(int i, float h) {
  const Control& c = kControls[i];
  const float s = Scale();
  const float r = c.r * s;
  const bool top = c.group == kDpad || c.group == kSystem;
  const float y = top ? c.y * s : 1.0f - (1.0f - c.y) * s;
  return std::clamp(y, r + 0.02f, 1.0f - r - 0.02f) * h;
}

float Radius(int i, float h) { return kControls[i].r * Scale() * h; }

int Hit(float x, float y, float w, float h, float slack) {
  int best = kNone;
  float best_d = FLT_MAX;
  for (int i = 0; i < kControlCount; i++) {
    float dx = x - CenterX(i, w, h), dy = y - CenterY(i, h);
    float r = Radius(i, h) * slack;  // fingers are wide
    float d = dx * dx + dy * dy;
    if (d <= r * r && d < best_d) best = i, best_d = d;
  }
  return best;
}

bool OnEvent(void*, SDL_Event* e) {
  if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
      e->type != SDL_EVENT_FINGER_CANCELED)
    return true;
  if (!Visible()) return true;
  const float w = g_w.load(), h = g_h.load();
  if (w <= 0 || h <= 0) return true;
  const float x = e->tfinger.x * w, y = e->tfinger.y * h;
  std::lock_guard lock(g_mutex);
  if (e->type == SDL_EVENT_FINGER_DOWN) {
    Finger f;
    f.owner = Hit(x, y, w, h, 1.2f);
    if (f.owner == kNone) f.owner = x < w * 0.5f ? kLeftStick : kRightStick;
    f.x0 = f.x = x;
    f.y0 = f.y = y;
    g_fingers[e->tfinger.fingerID] = f;
  } else if (e->type == SDL_EVENT_FINGER_MOTION) {
    auto it = g_fingers.find(e->tfinger.fingerID);
    if (it == g_fingers.end()) return true;
    Finger& f = it->second;
    f.x = x;
    f.y = y;
    if (f.owner >= 0 && kControls[f.owner].group == kFace && REXCVAR_GET(touch_button_slide)) {
      // Exact radius here (no slack), so a finger resting between two buttons does not flicker.
      int over = Hit(x, y, w, h, 1.0f);
      if (over >= 0 && over != f.owner && kControls[over].group == kFace) f.owner = over;
    }
  } else {
    g_fingers.erase(e->tfinger.fingerID);
  }
  g_packet++;
  return true;
}

int16_t Axis(float v) {
  v = std::clamp(v, -1.0f, 1.0f);
  return int16_t(std::lround(v * 32767.0f));
}

void Fill(X_INPUT_GAMEPAD* pad) {
  const float h = g_h.load();
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;
  std::lock_guard lock(g_mutex);
  for (auto& [id, f] : g_fingers) {
    if (f.owner >= 0) {
      const Control& c = kControls[f.owner];
      if (c.kind == kTrigger)
        (c.right ? rt : lt) = 0xFF;
      else
        buttons |= c.button;
    } else if (h > 0) {
      const float R = kStickRadius * Scale() * h;
      float dx = (f.x - f.x0) / R, dy = -(f.y - f.y0) / R;
      float len = std::sqrt(dx * dx + dy * dy);
      if (f.owner == kLeftStick && len >= kSprintPush && REXCVAR_GET(touch_sprint_on_push)) rt = 0xFF;
      if (len < kStickDeadzone) {
        dx = dy = 0;
      } else if (len > 1.0f) {
        dx /= len, dy /= len;
      }
      if (f.owner == kLeftStick)
        lx = dx, ly = dy;
      else
        rx = dx, ry = dy;
    }
  }
  pad->buttons = buttons;
  pad->left_trigger = lt;
  pad->right_trigger = rt;
  pad->thumb_lx = Axis(lx);
  pad->thumb_ly = Axis(ly);
  pad->thumb_rx = Axis(rx);
  pad->thumb_ry = Axis(ry);
}

bool RealPadConnected() {
  int count = 0;
  SDL_JoystickID* pads = SDL_GetGamepads(&count);
  SDL_free(pads);
  return count > 0;
}

void EnsureEventWatch() {
  if (!g_watch_added.exchange(true)) SDL_AddEventWatch(OnEvent, nullptr);
}

constexpr DeviceId kTouchDevice = static_cast<DeviceId>(0x544F5543);  // "TOUC"

class TouchDriver final : public InputDriver {
 public:
  TouchDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    if (!REXCVAR_GET(touch_controls)) return;
    DeviceInfo info;
    info.id = kTouchDevice;
    info.name = "Touch screen";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    if (id != kTouchDevice || !REXCVAR_GET(touch_controls)) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = g_packet.load();
      Fill(&out->gamepad);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (id != kTouchDevice || !REXCVAR_GET(touch_controls)) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      caps->gamepad.buttons = 0xF3FF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = int16_t(0x7FFF);
      caps->gamepad.thumb_ly = int16_t(0x7FFF);
      caps->gamepad.thumb_rx = int16_t(0x7FFF);
      caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kTouchDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kTouchDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

ImU32 WithAlpha(ImU32 c, float a) {
  const uint32_t alpha = uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

void DrawPad(ImDrawList* list, float w, float h) {
  g_w = w;
  g_h = h;
  EnsureEventWatch();
  bool held[kControlCount] = {};
  bool sprint_by_push = false;
  struct Stick {
    bool on;
    float x0, y0, x, y;
  } sticks[2] = {};
  {
    std::lock_guard lock(g_mutex);
    for (auto& [id, f] : g_fingers) {
      if (f.owner >= 0) {
        held[f.owner] = true;
        continue;
      }
      sticks[f.owner == kLeftStick ? 0 : 1] = {true, f.x0, f.y0, f.x, f.y};
      if (f.owner == kLeftStick && REXCVAR_GET(touch_sprint_on_push)) {
        const float R = kStickRadius * Scale() * h;
        const float dx = f.x - f.x0, dy = f.y - f.y0;
        sprint_by_push |= dx * dx + dy * dy >= (kSprintPush * R) * (kSprintPush * R);
      }
    }
  }
  const float o = Opacity();
  const ImU32 white = IM_COL32(255, 255, 255, 255);
  ImFont* font = ImGui::GetFont();
  for (int i = 0; i < kControlCount; i++) {
    const Control& c = kControls[i];
    const bool on = held[i] || (c.kind == kTrigger && c.right && sprint_by_push);
    const ImVec2 p(CenterX(i, w, h), CenterY(i, h));
    const float r = Radius(i, h);
    list->AddCircleFilled(p, r, WithAlpha(c.tint, (on ? 0.55f : 0.18f) * o), 40);
    list->AddCircle(p, r, WithAlpha(c.group == kFace ? c.tint : white, 0.85f * o), 40, std::max(1.5f, r * 0.05f));
    const char* caption = c.group == kDpad || c.group == kSystem ? nullptr : Caption(i);
    const float letter_size = r * (caption ? 0.72f : 0.62f);
    ImVec2 ts = font->CalcTextSizeA(letter_size, FLT_MAX, 0.0f, c.letter);
    const float letter_y = caption ? p.y - ts.y * 0.72f : p.y - ts.y * 0.5f;
    list->AddText(font, letter_size, ImVec2(p.x - ts.x * 0.5f, letter_y), WithAlpha(white, 0.95f * o), c.letter);
    if (caption && std::strcmp(caption, c.letter) != 0) {
      const float cap_size = r * 0.34f;
      ImVec2 cs = font->CalcTextSizeA(cap_size, FLT_MAX, 0.0f, caption);
      list->AddText(font, cap_size, ImVec2(p.x - cs.x * 0.5f, p.y + r * 0.18f), WithAlpha(white, 0.85f * o), caption);
    }
  }
  const float R = kStickRadius * Scale() * h;
  // Resting hint for the move stick: where the left thumb goes.
  if (!sticks[0].on) {
    const ImVec2 hint((0.36f * Scale() + Inset(w, h)) * h, (1.0f - 0.30f * Scale()) * h);
    list->AddCircle(hint, R, WithAlpha(white, 0.30f * o), 48, 2.0f);
    list->AddCircleFilled(hint, R * 0.38f, WithAlpha(white, 0.15f * o), 32);
  }
  for (int s = 0; s < 2; s++) {
    if (!sticks[s].on) continue;
    const Stick& st = sticks[s];
    const ImVec2 base(st.x0, st.y0);
    list->AddCircleFilled(base, R, WithAlpha(white, 0.10f * o), 48);
    list->AddCircle(base, R, WithAlpha(white, 0.60f * o), 48, 2.0f);
    if (s == 0 && REXCVAR_GET(touch_sprint_on_push))
      list->AddCircle(base, R * kSprintPush, WithAlpha(white, (sprint_by_push ? 0.7f : 0.22f) * o), 48, 1.5f);
    float dx = st.x - st.x0, dy = st.y - st.y0, len = std::sqrt(dx * dx + dy * dy);
    if (len > R) dx *= R / len, dy *= R / len;
    list->AddCircleFilled(ImVec2(st.x0 + dx, st.y0 + dy), R * 0.40f, WithAlpha(white, 0.50f * o), 32);
    if (s == 1 && g_labels.right_stick) {
      const float cap_size = R * 0.26f;
      ImVec2 cs = font->CalcTextSizeA(cap_size, FLT_MAX, 0.0f, g_labels.right_stick);
      list->AddText(font, cap_size, ImVec2(base.x - cs.x * 0.5f, base.y - R - cs.y - 4.0f),
                    WithAlpha(white, 0.75f * o), g_labels.right_stick);
    }
  }
}

class TouchOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!Visible()) {
      // A controller was connected mid-touch: no button stays held.
      std::lock_guard lock(g_mutex);
      if (!g_fingers.empty()) {
        g_fingers.clear();
        g_packet++;
      }
      return;
    }
    DrawPad(ImGui::GetForegroundDrawList(), io.DisplaySize.x, io.DisplaySize.y);
  }
};

}  // namespace

void SetLabels(const Labels& labels) { g_labels = labels; }

std::unique_ptr<rex::input::InputDriver> CreateDriver() { return std::make_unique<TouchDriver>(); }

std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<TouchOverlay>(drawer);
}

bool Visible() {
  if (!REXCVAR_GET(touch_controls)) return false;
  // Re-checked twice a second: a pad connected over Bluetooth hides the overlay.
  static std::atomic<uint64_t> next_check{0};
  static std::atomic<bool> pad{false};
  const uint64_t now = SDL_GetTicks();
  if (now >= next_check.load()) {
    next_check = now + 500;
    pad = RealPadConnected();
  }
  return !pad.load();
}

}  // namespace rexios::touch
