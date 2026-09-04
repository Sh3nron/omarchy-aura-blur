-- Quality profile used by the compositor plugin's live framebuffer blur.
hl.config({
  decoration = {
    blur = {
      enabled = true,
      size = 6,
      passes = 3,
      noise = 0.0,
      brightness = 1.0,
      contrast = 1.0,
      vibrancy = 0.0,
      new_optimizations = true,
    },
  },
})

-- The plugin invokes blur explicitly; ordinary windows must not blur themselves.
o.window(".*", { no_blur = true })

-- ABI validation prevents an old binary loading after a Hyprland upgrade.
o.exec_on_start(os.getenv("HOME") .. "/.config/hypr/gradual-blur/load-plugin")
