#pragma once

#include <string>

namespace od {

/// Machine-wide (per user) guard so only one OpenDisplay session runs at a
/// time, whichever program starts it (opendisplayd, the standalone CLI).
/// Uses flock(2) on $XDG_RUNTIME_DIR/opendisplay/session.lock: the kernel
/// drops the lock when the holder exits or crashes, so it can never go stale.
class SessionLock {
public:
    SessionLock() = default;
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    /// Takes the lock, recording `owner` (e.g. "opendisplayd") and this PID.
    /// Returns false when another session holds it; `holder` then describes
    /// that session ("opendisplay-linux (pid 1234)") when known.
    bool acquire(const std::string& owner, std::string* holder = nullptr);
    void release();
    [[nodiscard]] bool held() const { return fd_ >= 0; }

    /// Path of the lock file; tests may override it.
    static std::string path();

private:
    int fd_ = -1;
};

}  // namespace od
