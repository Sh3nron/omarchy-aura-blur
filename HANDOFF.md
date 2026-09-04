# Aura Blur for Omarchy

This revision replaces both rejected renderers: the single-pass final-screen
shader and the stepped layer-shell halos. The effect is now a Hyprland
compositor plugin with a single continuous rounded-distance matte.

## Rendering architecture

The Quickshell observer sends exact live card rectangles, corner radii, output
names, and opacity over a versioned Unix socket. At Hyprland's
`RENDER_POST_WINDOWS` stage, a custom render-pass element:

1. blurs the live compositor framebuffer with Hyprland's native three-pass
   blur (`size = 6`, `passes = 3`, `noise = 0`);
2. computes one smooth two-axis falloff from each real rounded card edge;
3. composites the blurred texture through that matte; and
4. lets Hyprland render Top and Overlay layers afterward.

There are no screenshots, cached desktop images, fullscreen mask windows,
ring layers, or final-screen shaders. The matte contains only scalar opacity
values and is quarter-resolution with linear GPU sampling; the background
itself remains a live full-resolution compositor texture.

The matte deliberately underlaps each card by 12 logical pixels. That small
hidden overlap prevents antialiased card edges and linear matte sampling from
revealing an unblurred seam. It is actual live blur beneath the UI—not a visual
extension placed alongside it.

The custom pass declares `needsLiveBlur()` and exposes the active halo bounds
to Hyprland. This lets the compositor expand partial redraws by the native blur
kernel radius and repaint underlying application pixels before they are
sampled. The blur function receives only that valid expanded damage region;
sampling the cleared remainder of Hyprland's temporary framebuffer would leak
wallpaper or transparent pixels into an otherwise opaque application.

## Layer guarantees

- Application content is present when the plugin blur pass runs.
- The menu bar (`Top`) is rendered afterward and stays sharp.
- Popups, OSDs, notifications, and the screenshot selector (`Overlay`) are
  rendered afterward and stay sharp.
- The plugin creates no Wayland surface and accepts no pointer or keyboard
  input.
- An ABI stamp prevents the plugin from loading after a Hyprland upgrade until
  it is rebuilt by `install.sh`.

## Components

- `observer/`: exact Qt/Quickshell popup geometry observer only.
- `compositor/`: ABI-sensitive Hyprland render-pass plugin.
- `copy/gradual-blur.lua`: native blur quality profile and startup loader.
- `copy/load-plugin`: exact-ABI guard and plugin loader.
- `copy/config.jsonc`: detector limits and namespace exclusions.
- `install.sh`: builds, backs up, migrates, installs, and validates.
- `setup.sh` / `uninstall.sh`: marketplace-safe native setup and removal.

The public Omarchy plugin ID is `io.github.sh3nron.aura-blur`. Marketplace
installation deliberately does not hide the native build: users add the
repository first, then explicitly run `setup.sh`. Removal runs `uninstall.sh`
before `omarchy plugin remove` so no Hyprland configuration is orphaned.

## Verification

```bash
./install.sh
hyprctl plugin list
hyprctl getoption decoration:screen_shader
hyprctl configerrors
omarchy-shell osd show '{"icon":"audio-volume-high","value":72,"duration":5000}'
```

`gradual-blur` must appear in the plugin list, `screen_shader` must be empty,
and config errors must be empty. Timestamped backups are stored under
`~/.local/state/gradual-blur/backups/`. Nothing in `/usr/share/omarchy` is
modified.
