#include "opendisplay/hyprland_events.hpp"
#include "opendisplay/latency_stats.hpp"

#include <cassert>
#include <chrono>

using namespace std::chrono_literals;

namespace {

void parsesEvents() {
    const auto reload = od::parseHyprlandEvent("configreloaded>>\n");
    assert(reload && reload->name == "configreloaded" && reload->data.empty());

    const auto removed = od::parseHyprlandEvent("monitorremoved>>OpenDisplay-42");
    assert(removed && removed->name == "monitorremoved"
           && removed->data == "OpenDisplay-42");

    // Data may itself contain ">>"; only the first separator splits.
    const auto title = od::parseHyprlandEvent("windowtitle>>a>>b\r\n");
    assert(title && title->name == "windowtitle" && title->data == "a>>b");

    assert(!od::parseHyprlandEvent(""));
    assert(!od::parseHyprlandEvent("no separator"));
    assert(!od::parseHyprlandEvent(">>missing name"));
}

void buildsSocketPath() {
    assert(od::hyprlandEventSocketPath("/run/user/1000", "abc")
           == "/run/user/1000/hypr/abc/.socket2.sock");
    assert(od::hyprlandEventSocketPath("", "abc").empty());
    assert(od::hyprlandEventSocketPath("/run/user/1000", "").empty());
}

void debouncesBursts() {
    using Clock = od::Debouncer::Clock;
    const auto start = Clock::time_point{} + 10s;
    od::Debouncer debouncer(150ms);
    assert(!debouncer.pending());
    assert(!debouncer.remaining(start));
    assert(!debouncer.consumeDue(start));

    debouncer.trigger(start);
    debouncer.trigger(start + 100ms);  // a burst extends the deadline
    assert(debouncer.pending());
    assert(*debouncer.remaining(start + 100ms) == 150ms);
    assert(!debouncer.consumeDue(start + 200ms));
    assert(debouncer.consumeDue(start + 250ms));
    assert(!debouncer.pending());
    assert(!debouncer.consumeDue(start + 400ms));  // fires once per burst
}

void summarizesLatency() {
    assert(od::latencyPercentile({}, 50) == 0);
    std::vector<std::int64_t> samples;
    for (std::int64_t value = 100; value >= 1; --value) samples.push_back(value);
    const auto summary = od::summarizeLatency(samples);
    assert(summary.count == 100);
    assert(summary.p50 == 50);
    assert(summary.p95 == 95);
    assert(summary.max == 100);
    assert(od::latencyPercentile({7}, 95) == 7);

    od::LatencyWindow window(5'000);
    assert(!window.take(0));
    window.add(1'000, 10);
    window.add(2'000, -3);  // clock skew clamps to zero
    assert(!window.take(5'999));
    const auto taken = window.take(6'000);
    assert(taken && taken->count == 2 && taken->max == 10 && taken->p50 == 0);
    assert(!window.take(6'001));  // a new window just opened
}

}  // namespace

int main() {
    parsesEvents();
    buildsSocketPath();
    debouncesBursts();
    summarizesLatency();
    return 0;
}
