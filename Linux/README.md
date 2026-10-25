# OpenDisplay for Linux

This target connects the existing, unmodified iOS/iPadOS receiver to a KDE or
Hyprland Wayland session. It discovers `_opensidecar._tcp` services with Avahi
or opens the receiver's port through usbmuxd, captures with PipeWire, and
streams Annex B H.264 produced by FFmpeg. The command-line client and an
initial Kirigami control application share the same connection engine.

## Arch Linux dependencies

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-declarative qt6-wayland \
  kirigami pipewire avahi libusbmuxd usbmuxd ffmpeg libkscreen wayland \
  xdg-desktop-portal xdg-desktop-portal-kde
```

For Hyprland, also install `hyprland xdg-desktop-portal-hyprland`. The package
keeps the KDE dependencies so one binary can select either backend.

For hardware encoding, install the driver for the GPU: `libva-mesa-driver` for AMD,
`intel-media-driver` for modern Intel GPUs, or `nvidia-utils` for NVIDIA.
`libva-utils` is useful for checking VA-API with `vainfo`. The process must be
able to open `/dev/dri/renderD128`; normal Arch installations grant this through
the active desktop session.

USB requires the device to be unlocked and paired with the machine. Accept the
device's Trust prompt, then verify the usbmuxd path before starting OpenDisplay:

```sh
idevice_id -l
idevicepair pair       # first connection only
idevicepair validate
```

If `idevice_id` cannot see the device, reconnect the cable and inspect
`journalctl -u usbmuxd.service`; OpenDisplay cannot use a device that usbmuxd
has not finished adding. A journal `lockdown error -8` is a failed mux
preflight: unlock and disconnect the device, restart `usbmuxd.service`, then
reconnect and accept the Trust prompt. Start the service if socket activation
does not do so automatically. Wi-Fi discovery requires a running Avahi daemon;
installing the package does not enable system services:

```sh
sudo systemctl enable --now avahi-daemon.service
systemctl status avahi-daemon.service
```

## Build and test

```sh
cmake -S Linux -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux --output-on-failure
```

## Build and install the Arch package

The `-git` package follows the pushed `linux-port` branch on the public
[GitHub repository](https://github.com/aaditya19saini/opendisplay-linux). From the
repository root, use a clean package build so an older clone of `main` cannot
remain in makepkg's `src/` directory:

```sh
(cd Linux/packaging/arch && makepkg --cleanbuild --syncdeps --install)
```

The short equivalent is `makepkg -Csi`. Building the package consumes the
pushed branch, not uncommitted files in the current worktree. Push a commit to
`origin/linux-port` before packaging when testing new local changes.

## Run

Sessions are owned by a small background service, `opendisplayd`, so closing
or restarting a client never interrupts streaming. Enable it once:

```sh
systemctl --user enable --now opendisplayd.service
```

Open the OpenDisplay app on the iOS device, then control the service with
`opendisplayctl` (it starts the service on demand) or the GUI:

```sh
opendisplayctl extend          # connect, or switch live to extend
opendisplayctl mirror          # connect, or switch live to mirror
opendisplayctl toggle-mode
opendisplayctl status          # add --json for scripts
opendisplayctl watch           # follow state and latency stats
opendisplayctl disconnect
opendisplayctl devices
opendisplayctl set encoder=nvenc fps=60 extendTo=left
opendisplayctl set workspace=dedicated   # or a number (moves it there), or auto
opendisplayctl autoconnect on  # reconnect when the service starts
opendisplay-gui
```

Behavior worth knowing:

- The first connection for each device and mode shows the desktop's share
  dialog. The service saves the portal restore token
  (`~/.local/state/opendisplay/state.json`, mode 0600), and later connections
  skip the dialog. Virtual monitors use a stable per-device name
  (`OpenDisplay-u<udid>` for USB) so the token keeps matching.
- Switching between extend and mirror rebuilds only the capture pipeline; the
  receiver stays connected.
- Only one session runs at a time, whichever program starts it. A second
  attempt (for example the standalone CLI while the service is streaming)
  fails immediately and names the running session. The lock
  (`$XDG_RUNTIME_DIR/opendisplay/session.lock`) is a kernel file lock, so a
  crashed session never leaves it stuck.
- When the receiver drops (sleep, unplug, app closed) the service retries
  with exponential backoff until you disconnect. After a crash, systemd
  restarts it and it resumes the session that was running.
- Hyprland config reloads (layout toggles, theme changes) drop runtime
  monitor rules. While a session runs, the service writes its rules to
  `~/.local/state/opendisplay/hyprland.lua`; add this line at the end of
  `~/.config/hypr/hyprland.lua` so reloads keep the virtual monitor's mode
  (otherwise it flips briefly and the stream restarts):

  ```lua
  pcall(dofile, (os.getenv("XDG_STATE_HOME") or (os.getenv("HOME") .. "/.local/state")) .. "/opendisplay/hyprland.lua")
  ```

  Without the line, the service still re-applies its rules after each reload.
- Logs: `journalctl --user -u opendisplayd -f`. The GUI's diagnostics checkbox
  enables verbose logs, including capture-to-send latency every 5 s.

The control socket is `$XDG_RUNTIME_DIR/opendisplay/control.sock` and speaks
one JSON object per line (`{"cmd":"status"}`, `{"cmd":"connect","mode":"mirror"}`,
`{"cmd":"watch"}` …), so other front ends such as bar widgets can drive it.

The GUI exposes common settings and monitor-layout overrides, and uses the
desktop's StatusNotifier tray when available. Quitting the GUI leaves the
service and any stream running.

The standalone CLI still runs a session in the foreground without the
service, which is useful for debugging:

```sh
./build/linux/opendisplay-linux --transport auto --mode extend
```

The compositor backend is detected automatically; override it with
`--compositor kde` or `--compositor hyprland`. The first run displays the
desktop's capture permission dialog. Other useful forms are:

```sh
./build/linux/opendisplay-linux --list
./build/linux/opendisplay-linux --transport usb --udid DEVICE_UDID
./build/linux/opendisplay-linux --transport wifi --host 192.168.1.40
./build/linux/opendisplay-linux --encoder vaapi --vaapi-device /dev/dri/renderD128
./build/linux/opendisplay-linux --encoder nvenc --mode mirror --no-input
./build/linux/opendisplay-linux --compositor hyprland --reference-monitor eDP-1
```

### Virtual monitor layout

In extend mode, OpenDisplay queries the selected compositor before opening its
portal. One enabled monitor is selected automatically; with two or more, pass
its connector name or current backend id explicitly:

```sh
opendisplay-linux --reference-monitor DP-1
opendisplay-linux --reference-monitor eDP-1 --extend-to right --align-to bottom
opendisplay-linux --extend-to bottom --align-to right
```

The default is bottom-right: extend right and align bottom. Left/right extension
accepts `top`, `bottom`, or `center` alignment; top/bottom extension accepts
`left`, `right`, or `center`. Positions use compositor logical coordinates.

The automatic mode starts with the iPad's native pixels. It uses physical DPI
when both panel sizes are available, otherwise the iOS-reported native scale;
the result is rounded to a 0.05 scale step. Pixel dimensions are nudged to the
nearest values that produce integer logical dimensions; widths are also made
divisible by eight for KDE's CVT-generated custom modes. Hyprland keeps native
pixel dimensions when they already produce an integer logical size. Detection
and any part of the calculation can be overridden:

```sh
opendisplay-linux --virtual-resolution 2420x1668 --display-scale 1.25
opendisplay-linux --virtual-refresh 60 --ipad-size-mm 263x181
opendisplay-linux --reference-resolution 3840x2160 --reference-scale 1.5 \
  --reference-size-mm 597x336
opendisplay-linux --reference-geometry 2560x1440+0+0
```

`--scale` remains the video encoder resolution multiplier and does not change
desktop display scaling. KDE custom virtual modes require Plasma/libkscreen
6.6 or newer. OpenDisplay uses libkscreen in-process so output identity and
detailed configuration errors remain reliable while KDE adds or renumbers
outputs.

On Hyprland, OpenDisplay creates a named headless output with `hyprctl`, applies
the same resolution, scaling, and placement calculation, and asks the
ScreenCast portal to capture a monitor. Select the displayed `OpenDisplay-PID`
monitor in the share picker. Touch input uses Hyprland's
`zwlr_virtual_pointer_manager_v1` support and does not require the unsupported
RemoteDesktop portal. Current Hyprland Lua monitor rules are used first, with
the pre-0.55 `keyword monitor` command as a compatibility fallback. The
headless output is removed when the session stops. Before XDPH opens its share
picker, OpenDisplay restores focus and cursor placement to the selected
reference monitor so the authorization prompt remains reachable. The reference
output is temporarily pinned to its detected geometry while the headless output
exists, preventing Hyprland `auto` rules from reversing their relative order;
the user's configuration is reloaded after teardown.

If Bonjour discovery is unavailable, confirm that the phone app is open and
inspect the advertised service with `avahi-browse -rt _opensidecar._tcp`.
You can bypass Avahi when the receiver's address is known:

```sh
opendisplay-linux --transport wifi --host 192.168.1.40 --port 9000
```

`--encoder auto` prefers VA-API, then NVENC, and falls back to `libx264`.
Use `Linux/tools/fake_receiver.py` to exercise the TCP framing without an iOS
device; it does not decode video or advertise Bonjour.

## Current MVP limits

KDE Plasma Wayland and Hyprland are supported. Hyprland's portal picker cannot
yet be preselected by output name, so the requested monitor must be chosen in
the dialog. Audio,
clipboard, cursor sprites, encryption, reconnection, and multi-device sessions
remain out of scope. Compositor-neutral layout lives in `display_layout`, while
KScreen and Hyprland output operations remain isolated in their respective
controllers. Later compositor backends need not change transport, protocol,
layout planning, or encoding code.
