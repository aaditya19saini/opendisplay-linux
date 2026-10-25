#include "opendisplay/hyprland_portal.hpp"

#include "opendisplay/log.hpp"

#include <QCoreApplication>
#include <QDBusObjectPath>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace od {
namespace {

constexpr auto screenCastInterface = "org.freedesktop.portal.ScreenCast";

}  // namespace

HyprlandPortal::HyprlandPortal(QObject* parent) : QObject(parent), portal_(this) {}

HyprlandPortal::~HyprlandPortal() { stop(); }

PortalCapture HyprlandPortal::start(const DesktopRequest& request) {
    stop();
    const int requestedWidth = std::max(2, request.receiver.pixelsWide);
    const int requestedHeight = std::max(2, request.receiver.pixelsHigh);
    int captureWidth = requestedWidth;
    int captureHeight = requestedHeight;
    int captureLogicalWidth = requestedWidth;
    int captureLogicalHeight = requestedHeight;
    std::string inputOutputName;

    try {
        const auto currentOutputs = outputs_.outputs();
        const auto reference = selectReferenceOutput(currentOutputs,
                                                     request.display.referenceMonitor);
        inputOutputName = reference.name;
        if (request.mode == CaptureMode::Extend) {
            const auto layout = planDisplayLayout(reference, request.receiver, request.display,
                                                  request.refreshRate,
                                                  DisplayModePolicy::IntegerLogicalSize);
            log("Reference monitor: " + reference.name + " "
                + std::to_string(layout.reference.resolution.width) + 'x'
                + std::to_string(layout.reference.resolution.height) + " at scale "
                + std::to_string(layout.reference.scale));
            if (layout.usedPhysicalSizing) {
                debug("Virtual scale derived from reference and receiver physical sizes");
            } else if (!request.display.virtualScale) {
                debug("Virtual scale derived from the receiver native scale");
            }
            if (layout.adjustedResolution) {
                debug("Adjusted virtual resolution for integer logical geometry: "
                      + std::to_string(layout.resolution.width) + 'x'
                      + std::to_string(layout.resolution.height));
            }
            virtualOutputName_ = !request.virtualOutputName.empty()
                ? request.virtualOutputName
                : "OpenDisplay-" + std::to_string(QCoreApplication::applicationPid());
            // A stable name can be left behind by a crashed session.
            for (const auto& output : currentOutputs) {
                if (output.name == virtualOutputName_) {
                    log("Removing leftover virtual monitor " + virtualOutputName_);
                    outputs_.remove(virtualOutputName_);
                    break;
                }
            }
            outputCreated_ = true;
            referencePinned_ = true;
            const auto configured = outputs_.create(virtualOutputName_, layout, reference);
            inputOutputName = configured.name;
            // Any Hyprland config reload (layout toggles, theme changes, saving the
            // config) drops runtime monitor rules, and the virtual output then
            // falls back to the user's catch-all rule (wrong size and scale).
            // Re-apply our rules after every reload while the session runs.
            const bool watching = events_.start(
                [this, name = virtualOutputName_, layout, reference](
                    const HyprlandEvent& event) {
                    if (event.name == "configreloaded") {
                        try {
                            outputs_.reapply(name, layout, reference);
                        } catch (const std::exception& error) {
                            log(std::string("Cannot re-apply monitor rules after a "
                                            "Hyprland reload: ") + error.what());
                        }
                    } else if (event.name == "monitorremoved" && event.data == name) {
                        log("Hyprland removed the virtual monitor " + name);
                    }
                });
            if (!watching) {
                log("Hyprland event socket unavailable; a config reload will reset the "
                    "virtual monitor until the session restarts");
            }
            captureWidth = layout.resolution.width;
            captureHeight = layout.resolution.height;
            captureLogicalWidth = layout.logicalGeometry.width;
            captureLogicalHeight = layout.logicalGeometry.height;
            log("Hyprland virtual monitor ready: " + virtualOutputName_ + " "
                + std::to_string(layout.resolution.width) + 'x'
                + std::to_string(layout.resolution.height) + '@'
                + std::to_string(layout.refreshRate) + ", scale "
                + std::to_string(layout.scale));
            log("In the screen-sharing dialog, select monitor " + virtualOutputName_ + '.');
        } else {
            log("In the screen-sharing dialog, select monitor " + reference.name + '.');
            captureWidth = reference.resolution.width;
            captureHeight = reference.resolution.height;
            captureLogicalWidth = reference.logicalGeometry.width;
            captureLogicalHeight = reference.logicalGeometry.height;
        }

        // Hot-plugging a headless output can make it Hyprland's active monitor.
        // Restore both monitor focus and cursor placement before XDPH creates
        // its chooser, otherwise the authorization UI can appear on the output
        // that is not capturable until the chooser is accepted.
        outputs_.focus(reference.name);
        log("Reference monitor focused; opening the share chooser on " + reference.name
            + "…");

        const auto availableSources = portal_.property(screenCastInterface,
                                                       "AvailableSourceTypes");
        if (availableSources.isValid() && (availableSources.toUInt() & 1U) == 0) {
            throw std::runtime_error(
                "the active Hyprland portal does not advertise monitor capture");
        }
        const auto availableCursorModes = portal_.property(screenCastInterface,
                                                           "AvailableCursorModes");
        log("Requesting a Hyprland screen-cast portal session…");
        sessionPath_ = portal_.createSession(screenCastInterface);

        QVariantMap sourceOptions;
        sourceOptions.insert(QStringLiteral("types"), 1U);  // MONITOR
        sourceOptions.insert(QStringLiteral("multiple"), false);
        const bool canEmbedCursor = !availableCursorModes.isValid()
            || (availableCursorModes.toUInt() & 2U) != 0;
        sourceOptions.insert(QStringLiteral("cursor_mode"), canEmbedCursor ? 2U : 1U);
        // Ask for a persistent restore token so later sessions with the same
        // (stable) output name skip the share dialog.
        sourceOptions.insert(QStringLiteral("persist_mode"), 2U);
        if (!request.restoreToken.empty()) {
            sourceOptions.insert(QStringLiteral("restore_token"),
                                 QString::fromStdString(request.restoreToken));
        }
        portal_.request(screenCastInterface, QStringLiteral("SelectSources"),
                        {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
                        std::move(sourceOptions));
        // XDPH normally launches its picker from Start. Reassert focus here in
        // case creating the portal session changed the active surface.
        outputs_.focus(reference.name);
        const auto started = portal_.request(
            screenCastInterface, QStringLiteral("Start"),
            {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString()}, {});
        const auto parsed = XdgPortal::firstStream(
            started.value(QStringLiteral("streams")),
            std::max(1, captureWidth), std::max(1, captureHeight));
        if (!parsed) {
            throw std::runtime_error("portal did not return a PipeWire stream");
        }
        auto stream = *parsed;
        const auto issuedToken = XdgPortal::unwrap(started.value(QStringLiteral("restore_token")))
                                     .toString().toStdString();
        if (!issuedToken.empty()) debug("Portal issued a restore token");
        if (request.mode == CaptureMode::Extend) {
            stream.logicalWidth = captureLogicalWidth;
            stream.logicalHeight = captureLogicalHeight;
        }
        if (request.requestInput) {
            pointer_.start(inputOutputName);
            inputEnabled_ = true;
        }
        const int fd = portal_.openPipeWireRemote(sessionPath_);
        log("Hyprland portal session ready; PipeWire node "
            + std::to_string(stream.nodeId) + ", portal size "
            + std::to_string(stream.logicalWidth) + 'x'
            + std::to_string(stream.logicalHeight));
        return {
            .sessionPath = sessionPath_.toStdString(),
            .stream = stream,
            .pipewireFd = fd,
            .captureWidth = captureWidth,
            .captureHeight = captureHeight,
            .restoreToken = issuedToken,
        };
    } catch (...) {
        stop();
        throw;
    }
}

void HyprlandPortal::stop() {
    // Stop watching first: our own teardown reloads the config, and the
    // removed output must not get its rules re-applied.
    events_.stop();
    inputEnabled_ = false;
    pointer_.stop();
    if (!sessionPath_.isEmpty()) {
        portal_.closeSession(sessionPath_);
        sessionPath_.clear();
    }
    if (outputCreated_) {
        try {
            outputs_.remove(virtualOutputName_);
        } catch (const std::exception& error) {
            debug(std::string("Cannot remove Hyprland virtual output ")
                  + virtualOutputName_ + ": " + error.what());
        }
    }
    outputCreated_ = false;
    virtualOutputName_.clear();
    if (referencePinned_) {
        try {
            outputs_.reload();
            debug("Reloaded Hyprland configuration after removing temporary monitor rules");
        } catch (const std::exception& error) {
            debug(std::string("Cannot restore Hyprland monitor rules: ") + error.what());
        }
    }
    referencePinned_ = false;
}

void HyprlandPortal::pointer(const std::string_view phase, const double normalizedX,
                             const double normalizedY) {
    if (inputEnabled_) pointer_.pointer(phase, normalizedX, normalizedY);
}

void HyprlandPortal::scroll(const double dx, const double dy) {
    if (inputEnabled_) pointer_.scroll(dx, dy);
}

}  // namespace od
