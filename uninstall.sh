#!/usr/bin/env bash
set -euo pipefail

PLUGIN_ID="io.github.sh3nron.aura-blur"
BLUR_DIR="$HOME/.config/hypr/gradual-blur"
NATIVE_CONFIG="$HOME/.config/hypr/gradual-blur.lua"
HYPR_CONFIG="$HOME/.config/hypr/hyprland.lua"
SHELL_CONFIG="$HOME/.config/omarchy/shell.json"
STAMP="$(date +%Y%m%d%H%M%S)"
BACKUP_DIR="$HOME/.local/state/aura-blur/uninstall-backups/$STAMP"

for command in hyprctl jq python3; do
  command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

mkdir -p "$BACKUP_DIR"
for path in "$BLUR_DIR" "$NATIVE_CONFIG" "$HYPR_CONFIG" "$SHELL_CONFIG"; do
  [[ -e "$path" ]] && cp -a "$path" "$BACKUP_DIR/"
done

plugin="$BLUR_DIR/gradual-blur-plugin.so"
if [[ -e "$plugin" ]]; then hyprctl plugin unload "$plugin" >/dev/null 2>&1 || true; fi

python3 - "$HYPR_CONFIG" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
lines = path.read_text().splitlines()
remove_markers = (
    "Aura Blur: native geometry observer import path",
    "Aura Blur compositor plugin; user-owned and update-safe",
)
lines = [line for line in lines
         if not any(marker in line for marker in remove_markers)
         and not ("QML_IMPORT_PATH" in line and "io.github.sh3nron.aura-blur" in line)
         and line.strip() != 'require("hypr.gradual-blur")']
path.write_text("\n".join(lines).rstrip() + "\n")
PY

omarchy plugin disable "$PLUGIN_ID" >/dev/null 2>&1 || {
  shell_tmp="$(mktemp "${TMPDIR:-/tmp}/aura-blur-shell.XXXXXX")"
  jq --arg id "$PLUGIN_ID" '.plugins = [(.plugins // [])[] | select(.id != $id)]' \
    "$SHELL_CONFIG" > "$shell_tmp"
  install -m 644 "$shell_tmp" "$SHELL_CONFIG"
  rm -f "$shell_tmp"
}

[[ "$BLUR_DIR" == "$HOME/.config/hypr/gradual-blur" ]] || {
  echo "Refusing unexpected runtime path: $BLUR_DIR" >&2
  exit 1
}
rm -rf -- "$BLUR_DIR"
rm -f -- "$NATIVE_CONFIG"

hyprctl reload >/dev/null
omarchy restart shell >/dev/null

echo "Aura Blur runtime removed. Backup: $BACKUP_DIR"
echo "Now remove the marketplace checkout with:"
echo "  omarchy plugin remove $PLUGIN_ID"
echo "Hyprland config errors:"
hyprctl configerrors
