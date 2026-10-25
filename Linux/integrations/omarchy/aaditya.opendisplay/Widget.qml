import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons

// Bar face of OpenDisplay. Left click opens the popup, right click switches
// extend/mirror, middle click connects or disconnects. All state comes from
// Service.qml, which follows opendisplayd.
BarWidget {
  id: root
  moduleName: "aaditya.opendisplay"

  readonly property string iconTablet: String.fromCodePoint(0xF04F6)
  readonly property string iconCasting: String.fromCodePoint(0xF0118)
  readonly property string iconExtend: String.fromCodePoint(0xF0119)
  readonly property string iconMirror: String.fromCodePoint(0xF11FD)

  property var display: null
  property bool popupOpen: false

  // Only take a bar slot while a session is wanted (connecting, streaming or
  // retrying); start one with opendisplayctl, the GUI or a keybinding.
  readonly property bool shown: display !== null && (display.active || display.streaming || display.busy)
  visible: shown
  onShownChanged: if (!shown) popupOpen = false

  readonly property string status: display ? display.status : "offline"
  readonly property bool streaming: display !== null && display.streaming
  readonly property bool busy: display !== null && display.busy
  readonly property var state: display ? display.state : ({})
  readonly property var stats: display ? display.stats : ({})

  readonly property string barIcon: streaming ? (display.mode === "mirror" ? iconMirror : iconExtend)
                                              : (busy ? iconCasting : iconTablet)
  readonly property color dim: Qt.darker(root.bar.foreground, 1.45)

  function close() { popupOpen = false }

  function statusText() {
    switch (status) {
    case "streaming": return display.mode === "mirror" ? "Mirroring" : "Extending"
    case "connecting": return "Connecting…"
    case "reconnecting":
      var seconds = Math.ceil(Number(state.retryInMs || 0) / 1000)
      return seconds > 0 ? "Retrying in " + seconds + " s" : "Reconnecting…"
    case "offline": return "Service offline"
    default: return "Off"
    }
  }

  function deviceText() {
    if (!state.device) return ""
    var parts = [String(state.device)]
    if (state.transport) parts.push(String(state.transport).toUpperCase())
    if (state.resolution) parts.push(String(state.resolution))
    return parts.join("  ·  ")
  }

  function statsText() {
    if (!stats || !stats.sent) return ""
    return Math.round(Number(stats.fps || 0)) + " fps  ·  capture→sent " + stats.sent.p50
      + " ms (p95 " + stats.sent.p95 + " ms)"
  }

  implicitWidth: shown ? icon.implicitWidth + Style.space(14) : 0
  implicitHeight: barSize

  // The service can come up after the bar, so keep asking until it is there.
  Timer {
    interval: 500
    repeat: true
    triggeredOnStart: true
    running: !root.display
    onTriggered: if (root.bar && root.bar.shell) root.display = root.bar.shell.serviceFor("aaditya.opendisplay")
  }

  // `omarchy-shell aaditya.opendisplay toggle` etc. for keybindings.
  IpcHandler {
    target: "aaditya.opendisplay"

    function open(): void { if (root.shown) root.popupOpen = true }
    function close(): void { root.popupOpen = false }
    function toggle(): void { root.popupOpen = root.shown && !root.popupOpen }
    function toggleMode(): void { if (root.display) root.display.toggleMode() }
  }

  MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    cursorShape: Qt.PointingHandCursor
    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton

    onClicked: function(mouse) {
      if (mouse.button === Qt.LeftButton) root.popupOpen = !root.popupOpen
      else if (!root.display) return
      else if (mouse.button === Qt.RightButton) root.display.toggleMode()
      else if (root.display.active) root.display.disconnectDisplay()
      else root.display.connectDisplay()
    }
    onEntered: if (root.bar) root.bar.showTooltip(root, "OpenDisplay: " + root.statusText()
                                                  + (root.state.device ? " — " + root.state.device : ""))
    onExited: if (root.bar) root.bar.hideTooltip(root)
  }

  Text {
    id: icon
    anchors.centerIn: parent
    textFormat: Text.PlainText
    text: root.barIcon
    color: root.streaming ? Color.accent : (root.busy ? root.bar.barForeground : Qt.darker(root.bar.barForeground, 1.55))
    font.family: root.bar.fontFamily
    font.pixelSize: Style.font.body

    SequentialAnimation on opacity {
      running: root.busy
      loops: Animation.Infinite
      onRunningChanged: if (!running) icon.opacity = 1
      NumberAnimation { to: 0.35; duration: 700 }
      NumberAnimation { to: 1; duration: 700 }
    }
  }

  PopupCard {
    id: popup
    anchorItem: root
    bar: root.bar
    owner: root
    open: root.popupOpen
    contentWidth: popup.fittedContentWidth(Style.space(330))
    contentHeight: popup.fittedContentHeight(column.implicitHeight)

    Column {
      id: column
      anchors.fill: parent
      spacing: Style.space(12)

      Item {
        width: parent.width
        height: Math.max(heading.implicitHeight, statusLabel.implicitHeight)

        Text {
          id: heading
          anchors.left: parent.left
          anchors.verticalCenter: parent.verticalCenter
          textFormat: Text.PlainText
          text: "OpenDisplay"
          color: root.bar.foreground
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.subtitle
          font.bold: true
        }

        Text {
          id: statusLabel
          anchors.right: parent.right
          anchors.verticalCenter: parent.verticalCenter
          textFormat: Text.PlainText
          text: root.statusText()
          color: root.streaming ? Color.accent : root.dim
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.bodySmall
        }
      }

      Text {
        width: parent.width
        visible: text !== ""
        textFormat: Text.PlainText
        text: root.deviceText()
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.body
        elide: Text.ElideRight
      }

      Text {
        width: parent.width
        visible: text !== "" && !root.streaming
        textFormat: Text.PlainText
        text: root.display && root.display.lastError !== "" ? root.display.lastError
              : String(root.state.detail || (root.status === "offline"
                  ? "Start it with: systemctl --user start opendisplayd" : ""))
        color: root.display && root.display.lastError !== "" ? Color.urgent : root.dim
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.bodySmall
        wrapMode: Text.Wrap
      }

      Text {
        width: parent.width
        visible: text !== ""
        textFormat: Text.PlainText
        text: root.statsText()
        color: root.dim
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
      }

      PanelSeparator { width: parent.width }

      Text {
        textFormat: Text.PlainText
        text: "Mode"
        color: root.dim
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
      }

      ButtonGroup {
        focusable: false
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        value: root.display ? root.display.mode : "extend"
        options: [
          { value: "extend", label: "Extend", icon: root.iconExtend, tooltip: "Use it as an extra screen" },
          { value: "mirror", label: "Mirror", icon: root.iconMirror, tooltip: "Show this screen on it" }
        ]
        onChanged: function(value) { if (root.display) root.display.setMode(value) }
      }

      Text {
        visible: root.display !== null && root.display.mode === "extend"
        textFormat: Text.PlainText
        text: "Position (applies on next connect)"
        color: root.dim
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
      }

      ButtonGroup {
        visible: root.display !== null && root.display.mode === "extend"
        focusable: false
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        value: root.display ? String(root.display.settings.extendTo || "right") : "right"
        options: [
          { value: "left", label: "Left" },
          { value: "right", label: "Right" },
          { value: "top", label: "Top" },
          { value: "bottom", label: "Bottom" }
        ]
        onChanged: function(value) {
          if (!root.display) return
          root.display.setSettings({
            extendTo: value,
            alignTo: value === "top" || value === "bottom" ? "right" : "bottom"
          })
        }
      }

      Text {
        visible: root.display !== null && root.display.mode === "extend"
        textFormat: Text.PlainText
        text: "Workspace on the display"
        color: root.dim
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
      }

      ButtonGroup {
        visible: root.display !== null && root.display.mode === "extend"
        focusable: false
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        value: {
          var current = root.display ? String(root.display.settings.workspace || "") : ""
          return current === "" || current === "auto" ? "auto" : (current === "dedicated" ? "dedicated" : "")
        }
        options: [
          { value: "auto", label: "Auto", tooltip: "Let Hyprland choose" },
          { value: "dedicated", label: "Dedicated", tooltip: "A workspace of its own, named after the device" }
        ]
        onChanged: function(value) { if (root.display) root.display.setSettings({ workspace: value }) }
      }

      ButtonGroup {
        visible: root.display !== null && root.display.mode === "extend"
        focusable: false
        spacing: Style.space(4)
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        value: root.display ? String(root.display.settings.workspace || "") : ""
        options: ["1", "2", "3", "4", "5", "6", "7", "8", "9"]
        onChanged: function(value) { if (root.display) root.display.setSettings({ workspace: value }) }
      }

      PanelSeparator { width: parent.width }

      Row {
        spacing: Style.space(8)

        Button {
          text: root.display && root.display.active ? "Disconnect" : "Connect"
          bordered: true
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          onClicked: {
            if (!root.display) return
            if (root.display.active) root.display.disconnectDisplay()
            else root.display.connectDisplay()
          }
        }

        Button {
          text: "Autoconnect"
          bordered: true
          selected: root.state.autoconnect === true
          tooltipText: "Connect automatically when the service starts"
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          onClicked: if (root.display) root.display.setAutoconnect(root.state.autoconnect !== true)
        }
      }
    }
  }
}
