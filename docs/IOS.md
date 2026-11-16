# FIFA 09 on iPhone and iPad

Status: **port code written, not yet run on a device.** The touch controls are tested on a desktop
(`game/src/ios/touch_pad_test.cpp`); the app, the SDK patch and the display-clock code are
compile-checked but have not been built with Xcode or played on an iPhone yet. See
[Open points](#open-points) before relying on it.

The iOS port reuses the iOS platform layer of the GTA V recompilation (`rexglue-sdk-gtav-linux.patch`
in gtav-recomp: UIKit main thread, guest memory in the app sandbox, MoltenVK, `args.txt`), merged
onto this project's SDK patch.

## What it adds

| | |
|---|---|
| **Touch controls** | A football pad drawn over the game: floating move stick, A / B / X / Y with their actions, Sprint, the modifiers, a skill-move stick, D-pad, Back / Start. Hidden while a Bluetooth controller is connected. |
| **Locked 60 fps** | The game's vblank comes from the screen itself (CADisplayLink at exactly 60 Hz, also on 120 Hz ProMotion screens), so every game frame lands on a display refresh. |
| **Phone paths** | Game files, saves, caches and logs live in the app's Documents folder (Finder / Files app). |

### Controls

| Touch | Pad | Attack | Notes |
|---|---|---|---|
| Left side, anywhere | Left stick | Move | Floating: it centres where your thumb lands. Push past the outer ring to also **sprint**. |
| A (green) | A | Pass | |
| B (red) | B | Lob / cross | |
| X (blue) | X | Shoot | Hold longer for more power. |
| Y (yellow) | Y | Through ball | |
| Sprint (big, bottom right) | RT | Sprint | |
| Finesse | RB | Finesse modifier | |
| LT, LB, R3 | LT, LB, R3 | | As on the 360 pad. |
| Right side, free space | Right stick | Skill moves | Floating, like the move stick. |
| D-pad (top left) | D-pad | Tactics | |
| Back / Start (top centre) | Back / Start | Pause | Hold Back for the NOBK.Online panel, as on PC. |

Slide a finger from one face button to another to press the new one (pass into shot without
lifting). Settings (`Documents/args.txt`, one per line):
`--touch_scale=0.6…1.3`, `--touch_opacity=0.1…1`, `--touch_inset=…` (distance from the notch side),
`--touch_sprint_on_push=false`, `--touch_button_slide=false`, `--touch_controls=false`.

### How the 60 fps lock works

FIFA 09 presents once per guest vblank, so the guest vblank rate is the frame rate (on PC the
launcher's FPS cap sets `--video_mode_refresh_rate`). The SDK normally makes that vblank with a
1 ms sleep loop at exactly 60.000 Hz. A phone screen refreshes on its own clock, so the two drift
and every few seconds one frame is shown twice or skipped: a visible stutter even at a steady
"60 fps".

On iOS the vblank thread waits for the screen instead: `game/src/ios/display_link.mm` runs a
CADisplayLink locked to 60 Hz (`preferredFrameRateRange` 60/60/60) on its own high-priority
thread, and the SDK (`patches/rexglue-ios.patch`, `GraphicsSystem` vsync worker) calls its
`rex_wait_display_vblank` before every guest vblank. If the screen cannot run at that rate, the
app is in the background, or a wait times out, that wait falls back to the timer. The vblank and
display-link threads run at `QOS_CLASS_USER_INTERACTIVE`, so they stay on the performance cores.

This removes the drift and timer jitter. It cannot make a slow frame fast: if the GPU or the
recompiled code needs more than 16.7 ms for a frame, that frame still misses its refresh.
Measure on the phone with `--log_level=info` (frame times in `Documents/logs/fifa09.log`).

## Requirements

- A Mac with Xcode (iOS SDK 17 or newer), CMake 3.25+, Ninja.
- Your own FIFA 09 Xbox 360 files, as for the PC port (`default.xex` with the SHA-256 in the
  README).
- An iPhone or iPad with an A14 chip or newer (iPhone 12 and later), iOS 17+, about 8 GB free.
- An ios-arm64 MoltenVK dynamic framework (`MoltenVK.xcframework/ios-arm64/MoltenVK.framework`,
  from the Vulkan SDK or a MoltenVK release).
- To install: a development provisioning profile (Xcode), or Sideloadly / AltStore with your
  own Apple ID (free accounts re-sign every 7 days).

## Build

```sh
# 1. SDK for iOS: rexglue-sdk v0.10.1 (546a082; the PC build's nobk patch applies to it unchanged)
#    + the PC patch + the iOS layer.
git clone --recursive https://github.com/rexglue/rexglue-sdk.git && cd rexglue-sdk
git checkout 546a082 && git submodule update --init --recursive
git apply ../fifa09-recomp/patches/nobk-sdk-1.3.0.patch
git apply ../fifa09-recomp/patches/rexglue-ios.patch
# Host (macOS) build first: the codegen tool runs on the Mac.
cmake --preset mac-arm64 && cmake --build out/build/mac-arm64 && cmake --install out/build/mac-arm64
# iOS runtime libraries (no codegen tool, no Vulkan loader):
cmake -S . -B out/build/ios-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_C_FLAGS=-mcpu=apple-a14 -DCMAKE_CXX_FLAGS=-mcpu=apple-a14 -DCMAKE_CXX_STANDARD=23 \
  -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_INSTALL_PREFIX=$PWD/out/install/ios-arm64
ninja -C out/build/ios-arm64 && cmake --install out/build/ios-arm64
# (the install registers the package in ~/.cmake/packages/rexglue: remove that entry, or a
#  later Mac configure can pick up the iOS SDK)

# 2. Generate the recompiled code on the Mac, as for the PC build (game/assets/default.xex,
#    fifa09_manifest.toml): rexglue codegen game/fifa09_manifest.toml

# 3. The game for iOS
cd ../fifa09-recomp/game
cmake -S . -B ../build/ios -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_C_FLAGS=-mcpu=apple-a14 -DCMAKE_CXX_FLAGS=-mcpu=apple-a14 \
  -DCMAKE_PREFIX_PATH=<rexglue-sdk>/out/install/ios-arm64 \
  -DCMAKE_FIND_ROOT_PATH=<rexglue-sdk>/out/install/ios-arm64
ninja -C ../build/ios

# 4. App bundle
EXE=fifa09 NAME="FIFA 09" BUNDLE_ID=com.yourname.fifa09 IOS_BUILD=../build/ios \
  IOS_SDK=<rexglue-sdk>/out/install/ios-arm64 MOLTENVK_FW=<MoltenVK.framework> \
  ios/make_app.sh --ipa        # unsigned ipa for Sideloadly / AltStore
#  or: ios/make_app.sh --install   (signed with your Xcode profile, installed over USB)
```

What `patches/rexglue-ios.patch` contains (on top of `nobk-sdk-1.3.0.patch`): the POSIX / ARM64 /
iOS layer from gtav-recomp (`REX_PLATFORM_IOS`, sandbox-safe guest memory, `SDL_RunApp` with
`args.txt`, MoltenVK next to the executable, SDL uikit), plus the display-clock vblank. Where the
two patches touched the same code (input locking, codegen fences, content headers), the SDK
v0.10.1 / nobk version is kept; GTA V's scripted test input is left out.

## Put the game on the phone

Copy your game folder (`default.xex`, `data0.big` … `data6.big`, `audiodata\`, `data\`) into the
app's Documents as `Game/`: Finder > your iPhone > Files > FIFA 09 (Mac), Apple Devices / iTunes
File Sharing (Windows), or the Files app. Layout:

```
Documents/Game/         the game (as Game\ next to the exe on PC)
Documents/args.txt      optional: your own launch arguments (read after the app's)
Documents/userdata      saves and profile
Documents/cache         shader cache
Documents/logs          fifa09.log
```

The app's own arguments are in `game/ios/args.txt` (Vulkan, 1x resolution, fast readback, 60 Hz
vsync, touch controls on).

## Open points

- **Not run on a device yet.** The SDK's Vulkan backend has an Apple / MoltenVK mode (no geometry
  shaders or wireframe fill required: primitives are converted on the CPU), which iOS inherits,
  but this game has not been run through it on a phone. First things to check: rendering
  (`fbo` render-target path), memory (jetsam limits on 4 GB phones) and the frame time in a match.
- CI (`.github/workflows/ios-check.yml`) builds the patched SDK for iOS with Xcode, compiles the
  app's iOS sources and runs the touch pad test. It cannot build the game: that needs your copy.
- Online (NOBK.Online) needs the network code (`online_hooks.h`) checked on iOS; the friends
  panel and achievements pop-ups draw through the same ImGui as the touch pad.
- The keyboard is only reachable with a hardware keyboard: online sign-in takes its account from
  the PC launcher's settings, so pass `--online_*` / `--user_gamertag` in `Documents/args.txt`.
- FSR is a Direct3D 12 effect here; the phone uses bilinear scaling.
- The executable contains code recompiled from your copy of the game: never share the app or ipa.
