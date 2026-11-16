#!/bin/bash
# Assemble the iOS app of a ReXGlue port and either sign + install it (dev build, Xcode profile)
# or write an unsigned .ipa for Sideloadly / AltStore, which sign it with the player's Apple ID.
#   ios/make_app.sh            -> signed .app (needs a provisioning profile for $BUNDLE_ID)
#   ios/make_app.sh --install  -> signed .app, installed on the connected device (devicectl)
#   ios/make_app.sh --ipa      -> unsigned <Name>.ipa next to the .app
# Inputs: the game built for iOS (cmake -B build/ios -DCMAKE_SYSTEM_NAME=iOS ..., see docs/IOS.md),
# the SDK's iOS install (librexruntime.dylib, librexgpu-xenos.dylib) and an ios-arm64 MoltenVK
# dynamic framework. Optional: CONFIG_TOML (the game's settings file, copied as <exe>.toml).
# The executable contains code recompiled from YOUR copy of the game: never publish the app or ipa.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="${EXE:?set EXE (executable name)}"
NAME="${NAME:-$EXE}"
VERSION="${VERSION:-1.0}"
BUNDLE_ID="${BUNDLE_ID:-com.example.$EXE}"
TEAM_ID="${TEAM_ID:-}"
BUILD="${IOS_BUILD:?set IOS_BUILD (the iOS cmake build folder)}"
SDK="${IOS_SDK:?set IOS_SDK (the SDK install for ios-arm64)}"
MOLTENVK_FW="${MOLTENVK_FW:?set MOLTENVK_FW (MoltenVK.xcframework/ios-arm64/MoltenVK.framework)}"
OUT="$BUILD/package"
APP="$OUT/$NAME.app"

rm -rf "$APP"; mkdir -p "$APP"
if [ -f "$BUILD/$EXE.app/$EXE" ]; then cp "$BUILD/$EXE.app/$EXE" "$APP/$EXE"; else cp "$BUILD/$EXE" "$APP/$EXE"; fi
cp "$SDK/lib/librexruntime.dylib" "$SDK/lib/librexgpu-xenos.dylib" "$APP/"
cp "$MOLTENVK_FW/MoltenVK" "$APP/libMoltenVK.dylib"
chmod u+w "$APP"/*.dylib
# Everything sits in the bundle root next to the executable (the SDK opens libMoltenVK.dylib there).
install_name_tool -id @rpath/libMoltenVK.dylib "$APP/libMoltenVK.dylib" 2>/dev/null || true
install_name_tool -add_rpath @executable_path "$APP/$EXE" 2>/dev/null || true
cp "$HERE/args.txt" "$APP/args.txt"
# CONFIG_TOML=<file>: the game's <exe>.toml settings, read by the SDK from the executable's folder.
if [ -n "${CONFIG_TOML:-}" ]; then cp "$CONFIG_TOML" "$APP/$EXE.toml"; fi
sed -e "s/@BUNDLE_ID@/$BUNDLE_ID/" -e "s/@BUILD@/$(date +%Y%m%d%H%M)/" -e "s/@EXE@/$EXE/" \
    -e "s/@NAME@/$NAME/g" -e "s/@VERSION@/$VERSION/" "$HERE/Info.plist.in" > "$APP/Info.plist"
plutil -convert binary1 "$APP/Info.plist"
printf 'APPL????' > "$APP/PkgInfo"

if [ "${1:-}" = "--ipa" ]; then
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  mkdir -p "$W/Payload"; cp -R "$APP" "$W/Payload/"
  (cd "$W" && zip -qr -X "$OUT/$NAME.ipa" Payload)
  echo "ipa: $OUT/$NAME.ipa (unsigned: install with Sideloadly or AltStore)"
  exit 0
fi

PROFILE=""
for p in "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles/"*.mobileprovision \
         "$HOME/Library/MobileDevice/Provisioning Profiles/"*.mobileprovision; do
  [ -f "$p" ] || continue
  appid=$(security cms -D -i "$p" 2>/dev/null | plutil -extract Entitlements.application-identifier raw - 2>/dev/null) || continue
  case "$appid" in *".$BUNDLE_ID") [ -z "$TEAM_ID" ] || [ "$appid" = "$TEAM_ID.$BUNDLE_ID" ] && PROFILE="$p" ;; esac
done
[ -n "$PROFILE" ] || { echo "no provisioning profile for $BUNDLE_ID (or use --ipa)"; exit 1; }
cp "$PROFILE" "$APP/embedded.mobileprovision"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
security cms -D -i "$PROFILE" > "$W/profile.plist" 2>/dev/null
plutil -extract Entitlements xml1 -o "$W/ent.plist" "$W/profile.plist"
IDENTITY=$(security find-identity -v -p codesigning | awk '/Apple Development/ {print $2; exit}')
for d in "$APP"/*.dylib; do codesign --force --sign "$IDENTITY" --timestamp=none "$d"; done
codesign --force --sign "$IDENTITY" --entitlements "$W/ent.plist" --timestamp=none --generate-entitlement-der "$APP"
codesign --verify --verbose "$APP"
echo "app: $APP ($(du -sh "$APP" | cut -f1))"

if [ "${1:-}" = "--install" ]; then
  DEVICE=$(xcrun devicectl list devices 2>/dev/null | grep -v unavailable | grep -E 'available|connected' \
    | grep -oE '[0-9A-F]{8}-[0-9A-F]{16}|[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}' | head -1)
  [ -n "$DEVICE" ] || { echo "no connected device"; exit 1; }
  xcrun devicectl device install app --device "$DEVICE" "$APP"
fi
