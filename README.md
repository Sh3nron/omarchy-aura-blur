# Aura Blur

Aura Blur adds a continuous, high-quality radial blur around Omarchy popups,
panels, notifications, and OSDs. Blur is strongest at the real rounded edge of
each surface and fades smoothly in every direction.

It uses Hyprland's live compositor framebuffer. It does **not** capture the
desktop, cache screenshots, or rasterize the background. Application content
is blurred before Omarchy renders its menu bar and overlay surfaces, keeping
the bar, popup content, and screenshot selector sharp.

## Requirements

- Omarchy Quattro with Hyprland and the Omarchy shell
- `hyprland`, `quickshell`, `qt6-declarative`, `cmake`, `ninja`, `gcc`,
  `pkgconf`, and `json-c`
- Hyprland development headers matching the running compositor

Aura Blur includes an ABI-sensitive Hyprland plugin. The setup script compiles
it locally against the installed Hyprland headers; no prebuilt compositor
binary is downloaded or executed.

## Install

Add the repository without enabling it first, then run the reviewed setup
script:

```sh
omarchy plugin add https://github.com/Sh3nron/omarchy-aura-blur.git
~/.config/omarchy/plugins/io.github.sh3nron.aura-blur/setup.sh
```

The setup script builds both native components, creates timestamped backups,
installs user-owned files under `~/.config`, enables the service, reloads
Hyprland, and restarts the Omarchy shell. It never uses `sudo` and never edits
`/usr/share/omarchy`.

## Update

Hyprland plugins are tied to the exact compositor ABI. Rebuild after every
Aura Blur or Hyprland update:

```sh
omarchy plugin update io.github.sh3nron.aura-blur
~/.config/omarchy/plugins/io.github.sh3nron.aura-blur/setup.sh
```

An ABI guard refuses to load an outdated build and displays a notification
instead of risking a compositor crash.

## Remove

Run the cleanup before deleting the marketplace checkout:

```sh
~/.config/omarchy/plugins/io.github.sh3nron.aura-blur/uninstall.sh
omarchy plugin remove io.github.sh3nron.aura-blur
```

The cleanup unloads the compositor plugin, removes only Aura Blur's runtime and
Hyprland configuration, removes its exact user-config entries, reloads the
desktop, and preserves a timestamped backup.

## How it works

A native Qt observer reports exact live popup geometry over a local Unix
socket. An ABI-matched Hyprland render-pass plugin applies native three-pass
blur through a continuous rounded-distance matte after application windows and
before Top/Overlay layers. The matte underlaps each popup slightly to avoid an
unblurred seam along antialiased edges.

See [HANDOFF.md](HANDOFF.md) for rendering and damage-tracking details.

## License

[MIT](LICENSE)
