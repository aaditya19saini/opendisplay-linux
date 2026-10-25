#pragma once

#include <string>
#include <string_view>

namespace od {

/// Rewrites one encoder packet into the access-unit shape the iOS receiver
/// expects: 4-byte start codes only, an access-unit delimiter first, no
/// filler data, and SPS/PPS in front of every IDR (taken from `parameterSets`,
/// which is refreshed whenever the packet carries its own).
struct AccessUnit {
    std::string bytes;
    bool keyframe = false;
};

AccessUnit normalizeAccessUnit(std::string_view packet, std::string& parameterSets);

}  // namespace od
