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

int main() {
    buildsLetterboxFilter();
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    od::FfmpegEncoder encoder;
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
    return 0;
}
