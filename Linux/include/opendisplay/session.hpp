#pragma once

#include "opendisplay/desktop_backend.hpp"
#include "opendisplay/video_encoder.hpp"
#include "opendisplay/latency_stats.hpp"
#include "opendisplay/pipewire_capture.hpp"
#include "opendisplay/socket.hpp"
#include "opendisplay/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <thread>

namespace od {

/// Latest 5-second latency window, as published to control clients.
struct SessionStats {
    double fps = 0;
    LatencySummary encode;
    LatencySummary sent;
};

class Session {
public:
    Session(Options options, std::unique_ptr<DesktopBackend> desktop);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start(const Endpoint& endpoint);
    /// Called by the Qt main loop. Returns false after disconnection.
    bool tick();
    void stop();

    /// Switches between extend and mirror without dropping the receiver
    /// connection (rebuilds only the capture pipeline).
    void switchMode(CaptureMode mode);
    void setRestoreToken(std::string token);
    /// Most recent restore token issued by the portal (empty if none).
    [[nodiscard]] std::string restoreToken() const;
    [[nodiscard]] std::optional<PhoneInfo> receiver() const;
    /// (serial, stats); the serial increases each time a new window is ready.
    [[nodiscard]] std::pair<std::uint64_t, SessionStats> stats() const;
    [[nodiscard]] std::string encoderName() const;

private:
    enum class EventKind { Hello, Touch, Scroll };
    struct Event {
        EventKind kind = EventKind::Touch;
        PhoneInfo phone;
        std::string phase;
        double x = 0;
        double y = 0;
    };

    void receiveLoop();
    void startPipeline(const PhoneInfo& phone);
    void stopPipeline();
    bool send(std::string_view payload);
    void queue(Event event);

    Options options_;
    Socket socket_;
    std::unique_ptr<DesktopBackend> desktop_;
    PipeWireCapture capture_;
    std::unique_ptr<VideoEncoder> encoder_ = makeVideoEncoder();
    std::thread receiver_;
    std::atomic_bool connected_ = false;
    std::mutex sendMutex_;
    mutable std::mutex stateMutex_;
    mutable std::mutex statsMutex_;
    SessionStats stats_;
    std::uint64_t statsSerial_ = 0;
    std::string restoreToken_;
    std::condition_variable helloCondition_;
    std::optional<PhoneInfo> phone_;
    std::optional<PhoneInfo> activePhone_;
    std::deque<Event> events_;
    std::chrono::steady_clock::time_point lastPing_{};
    std::chrono::steady_clock::time_point reconfigureAfter_{};
    // Capture -> encoded access unit, and capture -> written to the socket.
    LatencyWindow encodeLatency_;
    LatencyWindow sendLatency_;
    std::int64_t lastReceiverStatsLogMs_ = 0;
    bool pipelineRunning_ = false;
    bool reconfigurePending_ = false;
};

}  // namespace od
