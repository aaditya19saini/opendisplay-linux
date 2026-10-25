#include "opendisplay/session_lock.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <string>

int main() {
    char directory[] = "/tmp/od-lock-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const std::string file = std::string(directory) + "/nested/session.lock";
    ::setenv("OPENDISPLAY_SESSION_LOCK", file.c_str(), 1);
    assert(od::SessionLock::path() == file);

    {
        od::SessionLock first;
        od::SessionLock second;
        assert(first.acquire("opendisplayd"));
        assert(first.held());
        assert(first.acquire("opendisplayd"));  // re-acquire is a no-op

        std::string holder;
        assert(!second.acquire("opendisplay-linux", &holder));
        assert(!second.held());
        assert(holder == "opendisplayd (pid " + std::to_string(::getpid()) + ")");

        first.release();
        assert(second.acquire("opendisplay-linux"));
    }  // destructor releases

    // A holder that dies without releasing (crash, kill -9) frees the lock.
    int ready[2];
    assert(::pipe(ready) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        od::SessionLock crashed;
        const bool ok = crashed.acquire("crashing-daemon");
        const char byte = ok ? 1 : 0;
        [[maybe_unused]] auto written = ::write(ready[1], &byte, 1);
        ::pause();  // killed below; never releases
        ::_exit(0);
    }
    char byte = 0;
    assert(::read(ready[0], &byte, 1) == 1 && byte == 1);
    {
        od::SessionLock blocked;
        std::string holder;
        assert(!blocked.acquire("cli", &holder));
        assert(holder.starts_with("crashing-daemon (pid "));
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    od::SessionLock after;
    assert(after.acquire("cli"));
    return 0;
}
