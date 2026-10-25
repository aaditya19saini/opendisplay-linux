#include "opendisplay/annexb.hpp"

#include <vector>

namespace od {
namespace {

struct Nal {
    std::string_view payload;  // without start code
    int type = 0;
};

std::vector<Nal> splitNals(const std::string_view bytes) {
    std::vector<Nal> nals;
    std::size_t index = 0;
    std::size_t start = std::string_view::npos;
    const auto isStartCode = [&](std::size_t at, std::size_t& length) {
        if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
            if (bytes[at + 2] == 1) { length = 3; return true; }
            if (at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) {
                length = 4;
                return true;
            }
        }
        return false;
    };
    while (index < bytes.size()) {
        std::size_t length = 0;
        if (isStartCode(index, length)) {
            if (start != std::string_view::npos) {
                auto payload = bytes.substr(start, index - start);
                while (!payload.empty() && payload.back() == 0) payload.remove_suffix(1);
                if (!payload.empty()) nals.push_back({payload, payload[0] & 0x1f});
            }
            index += length;
            start = index;
        } else {
            ++index;
        }
    }
    if (start != std::string_view::npos && start < bytes.size()) {
        const auto payload = bytes.substr(start);
        if (!payload.empty()) nals.push_back({payload, payload[0] & 0x1f});
    }
    return nals;
}

void append(std::string& out, const std::string_view payload) {
    out.append("\0\0\0\1", 4);
    out.append(payload);
}

}  // namespace

AccessUnit normalizeAccessUnit(const std::string_view packet, std::string& parameterSets) {
    const auto nals = splitNals(packet);
    AccessUnit unit;
    bool hasParameterSets = false;
    std::string fresh;
    for (const auto& nal : nals) {
        if (nal.type == 5) unit.keyframe = true;
        if (nal.type == 7 || nal.type == 8) {
            hasParameterSets = true;
            append(fresh, nal.payload);
        }
    }
    if (hasParameterSets) parameterSets = fresh;

    append(unit.bytes, std::string_view("\x09\xf0", 2));  // AUD, primary_pic_type any
    if (unit.keyframe && !hasParameterSets) unit.bytes += parameterSets;
    for (const auto& nal : nals) {
        if (nal.type == 9 || nal.type == 12) continue;  // own AUD; drop filler
        append(unit.bytes, nal.payload);
    }
    return unit;
}

}  // namespace od
