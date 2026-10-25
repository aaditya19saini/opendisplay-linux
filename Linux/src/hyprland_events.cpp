#include "opendisplay/hyprland_events.hpp"

#include "opendisplay/log.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace od {

std::optional<HyprlandEvent> parseHyprlandEvent(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    const auto separator = line.find(">>");
    if (separator == std::string_view::npos || separator == 0) {
        return std::nullopt;
    }
    return HyprlandEvent{
        .name = std::string(line.substr(0, separator)),
        .data = std::string(line.substr(separator + 2)),
    };
}

std::string hyprlandEventSocketPath(const std::string_view runtimeDir,
                                    const std::string_view instanceSignature) {
    if (runtimeDir.empty() || instanceSignature.empty()) return {};
    std::string path(runtimeDir);
    path += "/hypr/";
    path += instanceSignature;
    path += "/.socket2.sock";
    return path;
}

std::optional<Debouncer::Clock::duration> Debouncer::remaining(
    const Clock::time_point now) const {
    if (!deadline_) return std::nullopt;
    return *deadline_ > now ? *deadline_ - now : Clock::duration::zero();
}

bool Debouncer::consumeDue(const Clock::time_point now) {
    if (!deadline_ || now < *deadline_) return false;
    deadline_.reset();
    return true;
}

HyprlandEventWatcher::~HyprlandEventWatcher() { stop(); }

bool HyprlandEventWatcher::start(Callback callback,
                                 const std::chrono::milliseconds reloadDebounce) {
    stop();
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const auto path = hyprlandEventSocketPath(runtimeDir ? runtimeDir : "",
                                              signature ? signature : "");
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        debug("Hyprland event socket is unavailable");
        return false;
    }
    socketFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFd_ < 0) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        debug("Cannot connect to Hyprland event socket " + path + ": "
              + std::strerror(errno));
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }
    int wake[2]{};
    if (::pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        ::close(socketFd_);
        socketFd_ = -1;
        return false;
    }
    wakeRead_ = wake[0];
    wakeWrite_ = wake[1];
    callback_ = std::move(callback);
    running_.store(true);
    thread_ = std::thread(&HyprlandEventWatcher::run, this, reloadDebounce);
    return true;
}

void HyprlandEventWatcher::stop() {
    if (running_.exchange(false) && wakeWrite_ >= 0) {
        const char byte = 0;
        [[maybe_unused]] const auto ignored = ::write(wakeWrite_, &byte, 1);
    }
    if (thread_.joinable()) thread_.join();
    for (int* fd : {&socketFd_, &wakeRead_, &wakeWrite_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    callback_ = {};
}

void HyprlandEventWatcher::run(const std::chrono::milliseconds reloadDebounce) {
    Debouncer reload(reloadDebounce);
    HyprlandEvent reloadEvent{.name = "configreloaded", .data = {}};
    std::string pending;
    std::array<char, 4096> buffer{};
    while (running_.load()) {
        int timeout = -1;
        if (const auto left = reload.remaining(Debouncer::Clock::now())) {
            timeout = static_cast<int>(
                std::chrono::ceil<std::chrono::milliseconds>(*left).count());
        }
        std::array<pollfd, 2> fds{{{socketFd_, POLLIN, 0}, {wakeRead_, POLLIN, 0}}};
        const int ready = ::poll(fds.data(), fds.size(), timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents != 0) break;
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const auto count = ::read(socketFd_, buffer.data(), buffer.size());
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                log("Hyprland event socket closed");
                break;
            }
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const auto event = parseHyprlandEvent(
                    std::string_view(pending).substr(0, newline));
                pending.erase(0, newline + 1);
                if (!event) continue;
                if (event->name == "configreloaded") {
                    reload.trigger(Debouncer::Clock::now());
                } else if (callback_) {
                    callback_(*event);
                }
            }
        }
        if (reload.consumeDue(Debouncer::Clock::now()) && callback_) {
            callback_(reloadEvent);
        }
    }
}

}  // namespace od
