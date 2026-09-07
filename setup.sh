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
SHELL_CONFIG="$HOME/.config/omarchy/shell.json"
HYPR_CONFIG="$HOME/.config/hypr/hyprland.lua"
NATIVE_CONFIG="$HOME/.config/hypr/gradual-blur.lua"
STAMP="$(date +%Y%m%d%H%M%S)"
BACKUP_DIR="$HOME/.local/state/aura-blur/setup-backups/$STAMP"

cleanup_build() {
  [[ "$BUILD_ROOT" == "${TMPDIR:-/tmp}"/aura-blur-build.* ]] && rm -rf -- "$BUILD_ROOT"
}
trap cleanup_build EXIT

[[ -f "$ROOT_DIR/manifest.json" ]] || { echo "Run this script from the plugin checkout (see README)." >&2; exit 1; }
[[ "$ROOT_DIR" == "$PLUGIN_DIR" ]] || {
  echo "Expected the plugin checkout at $PLUGIN_DIR" >&2
  echo "Install it first with: omarchy plugin add https://github.com/Sh3nron/omarchy-aura-blur.git" >&2
  exit 1
}

for command in cmake ninja jq python3 pkg-config hyprctl omarchy; do
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
for path in "$BLUR_DIR" "$SHELL_CONFIG" "$HYPR_CONFIG" "$NATIVE_CONFIG"; do
  [[ -e "$path" ]] && cp -a "$path" "$BACKUP_DIR/"
done

mkdir -p "$BLUR_DIR" "$PLUGIN_DIR/GradualBlurObserver"
install -m 644 "$ROOT_DIR/copy/config.jsonc" "$BLUR_DIR/config.jsonc"
install -m 755 "$ROOT_DIR/copy/load-plugin" "$BLUR_DIR/load-plugin"
install -m 755 "$COMPOSITOR_BUILD/gradual-blur-plugin.so" "$BLUR_DIR/gradual-blur-plugin.so"
printf '%s\n' "$HYPR_ABI" > "$BLUR_DIR/hyprland-abi"
install -m 644 "$ROOT_DIR/copy/gradual-blur.lua" "$NATIVE_CONFIG"
install -m 644 "$ROOT_DIR/observer/plugin/GradualBlurObserver/qmldir" "$PLUGIN_DIR/GradualBlurObserver/qmldir"
install -m 755 "$OBSERVER_BUILD/native/libgradualblurobserverplugin.so" "$PLUGIN_DIR/GradualBlurObserver/libgradualblurobserverplugin.so"

python3 - "$HYPR_CONFIG" "$PLUGIN_DIR" <<'PY'
from pathlib import Path
import sys

hypr, plugin_dir = map(Path, sys.argv[1:])
text = hypr.read_text()
lines = [line for line in text.splitlines()
         if "Aura Blur: native geometry observer import path" not in line
         and not ("QML_IMPORT_PATH" in line and "io.github.sh3nron.aura-blur" in line)
         and line.strip() != 'require("hypr.gradual-blur")']
text = "\n".join(lines).rstrip() + "\n"
qml_path = f'hl.env("QML_IMPORT_PATH", "{plugin_dir}")'
if qml_path not in text:
    text += f'\n-- Aura Blur: native geometry observer import path.\n{qml_path}\n'
if 'require("hypr.gradual-blur")' not in text:
    text = text.rstrip() + "\n\n-- Aura Blur compositor plugin; user-owned and update-safe.\n" + 'require("hypr.gradual-blur")' + "\n"
hypr.write_text(text)
PY

omarchy plugin enable "$PLUGIN_ID" >/dev/null 2>&1 || {
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
  omarchy plugin enable "$PLUGIN_ID" >/dev/null 2>&1 || {
    shell_tmp="$(mktemp "${TMPDIR:-/tmp}/aura-blur-shell.XXXXXX")"
    jq --arg id "$PLUGIN_ID" '
      (.plugins // []) as $plugins
      | .plugins = (if any($plugins[]; .id == $id) then $plugins
                    else $plugins + [{"id": $id}] end)' \
      "$SHELL_CONFIG" > "$shell_tmp"
    install -m 644 "$shell_tmp" "$SHELL_CONFIG"
    rm -f "$shell_tmp"
  }
}

hyprctl eval 'hl.config({ decoration = { screen_shader = "" } })' >/dev/null || true

# Take the shell down before unloading the previous build: a connected
# geometry observer can make the compositor's plugin-unload path hang, and
# older server builds had no receive timeout on their client socket. With the
# observer disconnected the unload is always quick and clean.
pkill -x quickshell >/dev/null 2>&1 || true
sleep 0.5
hyprctl plugin unload "$BLUR_DIR/gradual-blur-plugin.so" >/dev/null 2>&1 || true

hyprctl reload >/dev/null
"$BLUR_DIR/load-plugin"
omarchy restart shell >/dev/null

echo "Installed Aura Blur. Backup: $BACKUP_DIR"
echo "Hyprland config errors:"
hyprctl configerrors
