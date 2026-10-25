#include "opendisplay/hyprland_output_controller.hpp"

#include "opendisplay/log.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace od {
namespace {

struct CommandResult {
    bool success = false;
    std::string output;
};

CommandResult hyprctl(const QStringList& arguments) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("hyprctl"), arguments);
    if (!process.waitForStarted(3'000)) {
        return {.success = false, .output = "cannot start hyprctl: "
                    + process.errorString().toStdString()};
    }
    if (!process.waitForFinished(5'000)) {
        process.kill();
        process.waitForFinished();
        return {.success = false, .output = "hyprctl timed out"};
    }
    return {
        .success = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        .output = process.readAll().trimmed().toStdString(),
    };
}

std::string outputSnapshot(const std::vector<DisplayOutput>& outputs) {
    std::ostringstream result;
    bool first = true;
    for (const auto& output : outputs) {
        if (!first) result << ", ";
        result << output.name << ' ' << output.resolution.width << 'x'
               << output.resolution.height << '+' << output.logicalGeometry.x << '+'
               << output.logicalGeometry.y << " scale " << output.scale << " modes [";
        bool firstMode = true;
        for (const auto& mode : output.modes) {
            if (!firstMode) result << ", ";
            result << mode.size.width << 'x' << mode.size.height << '@'
                   << mode.refreshRate;
            firstMode = false;
        }
        result << ']';
        first = false;
    }
    return result.str();
}

std::string scaleText(const double scale) {
    QString text = QString::number(scale, 'f', 4);
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text.toStdString();
}

std::optional<DisplayOutput> findOutput(const std::vector<DisplayOutput>& outputs,
                                        const std::string& name) {
    const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output) {
        return output.name == name;
    });
    return found == outputs.end() ? std::nullopt : std::optional(*found);
}

}  // namespace

bool hyprlandCommandResponseAccepted(const bool processSucceeded,
                                     const std::string_view output) {
    return processSucceeded && (output.empty() || output == "ok");
}

std::vector<DisplayOutput> parseHyprlandOutputs(const std::string_view json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(json.data(), static_cast<qsizetype>(json.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        throw std::runtime_error("hyprctl returned invalid monitor JSON: "
                                 + error.errorString().toStdString());
    }
    std::vector<DisplayOutput> outputs;
    static const QRegularExpression modePattern(
        QStringLiteral(R"(^(\d+)x(\d+)@([0-9.]+)Hz$)"));
    for (const auto& value : document.array()) {
        const auto native = value.toObject();
        DisplayOutput output;
        output.id = QString::number(native.value(QStringLiteral("id")).toInt()).toStdString();
        output.name = native.value(QStringLiteral("name")).toString().toStdString();
        output.resolution = {
            .width = native.value(QStringLiteral("width")).toInt(),
            .height = native.value(QStringLiteral("height")).toInt(),
        };
        output.refreshRate = native.value(QStringLiteral("refreshRate")).toDouble(60.0);
        output.connected = !output.name.empty();
        output.enabled = !native.value(QStringLiteral("disabled")).toBool()
            && output.resolution.width > 0 && output.resolution.height > 0;
        output.focused = native.value(QStringLiteral("focused")).toBool();
        output.scale = native.value(QStringLiteral("scale")).toDouble(1.0);
        if (output.scale <= 0) output.scale = 1.0;
        const double physicalWidth = native.value(QStringLiteral("physicalWidth")).toDouble();
        const double physicalHeight = native.value(QStringLiteral("physicalHeight")).toDouble();
        if (physicalWidth > 0 && physicalHeight > 0) {
            output.physicalSize = PhysicalSize{
                .widthMm = physicalWidth,
                .heightMm = physicalHeight,
            };
        }
        output.logicalGeometry = {
            .x = native.value(QStringLiteral("x")).toInt(),
            .y = native.value(QStringLiteral("y")).toInt(),
            .width = static_cast<int>(std::lround(output.resolution.width / output.scale)),
            .height = static_cast<int>(std::lround(output.resolution.height / output.scale)),
        };
        output.transform = native.value(QStringLiteral("transform")).toInt();
        if (output.transform == 1 || output.transform == 3 || output.transform == 5
            || output.transform == 7) {
            std::swap(output.logicalGeometry.width, output.logicalGeometry.height);
        }
        for (const auto& modeValue : native.value(QStringLiteral("availableModes")).toArray()) {
            const auto match = modePattern.match(modeValue.toString());
            if (!match.hasMatch()) continue;
            output.modes.push_back({
                .id = modeValue.toString().toStdString(),
                .size = {.width = match.captured(1).toInt(),
                         .height = match.captured(2).toInt()},
                .refreshRate = match.captured(3).toDouble(),
            });
        }
        outputs.push_back(std::move(output));
    }
    return outputs;
}

std::string hyprlandMonitorExpression(const std::string& outputName,
                                      const DisplayLayout& layout,
                                      const int transform) {
    const auto& geometry = layout.logicalGeometry;
    std::ostringstream expression;
    expression << "hl.monitor({ output = \"" << outputName << "\", mode = \""
               << layout.resolution.width << 'x' << layout.resolution.height << '@'
               << layout.refreshRate << "\", position = \"" << geometry.x << 'x'
               << geometry.y << "\", scale = " << scaleText(layout.scale);
    if (transform >= 0) expression << ", transform = " << transform;
    expression << " })";
    return expression.str();
}

std::string hyprlandFocusExpression(const std::string& outputName) {
    return "hl.dispatch(hl.dsp.focus({ monitor = \"" + outputName + "\" }))";
}

namespace {

DisplayLayout referenceLayoutFor(const DisplayOutput& reference) {
    DisplayLayout layout;
    layout.resolution = reference.resolution;
    layout.scale = reference.scale;
    layout.logicalGeometry = reference.logicalGeometry;
    layout.refreshRate = std::max(1, static_cast<int>(std::lround(reference.refreshRate)));
    return layout;
}

QString stateHome() {
    const auto configured = qEnvironmentVariable("XDG_STATE_HOME");
    return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : configured;
}

}  // namespace

std::string hyprlandPersistentRulesPath() {
    return (stateHome() + QStringLiteral("/opendisplay/hyprland.lua")).toStdString();
}

std::string hyprlandPersistentRules(const std::string& outputName, const DisplayLayout& layout,
                                    const DisplayOutput& reference,
                                    const std::string& workspaceId) {
    auto rules = "-- Generated by OpenDisplay while a session is active; cleared when it ends.\n"
                 "-- Keeps the virtual monitor's mode across Hyprland config reloads.\n"
        + hyprlandMonitorExpression(reference.name, referenceLayoutFor(reference),
                                    reference.transform)
        + '\n' + hyprlandMonitorExpression(outputName, layout) + '\n';
    if (!workspaceId.empty()) rules += hyprlandWorkspaceRuleExpression(workspaceId, outputName) + '\n';
    return rules;
}

std::string hyprlandWorkspaceId(const std::string& setting, const std::string& receiverName) {
    if (setting.empty()) return {};
    if (setting != "dedicated") return setting;
    std::string name;
    for (const char character : receiverName) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '-'
            || character == '_') {
            name.push_back(character);
        }
    }
    return "name:" + (name.empty() ? std::string("OpenDisplay") : name);
}

std::string hyprlandWorkspaceRuleExpression(const std::string& workspaceId,
                                            const std::string& outputName) {
    return "hl.workspace_rule({ workspace = \"" + workspaceId + "\", monitor = \"" + outputName
        + "\", default = true })";
}

std::string hyprlandMoveWorkspaceExpression(const std::string& workspaceId,
                                            const std::string& monitorName) {
    return "hl.dispatch(hl.dsp.workspace.move({ workspace = \"" + workspaceId
        + "\", monitor = \"" + monitorName + "\" }))";
}

bool hyprlandConfigLoadsPersistentRules() {
    const auto configHome = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config"));
    QFile config(configHome + QStringLiteral("/hypr/hyprland.lua"));
    if (!config.open(QIODevice::ReadOnly)) return false;
    return config.readAll().contains("opendisplay/hyprland.lua");
}

void HyprlandOutputController::writePersistentRules(const std::string& outputName,
                                                    const DisplayLayout& layout,
                                                    const DisplayOutput& reference,
                                                    const std::string& workspaceId) const {
    const auto path = QString::fromStdString(hyprlandPersistentRulesPath());
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        log("Cannot write " + path.toStdString());
        return;
    }
    file.write(QByteArray::fromStdString(
        hyprlandPersistentRules(outputName, layout, reference, workspaceId)));
    if (!file.commit()) log("Cannot write " + path.toStdString());
}

bool HyprlandOutputController::moveWorkspace(const std::string& workspaceId,
                                             const std::string& monitorName) const {
    const auto result = hyprctl({QStringLiteral("eval"),
                                 QString::fromStdString(
                                     hyprlandMoveWorkspaceExpression(workspaceId, monitorName))});
    return hyprlandCommandResponseAccepted(result.success, result.output);
}

void HyprlandOutputController::placeWorkspace(const std::string& workspaceId,
                                              const std::string& outputName) const {
    if (workspaceId.empty()) return;
    const auto rule = hyprctl({QStringLiteral("eval"),
                               QString::fromStdString(
                                   hyprlandWorkspaceRuleExpression(workspaceId, outputName))});
    if (!hyprlandCommandResponseAccepted(rule.success, rule.output)) {
        log("Cannot bind workspace " + workspaceId + " to " + outputName + ": " + rule.output);
        return;
    }
    if (moveWorkspace(workspaceId, outputName)) {
        log("Moved workspace " + workspaceId + " to " + outputName);
        return;
    }
    // Not open yet: focusing it creates it on its bound output.
    const auto focus = hyprctl({QStringLiteral("eval"),
                                QStringLiteral("hl.dispatch(hl.dsp.focus({ workspace = \"%1\" }))")
                                    .arg(QString::fromStdString(workspaceId))});
    if (hyprlandCommandResponseAccepted(focus.success, focus.output)) {
        log("Opened workspace " + workspaceId + " on " + outputName);
    } else {
        log("Cannot open workspace " + workspaceId + ": " + focus.output);
    }
}

void HyprlandOutputController::clearPersistentRules() const {
    QFile::remove(QString::fromStdString(hyprlandPersistentRulesPath()));
}

std::vector<DisplayOutput> HyprlandOutputController::outputs() const {
    const auto result = hyprctl({QStringLiteral("-j"), QStringLiteral("monitors"),
                                 QStringLiteral("all")});
    if (!result.success) {
        throw std::runtime_error("cannot query Hyprland monitors with hyprctl: "
                                 + result.output);
    }
    return parseHyprlandOutputs(result.output);
}

std::string HyprlandOutputController::applyRule(const std::string& name,
                                                const DisplayLayout& ruleLayout,
                                                const int transform) const {
    const auto expression = hyprlandMonitorExpression(name, ruleLayout, transform);
    const auto luaResult = hyprctl({QStringLiteral("eval"),
                                    QString::fromStdString(expression)});
    debug("Hyprland Lua monitor-rule response for " + name + ": "
          + (luaResult.output.empty() ? std::string("<empty>") : luaResult.output));

    CommandResult ruleResult = luaResult;
    std::string ruleMethod = "Lua hl.monitor";
    if (!hyprlandCommandResponseAccepted(luaResult.success, luaResult.output)) {
        const auto& geometry = ruleLayout.logicalGeometry;
        QString legacy = QStringLiteral("%1,%2x%3@%4,%5x%6,%7")
            .arg(QString::fromStdString(name))
            .arg(ruleLayout.resolution.width)
            .arg(ruleLayout.resolution.height)
            .arg(ruleLayout.refreshRate)
            .arg(geometry.x)
            .arg(geometry.y)
            .arg(QString::fromStdString(scaleText(ruleLayout.scale)));
        if (transform >= 0) {
            legacy += QStringLiteral(",transform,%1").arg(transform);
        }
        ruleResult = hyprctl({QStringLiteral("keyword"), QStringLiteral("monitor"),
                              legacy});
        ruleMethod = "legacy keyword monitor";
        debug("Hyprland legacy monitor-rule response for " + name + ": "
              + (ruleResult.output.empty() ? std::string("<empty>")
                                           : ruleResult.output));
    }
    if (!hyprlandCommandResponseAccepted(ruleResult.success, ruleResult.output)) {
        const auto version = hyprctl({QStringLiteral("version")});
        throw std::runtime_error(
            "Hyprland could not install monitor rule for '" + name
            + "'. Lua response: '" + luaResult.output + "'; legacy response: '"
            + ruleResult.output + "'; version: " + version.output);
    }
    return ruleMethod;
}

std::string HyprlandOutputController::pinReference(const DisplayOutput& reference) const {
    return applyRule(reference.name, referenceLayoutFor(reference), reference.transform);
}

void HyprlandOutputController::reapply(const std::string& outputName,
                                       const DisplayLayout& layout,
                                       const DisplayOutput& reference) const {
    pinReference(reference);
    const auto method = applyRule(outputName, layout, -1);
    log("Re-applied monitor rules for " + outputName + " and " + reference.name
        + " after a Hyprland config reload (" + method + ")");
}

DisplayOutput HyprlandOutputController::create(const std::string& outputName,
                                               const DisplayLayout& layout,
                                               const DisplayOutput& detectedReference) const {
    if (findOutput(outputs(), outputName)) {
        throw std::runtime_error("Hyprland output '" + outputName + "' already exists");
    }

    // Hyprland re-arranges outputs whose user rule uses an automatic position
    // whenever another output is hot-plugged. Pin the detected reference to its
    // current state so the new explicit output cannot become the layout anchor.
    const auto referenceMethod = pinReference(detectedReference);
    debug("Pinned reference monitor " + detectedReference.name + " at "
          + std::to_string(detectedReference.logicalGeometry.x) + 'x'
          + std::to_string(detectedReference.logicalGeometry.y) + " through "
          + referenceMethod);

    // Install the exact-name rule before hot-plugging the headless output. This
    // lets Hyprland select the custom mode during its initial output commit,
    // rather than briefly committing the backend's default 1920x1080 mode.
    const auto ruleMethod = applyRule(outputName, layout, -1);

    const auto result = hyprctl({QStringLiteral("output"), QStringLiteral("create"),
                                 QStringLiteral("headless"),
                                 QString::fromStdString(outputName)});
    if (!hyprlandCommandResponseAccepted(result.success, result.output)) {
        throw std::runtime_error("Hyprland could not create headless output '" + outputName
                                 + "': " + result.output);
    }
    QElapsedTimer timer;
    timer.start();
    std::vector<DisplayOutput> current;
    while (timer.elapsed() < 3'000) {
        current = outputs();
        const auto output = findOutput(current, outputName);
        const auto reference = findOutput(current, detectedReference.name);
        if (output && output->resolution.width == layout.resolution.width
            && output->resolution.height == layout.resolution.height
            && output->logicalGeometry.x == layout.logicalGeometry.x
            && output->logicalGeometry.y == layout.logicalGeometry.y
            && std::abs(output->scale - layout.scale) < 0.01 && reference
            && reference->logicalGeometry.x == detectedReference.logicalGeometry.x
            && reference->logicalGeometry.y == detectedReference.logicalGeometry.y) {
            debug("Hyprland applied the virtual-monitor rule through " + ruleMethod);
            return *output;
        }
        QThread::msleep(100);
    }
    throw std::runtime_error(
        "Hyprland created '" + outputName + "' but did not apply the requested "
        + std::to_string(layout.resolution.width) + 'x'
        + std::to_string(layout.resolution.height) + '@'
        + std::to_string(layout.refreshRate) + " mode, position "
        + std::to_string(layout.logicalGeometry.x) + 'x'
        + std::to_string(layout.logicalGeometry.y) + ", scale "
        + scaleText(layout.scale) + " via " + ruleMethod
        + "; last monitor snapshot: " + outputSnapshot(current)
        + ". Hyprland may have re-arranged an output with an automatic position; inspect "
          "`hyprctl rollinglog` for monitor-rule details.");
}

void HyprlandOutputController::focus(const std::string& outputName) const {
    const auto luaResult = hyprctl({
        QStringLiteral("eval"),
        QString::fromStdString(hyprlandFocusExpression(outputName)),
    });
    debug("Hyprland Lua focus response: "
          + (luaResult.output.empty() ? std::string("<empty>") : luaResult.output));

    CommandResult focusResult = luaResult;
    std::string focusMethod = "Lua hl.dsp.focus";
    if (!hyprlandCommandResponseAccepted(luaResult.success, luaResult.output)) {
        focusResult = hyprctl({QStringLiteral("dispatch"),
                               QStringLiteral("focusmonitor"),
                               QString::fromStdString(outputName)});
        focusMethod = "legacy focusmonitor";
        debug("Hyprland legacy focus response: "
              + (focusResult.output.empty() ? std::string("<empty>")
                                            : focusResult.output));
    }
    if (!hyprlandCommandResponseAccepted(focusResult.success, focusResult.output)) {
        throw std::runtime_error(
            "Hyprland could not focus reference monitor '" + outputName
            + "' before opening the share chooser. Lua response: '" + luaResult.output
            + "'; legacy response: '" + focusResult.output + "'");
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2'000) {
        const auto current = outputs();
        const auto output = findOutput(current, outputName);
        if (output && output->focused) {
            debug("Focused reference monitor " + outputName + " through " + focusMethod);
            return;
        }
        QThread::msleep(50);
    }
    throw std::runtime_error(
        "Hyprland accepted the focus request for reference monitor '" + outputName
        + "' but still reports another monitor as focused; refusing to open an inaccessible "
          "share chooser");
}

void HyprlandOutputController::remove(const std::string& outputName) const {
    if (!findOutput(outputs(), outputName)) return;
    const auto result = hyprctl({QStringLiteral("output"), QStringLiteral("remove"),
                                 QString::fromStdString(outputName)});
    if (!result.success) {
        throw std::runtime_error("Hyprland could not remove output '" + outputName
                                 + "': " + result.output);
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3'000) {
        if (!findOutput(outputs(), outputName)) return;
        QThread::msleep(100);
    }
    throw std::runtime_error("Hyprland did not remove output '" + outputName
                             + "' within 3 seconds");
}

void HyprlandOutputController::reload() const {
    const auto result = hyprctl({QStringLiteral("reload")});
    if (!hyprlandCommandResponseAccepted(result.success, result.output)) {
        throw std::runtime_error("Hyprland could not reload the user configuration: "
                                 + result.output);
    }
}

}  // namespace od
