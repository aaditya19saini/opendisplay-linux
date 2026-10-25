#include "opendisplay/session_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

namespace od {

SessionLock::~SessionLock() { release(); }

std::string SessionLock::path() {
    if (const char* override = std::getenv("OPENDISPLAY_SESSION_LOCK"); override && *override) {
        return override;
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::string(runtime && *runtime ? runtime : "/tmp") + "/opendisplay/session.lock";
}

bool SessionLock::acquire(const std::string& owner, std::string* holder) {
    if (fd_ >= 0) return true;
    const auto file = path();
    const auto directory = file.substr(0, file.rfind('/'));
    ::mkdir(directory.c_str(), 0700);
    const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // can't create a lock: don't block streaming over it
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (holder) {
            char buffer[256]{};
            const auto count = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
            std::string text(buffer, count > 0 ? static_cast<std::size_t>(count) : 0);
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            *holder = text.empty() ? std::string("another OpenDisplay session") : text;
        }
        ::close(fd);
        return false;
    }
    const auto record = owner + " (pid " + std::to_string(::getpid()) + ")\n";
    if (::ftruncate(fd, 0) == 0) {
        [[maybe_unused]] const auto written = ::pwrite(fd, record.data(), record.size(), 0);
    }
    fd_ = fd;
    return true;
}

void SessionLock::release() {
    if (fd_ < 0) return;
    [[maybe_unused]] const auto truncated = ::ftruncate(fd_, 0);
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace od
