#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OBSERVER_DIR="$ROOT_DIR/observer"
COMPOSITOR_DIR="$ROOT_DIR/compositor"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/aura-blur-build.XXXXXX")"
OBSERVER_BUILD="$BUILD_ROOT/observer"
COMPOSITOR_BUILD="$BUILD_ROOT/compositor"
BLUR_DIR="$HOME/.config/hypr/gradual-blur"
PLUGIN_ID="io.github.sh3nron.aura-blur"
PLUGIN_DIR="$HOME/.config/omarchy/plugins/$PLUGIN_ID"
LEGACY_PLUGIN_DIR="$HOME/.config/omarchy/plugins/yeshuah.gradual-blur"
SHELL_CONFIG="$HOME/.config/omarchy/shell.json"
AUTOSTART="$HOME/.config/hypr/autostart.lua"
HYPR_CONFIG="$HOME/.config/hypr/hyprland.lua"
BINDINGS="$HOME/.config/hypr/bindings.lua"
NATIVE_CONFIG="$HOME/.config/hypr/gradual-blur.lua"
STAMP="$(date +%Y%m%d%H%M%S)"
BACKUP_DIR="$HOME/.local/state/aura-blur/setup-backups/$STAMP"

cleanup_build() {
  [[ "$BUILD_ROOT" == "${TMPDIR:-/tmp}"/aura-blur-build.* ]] && rm -rf -- "$BUILD_ROOT"
}
trap cleanup_build EXIT

for command in cmake ninja jq python3 pkg-config hyprctl; do
  command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

HYPR_ABI="$(hyprctl version -j | jq -r '.commit // .hash // empty')"
[[ -n "$HYPR_ABI" ]] || { echo "Could not identify the running Hyprland ABI" >&2; exit 1; }

echo "Building geometry observer and compositor plugin..."
cmake -S "$OBSERVER_DIR" -B "$OBSERVER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$OBSERVER_BUILD" >/dev/null
cmake -S "$COMPOSITOR_DIR" -B "$COMPOSITOR_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$COMPOSITOR_BUILD" >/dev/null

mkdir -p "$BACKUP_DIR"
for path in "$BLUR_DIR" "$PLUGIN_DIR" "$LEGACY_PLUGIN_DIR" "$SHELL_CONFIG" "$AUTOSTART" "$HYPR_CONFIG" "$BINDINGS" "$NATIVE_CONFIG"; do
  [[ -e "$path" ]] && cp -a "$path" "$BACKUP_DIR/"
done

old_plugin="$BLUR_DIR/gradual-blur-plugin.so"
if [[ -e "$old_plugin" ]]; then hyprctl plugin unload "$old_plugin" >/dev/null 2>&1 || true; fi

mkdir -p "$BLUR_DIR" "$PLUGIN_DIR/GradualBlurObserver"
install -m 644 "$ROOT_DIR/copy/config.jsonc" "$BLUR_DIR/config.jsonc"
install -m 755 "$ROOT_DIR/copy/load-plugin" "$BLUR_DIR/load-plugin"
install -m 755 "$COMPOSITOR_BUILD/gradual-blur-plugin.so" "$BLUR_DIR/gradual-blur-plugin.so"
printf '%s\n' "$HYPR_ABI" > "$BLUR_DIR/hyprland-abi"
install -m 644 "$ROOT_DIR/copy/gradual-blur.lua" "$NATIVE_CONFIG"
if [[ "$(realpath "$ROOT_DIR")" != "$(realpath "$PLUGIN_DIR")" ]]; then
  install -m 644 "$ROOT_DIR/manifest.json" "$PLUGIN_DIR/manifest.json"
  install -m 644 "$ROOT_DIR/Service.qml" "$PLUGIN_DIR/Service.qml"
  install -m 755 "$ROOT_DIR/uninstall.sh" "$PLUGIN_DIR/uninstall.sh"
  install -m 644 "$ROOT_DIR/README.md" "$PLUGIN_DIR/README.md"
  install -m 644 "$ROOT_DIR/LICENSE" "$PLUGIN_DIR/LICENSE"
fi
install -m 644 "$OBSERVER_DIR/plugin/GradualBlurObserver/qmldir" "$PLUGIN_DIR/GradualBlurObserver/qmldir"
install -m 755 "$OBSERVER_BUILD/native/libgradualblurobserverplugin.so" "$PLUGIN_DIR/GradualBlurObserver/libgradualblurobserverplugin.so"
rm -rf "$PLUGIN_DIR/native"

pkill -TERM -f "$BLUR_DIR/gradual-blur-daemon.py" 2>/dev/null || true
rm -f "$BLUR_DIR/gradual-blur-daemon.py" "$BLUR_DIR/gradual-blur.frag" \
  "$BLUR_DIR/gradual-blur.frag.tmpl" "$BLUR_DIR/gradual-blur-unblur" \
  "${XDG_RUNTIME_DIR:-/run/user/$UID}/gradual-blur.suspend"

python3 - "$AUTOSTART" "$BINDINGS" "$HYPR_CONFIG" "$PLUGIN_DIR" <<'PY'
from pathlib import Path
import sys

autostart, bindings, hypr, plugin_dir = map(Path, sys.argv[1:])
lines = [line for line in autostart.read_text().splitlines()
         if "gradual-blur-daemon.py" not in line
         and "Radial gradual blur behind pop-up panels" not in line
         and "Exact radial gradual blur around live Quickshell" not in line]
autostart.write_text("\n".join(lines).rstrip() + "\n")

lines = bindings.read_text().splitlines()
start = next((i for i, line in enumerate(lines) if "yeshuah.gradual-blur: capture exclusions" in line), None)
if start is not None: del lines[start:start + 5]
bindings.write_text("\n".join(lines).rstrip() + "\n")

text = hypr.read_text()
require = 'require("hypr.gradual-blur")'
lines = [line for line in text.splitlines()
         if "yeshuah.gradual-blur: user-owned QML import path" not in line
         and not ("QML_IMPORT_PATH" in line and "yeshuah.gradual-blur" in line)]
text = "\n".join(lines).rstrip() + "\n"
qml_path = f'hl.env("QML_IMPORT_PATH", "{plugin_dir}")'
if qml_path not in text:
    text += f'\n-- Aura Blur: native geometry observer import path.\n{qml_path}\n'
if require not in text:
    text = text.rstrip() + "\n\n-- Aura Blur compositor plugin; user-owned and update-safe.\n" + require + "\n"
hypr.write_text(text)
PY

shell_tmp="$(mktemp "${TMPDIR:-/tmp}/gradual-blur-shell.XXXXXX")"
jq '(.plugins // []) as $plugins
    | .plugins = ([$plugins[] | select(.id != "yeshuah.gradual-blur")]
      | if any(.[]; .id == "io.github.sh3nron.aura-blur") then .
        else . + [{"id":"io.github.sh3nron.aura-blur"}] end)' \
  "$SHELL_CONFIG" > "$shell_tmp"
install -m 644 "$shell_tmp" "$SHELL_CONFIG"
rm -f "$shell_tmp"

hyprctl eval 'hl.config({ decoration = { screen_shader = "" } })' >/dev/null || true
hyprctl reload >/dev/null
"$BLUR_DIR/load-plugin"
omarchy restart shell >/dev/null

# Remove the retired development-ID copy only after the new service and native
# runtime have installed successfully. Its contents are preserved in BACKUP_DIR.
if [[ "$LEGACY_PLUGIN_DIR" != "$ROOT_DIR" ]]; then rm -rf "$LEGACY_PLUGIN_DIR"; fi

echo "Installed Aura Blur. Backup: $BACKUP_DIR"
echo "Hyprland config errors:"
hyprctl configerrors
