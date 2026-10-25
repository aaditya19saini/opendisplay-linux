#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace od {

/// One line from Hyprland's event socket (`.socket2.sock`): `name>>data`.
struct HyprlandEvent {
    std::string name;
    std::string data;
};

std::optional<HyprlandEvent> parseHyprlandEvent(std::string_view line);

/// Path of Hyprland's event socket for the given runtime dir and instance
/// signature, or an empty string when either is missing.
std::string hyprlandEventSocketPath(std::string_view runtimeDir,
                                    std::string_view instanceSignature);

/// Collapses bursts of triggers into one action that fires `delay` after the
/// last trigger. Rapid layout toggles each reload the Hyprland config, and
/// re-applying monitor rules once per burst is enough.
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;

    explicit Debouncer(Clock::duration delay) : delay_(delay) {}

    void trigger(Clock::time_point now) { deadline_ = now + delay_; }
    [[nodiscard]] bool pending() const { return deadline_.has_value(); }
    /// Time until the action is due, or nullopt when nothing is pending.
    [[nodiscard]] std::optional<Clock::duration> remaining(Clock::time_point now) const;
    /// True once (and clears the pending state) when the deadline has passed.
    bool consumeDue(Clock::time_point now);

private:
    Clock::duration delay_;
    std::optional<Clock::time_point> deadline_;
};

/// Watches Hyprland's event socket on a background thread. Only
/// `configreloaded` is debounced; other events are delivered as they arrive.
/// Callbacks run on the watcher thread.
class HyprlandEventWatcher {
public:
    using Callback = std::function<void(const HyprlandEvent&)>;

    HyprlandEventWatcher() = default;
    ~HyprlandEventWatcher();
    HyprlandEventWatcher(const HyprlandEventWatcher&) = delete;
    HyprlandEventWatcher& operator=(const HyprlandEventWatcher&) = delete;

    /// Returns false when the event socket is unavailable.
    bool start(Callback callback,
               std::chrono::milliseconds reloadDebounce = std::chrono::milliseconds(150));
    void stop();

private:
    void run(std::chrono::milliseconds reloadDebounce);

    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int socketFd_ = -1;
    int wakeRead_ = -1;
    int wakeWrite_ = -1;
};

}  // namespace od
