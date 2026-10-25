#pragma once

#include "opendisplay/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace od {

struct EncoderConfig {
    EncoderKind kind = EncoderKind::Auto;
    /// VA-API render node; empty picks the first node that can encode H.264.
    std::string vaapiDevice;
    int outputWidth = 0;
    int outputHeight = 0;
    int fps = 60;
    int bitrate = 18'000'000;
};

/// H.264 encoder fed with captured frames. Implementations keep at most one
/// pending frame (latest wins) so latency cannot build up under load, and
/// call the callback with one complete Annex-B access unit per frame
/// (4-byte start codes, AUD first, SPS/PPS on keyframes).
class VideoEncoder {
public:
    using FrameCallback = std::function<void(EncodedFrame)>;

    virtual ~VideoEncoder() = default;
    virtual void start(EncoderConfig config, FrameCallback callback) = 0;
    virtual void submit(CapturedFrame frame) = 0;
    virtual void requestKeyframe() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::string selectedEncoder() const = 0;
};

/// In-process libavcodec encoder unless OPENDISPLAY_ENCODER_BACKEND=cli
/// selects the FFmpeg subprocess.
std::unique_ptr<VideoEncoder> makeVideoEncoder();

/// Render nodes (/dev/dri/renderD*) whose kernel driver is not NVIDIA's
/// (nvidia-vaapi-driver is decode-only), in node order.
std::vector<std::string> vaapiEncodeCandidates();

}  // namespace od
