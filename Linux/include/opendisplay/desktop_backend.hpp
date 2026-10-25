#pragma once

#include "opendisplay/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace od {

struct PortalStream {
    std::uint32_t nodeId = 0;
    int logicalWidth = 0;
    int logicalHeight = 0;
};

struct PortalCapture {
    std::string sessionPath;
    PortalStream stream;
    int pipewireFd = -1;
    int captureWidth = 0;
    int captureHeight = 0;
    /// Token the portal issued for this selection (empty if none).
    std::string restoreToken;
};

struct DesktopRequest {
    CaptureMode mode = CaptureMode::Extend;
    PhoneInfo receiver;
    DisplayOptions display;
    int refreshRate = 60;
    bool requestInput = true;
    std::string virtualOutputName;
    std::string restoreToken;
};

/// Compositor-specific output/capture authorization and input injection.
/// Transport, protocol, capture, and encoding remain platform-neutral.
class DesktopBackend {
public:
    virtual ~DesktopBackend() = default;
    virtual PortalCapture start(const DesktopRequest& request) = 0;
    virtual void stop() = 0;
    virtual void pointer(std::string_view phase, double normalizedX, double normalizedY) = 0;
    virtual void scroll(double dx, double dy) = 0;
    /// Changes which workspace the virtual output shows, live (see
    /// DisplayOptions::workspace). Backends without workspaces ignore it.
    virtual void setWorkspace(const std::string& /*workspace*/) {}
};

}  // namespace od
