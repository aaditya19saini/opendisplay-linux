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

This fork adds a Linux sender while retaining the original OpenDisplay
iOS/iPadOS receiver and wire protocol. The Linux application supports:

- KDE Plasma Wayland through `xdg-desktop-portal-kde` and KScreen.
- Hyprland through `xdg-desktop-portal-hyprland` and headless outputs.
- Wi-Fi discovery with Avahi and USB connectivity through `usbmuxd`.
- PipeWire capture and FFmpeg H.264 encoding, including supported hardware
  encoders.
- A command-line client and an experimental Qt Quick/Kirigami GUI.

Other compositors and non-Wayland sessions are currently unsupported. Portal
behavior also depends on compositor and desktop versions, so successful setup
on one system does not guarantee identical behavior elsewhere.

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
package include both `opendisplay-linux` and `opendisplay-gui`.

For dependencies, package-building commands, Wi-Fi and USB setup, monitor
placement and scaling options, KDE/Hyprland notes, and troubleshooting, use the
dedicated [Linux README](Linux/README.md).

## Upstream code and license

This fork contains only the Linux sender. The macOS sender, iOS/iPadOS
receiver, website, and Swift wire-protocol definitions live in the
[upstream project](https://github.com/peetzweg/opendisplay), where their
development belongs.
This fork remains available under the repository's [GPL-3.0 license](LICENSE).
