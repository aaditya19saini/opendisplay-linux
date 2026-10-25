# OpenDisplay Fixes

This file tracks the fixes and requirements raised during review. Keep it updated as additional fixes are requested.

## Phase 1: display correctness and latency visibility

- [x] Reapply the active Hyprland virtual monitor rules after a config reload so layout toggles and other reloads do not leave the display at the catch-all mode.
- [x] Fix the black screen/lag reported when switching to Dwindle mode or Scroll mode; the active OpenDisplay screen should remain visible and recover promptly after either layout change.
- [x] Preserve the source aspect ratio with letterboxing; never stretch the image. Apply this to Extend and Mirror modes.
- [x] Make these fixes device-agnostic for every supported receiver connected in the future. Derive virtual display geometry from the currently connected receiver; do not hard-code one device's resolution or identity. Reapply rules only for the active session and clean them up when it ends.
- [x] Let the user choose which desktop/workspace appears on the extended display, rather than forcing a single workspace arrangement.
- [x] Add per-frame latency measurements through capture, encoding, and socket send, and log receiver stats.
- [x] Use low-latency FFmpeg settings and the actual capture frame rate when available.
- [x] Add tests for Hyprland reload events, debounce behavior, and aspect-preserving filter construction.

## Status (2026-10-25)

- Reload handling: `18a5fc2` (runtime re-apply) and `218f692` (rules written to
  `~/.local/state/opendisplay/hyprland.lua`, loaded by `hyprland.lua`, so reloads
  no longer flip the mode or restart the stream). Verified with two layout
  toggles plus `hyprctl reload`: no PipeWire renegotiation, no encoder restart.
- Letterboxing: `18a5fc2` (FFmpeg filter) and `27c6dc0` (in-process encoder).
- Geometry comes from each receiver's hello; virtual outputs are named per
  device (`OpenDisplay-u<udid>`); rules and outputs are removed when a session
  ends and stale ones at service start.
- Latency: `27c6dc0` in-process encoder. Receiver e2e on 60 fps video went
  from p50 74 ms / p95 86 ms / 46 fps to p50 14 ms / p95 18 ms / 58 fps.
- Workspace selection: Auto / Dedicated (named after the device) / 1-9, in the
  bar panel or `opendisplayctl set workspace=…`; applied live, previous choice
  goes back to the laptop screen, all return when the display disconnects.

## Acceptance checks

- Connect supported receivers with different resolutions/aspect ratios and verify each gets suitable virtual display geometry.
- Toggle the Hyprland layout or reload its config during an Extend session; verify the active virtual monitor returns to its intended mode.
- Switch to Dwindle mode and Scroll mode during an active session; verify the iPad screen does not go black and resumes promptly.
- Choose a workspace for the extended display and verify the selected workspace is shown there.
- Verify Extend and Mirror show letterboxing where needed and never distort the image.
- Disconnect a receiver and confirm the active session's virtual output and watcher are cleaned up before connecting another receiver.

## Workspace behavior to discuss

- Can the user assign an existing workspace to the extended display, create a dedicated workspace there, or both?
- Where should workspace selection live (for example, the OpenDisplay panel or the desktop environment's normal workspace controls)?
