// On-screen football gamepad for touch screens (iPhone / iPad; also usable on a touch PC with
// --touch_controls=true). A synthetic input device the SDK merges into guest user 0, and its
// overlay, drawn as an ImGui dialog on top of the game.
//
// Layout (landscape, sizes in units of the screen height so it is the same on every phone):
//  - left side: a floating move stick (it centres where the thumb lands). Pushing past the outer
//    ring also holds Sprint (--touch_sprint_on_push), the usual phone football control;
//  - right side: the four face buttons in the Xbox diamond, labelled with the game's own actions,
//    a large Sprint (RT) button and the LT / LB / RB modifiers; a touch that starts on free space
//    on the right is a floating right stick (skill moves);
//  - top: D-pad (tactics) on the left, Back / Start in the middle.
// A finger that slides from one face button onto another presses the new one (pass -> shoot
// without lifting). The overlay hides while a real controller is connected (Bluetooth pads).
#pragma once

#include <memory>

namespace rex::input {
class InputDriver;
}
namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace rexios::touch {

// Per-game captions under the button letters (nullptr: letter only).
struct Labels {
  const char* a = "Pass";
  const char* b = "Lob";
  const char* x = "Shoot";
  const char* y = "Through";
  const char* lt = "LT";
  const char* rt = "Sprint";
  const char* lb = "LB";
  const char* rb = "RB";
  const char* right_stick = "Skills";
};

// Call once before the input system is created (ReXApp::OnPreSetup).
void SetLabels(const Labels& labels);

// The touch device; add it to the SDK's input system (RuntimeConfig::input_factory).
std::unique_ptr<rex::input::InputDriver> CreateDriver();

// The overlay; create it in ReXApp::OnCreateDialogs and keep it alive until OnShutdown.
std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer);

// Whether the overlay is shown (cvar touch_controls, hidden while a real pad is connected).
bool Visible();

}  // namespace rexios::touch
