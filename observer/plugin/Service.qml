import QtQuick
import Quickshell
import GradualBlurObserver 1.0

Scope {
  PopupGeometryObserver {
    socketPath: Quickshell.env("XDG_RUNTIME_DIR") + "/gradual-blur.sock"
    configPath: Quickshell.env("HOME") + "/.config/hypr/gradual-blur/config.jsonc"
  }
}
