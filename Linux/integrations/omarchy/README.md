# Omarchy bar plugin

`aaditya.opendisplay` puts OpenDisplay in the Omarchy bar. It follows
`opendisplayd` through `opendisplayctl watch --json`, so it reflects changes
made from the GUI, the CLI, or other bars instantly.

- **Left click**: popup with status, device, live latency, Extend/Mirror,
  position (left/right/top/bottom), the workspace shown on the display
  (Auto, Dedicated, or 1-9; applied live, and returned to the laptop screen
  when changed or disconnected), Connect/Disconnect and Autoconnect.
- **Right click**: switch between extend and mirror (live, no reconnect).
- **Middle click**: connect or disconnect.
- The widget only appears while a session is active: pulsing while connecting
  or retrying, accent cast icon while extending, mirror icon while mirroring.
  It hides when you disconnect; start a session with `opendisplayctl extend`,
  the GUI, or a keybinding.

## Install

Requires `opendisplayd`/`opendisplayctl` on `PATH` and the user service
(`systemctl --user enable --now opendisplayd.service`).

```sh
cp -r aaditya.opendisplay ~/.config/omarchy/plugins/
# Add {"id": "aaditya.opendisplay"} to a section of bar.layout in
# ~/.config/omarchy/shell.json (e.g. right after "omarchy.tray"), then:
omarchy restart shell
```

Packages install a copy under `/usr/share/opendisplay/integrations/omarchy/`.

## Keybindings

The widget exposes an IPC target, for example in `~/.config/hypr/bindings.lua`:

```lua
o.bind("SUPER + SHIFT + P", "OpenDisplay panel", "omarchy-shell aaditya.opendisplay toggle")
o.bind("SUPER + CTRL + P", "OpenDisplay extend/mirror", "opendisplayctl toggle-mode")
```

Other IPC methods: `open`, `close`, `toggleMode`.
