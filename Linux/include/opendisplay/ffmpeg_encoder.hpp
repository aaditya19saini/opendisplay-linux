#pragma once

#include "opendisplay/types.hpp"
#include "opendisplay/video_encoder.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace od {

/// FFmpeg filter chain that fits the captured frame into width x height,
/// preserving aspect ratio and padding with black (never stretching). VA-API
/// additionally converts to NV12 and uploads to the GPU.
std::string encoderVideoFilter(EncoderKind kind, int width, int height);

/// Low-latency FFmpeg subprocess adapter. Capture threads only replace a
/// single pending frame, preventing latency from growing under encoder load.
class FfmpegEncoder final : public VideoEncoder {
public:
    FfmpegEncoder() = default;
    ~FfmpegEncoder() override;
    FfmpegEncoder(const FfmpegEncoder&) = delete;
    FfmpegEncoder& operator=(const FfmpegEncoder&) = delete;

    void start(EncoderConfig config, FrameCallback callback) override;
    void submit(CapturedFrame frame) override;
    void requestKeyframe() override;
    void stop() override;
    [[nodiscard]] std::string selectedEncoder() const override;

private:
    void run();
    void startProcess(const VideoFormat& input);
    void stopProcess();
    void readOutput(int fd);
    void consumeNal(std::string nal, std::string& accessUnit, bool& hasVcl);
    void emitAccessUnit(std::string accessUnit);
    std::vector<std::string> arguments(const VideoFormat& input) const;
    EncoderKind chooseEncoder() const;

    EncoderConfig config_;
    FrameCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CapturedFrame> pending_;
    std::deque<std::int64_t> timestamps_;
    std::thread worker_;
    std::thread reader_;
    bool running_ = false;
    bool restartRequested_ = false;
    int inputFd_ = -1;
    int outputFd_ = -1;
    int childPid_ = -1;
    VideoFormat inputFormat_;
    EncoderKind selected_ = EncoderKind::Software;
};

}  // namespace od
