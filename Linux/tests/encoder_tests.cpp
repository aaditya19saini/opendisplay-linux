#include "opendisplay/annexb.hpp"
#include "opendisplay/av_encoder.hpp"
#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/wire.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool hasOnlyFourByteStartCodes(const std::string_view bytes) {
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1
            && (index == 0 || bytes[index - 1] != 0)) {
            return false;
        }
    }
    return true;
}

bool hasNalType(const std::string_view bytes, const unsigned char type) {
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 0
            && bytes[index + 3] == 1
            && (static_cast<unsigned char>(bytes[index + 4]) & 0x1fU) == type) {
            return true;
        }
    }
    return false;
}

}  // namespace

void buildsLetterboxFilter() {
    const auto software = od::encoderVideoFilter(od::EncoderKind::Software, 2224, 1668);
    assert(software.find("scale=2224:1668:force_original_aspect_ratio=decrease")
           != std::string::npos);
    assert(software.find("pad=2224:1668:(ow-iw)/2:(oh-ih)/2") != std::string::npos);
    assert(software.find("hwupload") == std::string::npos);
    assert(od::encoderVideoFilter(od::EncoderKind::Nvenc, 64, 64).find("hwupload")
           == std::string::npos);
    const auto vaapi = od::encoderVideoFilter(od::EncoderKind::Vaapi, 64, 64);
    // Pad before the NV12 conversion and upload.
    assert(vaapi.find("pad=") < vaapi.find("format=nv12,hwupload"));
}

void encodesLetterboxedFrames(od::VideoEncoder& encoder) {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    encoder.start(od::EncoderConfig{
        .kind = od::EncoderKind::Software,
        .outputWidth = 64,
        .outputHeight = 64,
        .fps = 30,
        .bitrate = 300'000,
    }, [&](od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            output.push_back(std::move(frame));
        }
        condition.notify_all();
    });

    for (int index = 0; index < 8; ++index) {
        od::CapturedFrame frame;
        // A 2:1 frame into a 1:1 output exercises the letterbox path.
        frame.format = od::VideoFormat{.width = 64, .height = 32, .stride = 256,
                                       .fps = 30};
        frame.capturedAtMs = 1000 + index;
        frame.sequence = static_cast<unsigned>(index);
        frame.bytes.assign(64 * 32 * 4, static_cast<char>(index * 20));
        encoder.submit(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(5), [&] { return !output.empty(); });
    }
    encoder.stop();
    assert(!output.empty());
    assert(od::wire::containsAnnexBStartCode(output.front().annexB));
    assert(output.front().keyframe);
    assert(hasNalType(output.front().annexB, 7));  // SPS
    assert(hasNalType(output.front().annexB, 8));  // PPS
    assert(hasNalType(output.front().annexB, 5));  // IDR slice
    for (const auto& encoded : output) {
        assert(hasOnlyFourByteStartCodes(encoded.annexB));
    }
}

void normalizesAccessUnits() {
    std::string params;
    // 3-byte start codes, a filler NAL, no AUD: SPS, PPS, IDR, filler.
    const std::string packet = std::string("\0\0\1\x67\x42", 5) + std::string("\0\0\1\x68\xce", 5)
        + std::string("\0\0\1\x65\x88", 5) + std::string("\0\0\1\x0c\xff", 5);
    const auto unit = od::normalizeAccessUnit(packet, params);
    assert(unit.keyframe);
    assert(unit.bytes.starts_with(std::string("\0\0\0\1\x09\xf0", 6)));
    assert(hasOnlyFourByteStartCodes(unit.bytes));
    assert(hasNalType(unit.bytes, 7) && hasNalType(unit.bytes, 8) && hasNalType(unit.bytes, 5));
    assert(!hasNalType(unit.bytes, 12));
    assert(params == std::string("\0\0\0\1\x67\x42\0\0\0\1\x68\xce", 12));

    // A later IDR without parameter sets gets the cached ones prepended.
    const auto idr = od::normalizeAccessUnit(std::string("\0\0\0\1\x65\x99", 6), params);
    assert(idr.keyframe && hasNalType(idr.bytes, 7) && hasNalType(idr.bytes, 8));
    assert(idr.bytes.find(std::string("\x09\xf0", 2)) == 4);  // AUD still first

    // A P-frame stays small: AUD + slice, existing AUD not duplicated.
    const auto p = od::normalizeAccessUnit(std::string("\0\0\0\1\x09\x10\0\0\0\1\x41\x9a", 12), params);
    assert(!p.keyframe && !hasNalType(p.bytes, 7));
    assert(p.bytes == std::string("\0\0\0\1\x09\xf0\0\0\0\1\x41\x9a", 12));
}

void computesLetterbox() {
    const auto wide = od::letterboxRect(1920, 1080, 2224, 1668);  // 16:9 into 4:3
    assert(wide.width == 2224 && wide.x == 0);
    assert(wide.height == 1250 && wide.y == 208);
    const auto tall = od::letterboxRect(1000, 2000, 2000, 2000);
    assert(tall.height == 2000 && tall.width == 1000 && tall.x == 500 && tall.y == 0);
    const auto same = od::letterboxRect(2224, 1668, 2224, 1668);
    assert(same.x == 0 && same.y == 0 && same.width == 2224 && same.height == 1668);
    for (const auto r : {wide, tall, same}) {
        assert(r.x % 2 == 0 && r.y % 2 == 0 && r.width % 2 == 0 && r.height % 2 == 0);
    }
}

int main() {
    buildsLetterboxFilter();
    normalizesAccessUnits();
    computesLetterbox();
    {
        od::FfmpegEncoder subprocess;
        encodesLetterboxedFrames(subprocess);
    }
    {
        od::AvEncoder inProcess;
        encodesLetterboxedFrames(inProcess);
        assert(inProcess.selectedEncoder() == "libx264");
    }
    return 0;
}
