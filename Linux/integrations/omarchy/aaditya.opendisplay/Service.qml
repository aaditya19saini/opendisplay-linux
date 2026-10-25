import QtQuick
import Quickshell
import Quickshell.Io

// One live view of opendisplayd for every bar: a long-running
// `opendisplayctl watch --json` streams state and stats as JSON lines, and
// actions are fire-and-forget opendisplayctl calls whose results come back
// through the same stream. opendisplayctl starts the daemon when needed.
Item {
  id: root

  property var shell: null

  property bool available: false     // the watch stream is connected
  property var state: ({})
  property var stats: ({})
  property string lastError: ""

  readonly property string status: available ? String(state.status || "idle") : "offline"
  readonly property string mode: String(state.mode || "extend")
  readonly property bool active: state.active === true
  readonly property bool streaming: status === "streaming"
  readonly property bool busy: status === "connecting" || status === "reconnecting"
  readonly property var settings: state.settings || ({})

  // Actions run one at a time; clicks that land while one is in flight queue.
  property var queue: []

  function run(args) {
    lastError = ""
    queue = queue.concat([args])
    if (!action.running) next()
  }

  function next() {
    if (queue.length === 0) return
    var args = queue[0]
    queue = queue.slice(1)
    action.command = ["opendisplayctl"].concat(args)
    action.running = true
  }

  function connectDisplay() { run(["connect"]) }
  function disconnectDisplay() { run(["disconnect"]) }
  function setMode(value) { run([value]) }   // `extend` / `mirror` connect or switch live
  function toggleMode() { run(["toggle-mode"]) }
  function setSettings(values) {
    var args = ["set"]
    for (var key in values) args.push(key + "=" + values[key])
    run(args)
  }
  function setAutoconnect(enabled) { run(["autoconnect", enabled ? "on" : "off"]) }

  function handleLine(line) {
    var text = String(line).trim()
    if (text === "") return
    var message
    try { message = JSON.parse(text) } catch (error) { return }
    if (message.ok === false) {
      lastError = String(message.error || "OpenDisplay error")
      return
    }
    available = true
    if (message.event === "stats") stats = message.stats || ({})
    else if (message.state) {
      state = message.state
      if (String(message.state.status) !== "streaming") stats = ({})
    }
  }

  Process {
    id: watcher
    command: ["opendisplayctl", "watch", "--json"]
    running: true
    stdout: SplitParser { onRead: function(line) { root.handleLine(line) } }
    onExited: {
      root.available = false
      retry.start()
    }
  }

  // The daemon restarts (updates, crashes); reattach shortly after.
  Timer {
    id: retry
    interval: 3000
    onTriggered: watcher.running = true
  }

  Process {
    id: action
    onExited: Qt.callLater(root.next)
    stderr: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var text = String(this.text || "").trim()
        if (text !== "") root.lastError = text.replace(/^opendisplayctl:\s*/, "")
      }
    }
  }
}
