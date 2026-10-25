<div align="center">

<img src="public/logo.png" width="128" alt="OpenDisplay app icon" />

# OpenDisplay Linux Fork

**Use an iPhone or iPad as an additional display on a Linux Wayland desktop.**

[Install the iOS/iPadOS receiver](https://peetzweg.github.io/opendisplay/) · [Linux guide](Linux/README.md) · [Upstream project](https://github.com/peetzweg/opendisplay)

</div>

> [!WARNING]
> This is a highly experimental, vibe-coded personal fork. It is not expected
> to merge back into the upstream project, compatibility may break without
> notice, and maintenance will be limited. Use it only if you are comfortable
> diagnosing Linux, Wayland, PipeWire, portal, and hardware-encoding issues.

## Current scope

This fork adds a Linux sender for the unmodified OpenDisplay iOS/iPadOS
receiver and wire protocol. It supports:

- **Hyprland** through `xdg-desktop-portal-hyprland` and headless outputs
  (the main tested setup, including Omarchy), and **KDE Plasma Wayland**
  through `xdg-desktop-portal-kde` and KScreen.
- **USB** through `usbmuxd` and **Wi-Fi** discovery through Avahi.
- **Extend** (the device becomes an extra monitor) and **Mirror** modes,
  switchable live without reconnecting.
- **Low latency**: PipeWire capture with in-process H.264 encoding. NVENC,
  VA-API (AMD/Intel) and x264 are supported; `auto` benchmarks them once and
  picks the fastest. Measured on an iPad over USB with 60 fps video: about
  14 ms typical and 18 ms worst-case delay at 58 fps.
- **A background service** (`opendisplayd`, a systemd user unit) that owns the
  session:
  - Closing the GUI or bar doesn't stop streaming.
  - Drops are retried and crashes resume.
  - The share dialog only appears the first time.
- **Clients**: `opendisplayctl` on the command line, a Qt Quick/Kirigami GUI,
  and an [Omarchy bar plugin](Linux/integrations/omarchy/). All of them show
  status and live latency and control mode, position, and workspace.
- **Hyprland niceties**:
  - The virtual monitor keeps its mode across config reloads such as layout
    toggles and theme changes.
  - Frames are letterboxed, never stretched.
  - You choose which workspace appears on the device: automatic, a dedicated
    one, or a numbered one.

Other compositors and non-Wayland sessions are currently unsupported. Portal
behavior also depends on compositor and desktop versions, so successful setup
on one system does not guarantee identical behavior elsewhere.

## Quick start

```sh
# Arch: build and install the package (see the Linux guide), then:
systemctl --user enable --now opendisplayd.service

# Open the OpenDisplay app on the iPad/iPhone, then:
opendisplayctl extend      # or: mirror, toggle-mode, status, disconnect
```

The first connection shows the desktop's screen-share dialog; pick the
`OpenDisplay-…` monitor (or your laptop screen for mirror). Later connections
skip it. On Hyprland, add the one-line `hyprland.lua` hook from the
[Linux guide](Linux/README.md#run) so config reloads never reset the display.

## Receiver application

No modified mobile application is required. Install and run the same upstream
OpenDisplay receiver on an iPhone or iPad from the
[OpenDisplay installation page](https://peetzweg.github.io/opendisplay/).
The receiver currently requires iOS/iPadOS 16 or newer. The public
[TestFlight beta](https://testflight.apple.com/join/3NYaY11c) remains available
when offered by the upstream project.

## Linux installation and usage

An Arch Linux `PKGBUILD` is provided under
[`Linux/packaging/arch/`](Linux/packaging/arch/PKGBUILD). Source builds and the
package install `opendisplayd` (with its systemd user unit), `opendisplayctl`,
`opendisplay-gui`, the standalone `opendisplay-linux` CLI, and the Omarchy
bar plugin under `/usr/share/opendisplay/integrations/omarchy/`.

For dependencies, package-building commands, the service and control
protocol, Wi-Fi and USB setup, monitor placement and scaling options,
KDE/Hyprland notes, and troubleshooting, use the dedicated
[Linux README](Linux/README.md).

## Upstream code and license

This fork contains only the Linux sender. The macOS sender, iOS/iPadOS
receiver, website, and Swift wire-protocol definitions live in the
[upstream project](https://github.com/peetzweg/opendisplay), where their
development belongs.
This fork remains available under the repository's [GPL-3.0 license](LICENSE).
