#pragma once

#include "opendisplay/video_encoder.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace od {

/// Fits a source rectangle into a target, preserving aspect ratio with even
/// dimensions and offsets (needed for 4:2:0 chroma). Used for letterboxing.
struct FitRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
FitRect letterboxRect(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight);

/// One encoder choice: codec name plus VA-API node when relevant.
struct EncoderChoice {
    EncoderKind kind = EncoderKind::Software;
    std::string device;  // VA-API render node
};

/// Picks the encoder for `Auto`: a cached benchmark result for this size when
/// present, otherwise a short synthetic benchmark of every usable encoder
/// (median send-to-packet latency), cached in ~/.cache/opendisplay.
EncoderChoice chooseAutoEncoder(int width, int height, int fps, int bitrate);

/// Opened encoder + conversion state for one input format (defined in the .cpp).
struct AvCodecState;

/// In-process libavcodec H.264 encoder. Compared with the FFmpeg subprocess
/// it emits each access unit as soon as the encoder returns it (no waiting
/// for the next frame's delimiter), avoids the pipe copy, and feeds NVENC
/// the captured BGRx buffer directly when no scaling is needed.
class AvEncoder final : public VideoEncoder {
public:
    AvEncoder();
    ~AvEncoder() override;
    AvEncoder(const AvEncoder&) = delete;
    AvEncoder& operator=(const AvEncoder&) = delete;

    void start(EncoderConfig config, FrameCallback callback) override;
    void submit(CapturedFrame frame) override;
    void requestKeyframe() override;
    void stop() override;
    [[nodiscard]] std::string selectedEncoder() const override;

private:
    void run();

    EncoderConfig config_;
    FrameCallback callback_;
    std::unique_ptr<AvCodecState> codec_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CapturedFrame> pending_;
    std::thread worker_;
    bool running_ = false;
    std::atomic_bool keyframeRequested_ = false;
    std::string selectedName_;
};

}  // namespace od
