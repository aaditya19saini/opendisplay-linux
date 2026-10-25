#include "opendisplay/av_encoder.hpp"

#include "opendisplay/annexb.hpp"
#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace od {
namespace {

using Clock = std::chrono::steady_clock;

std::string avError(const int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

const char* codecName(const EncoderKind kind) {
    switch (kind) {
    case EncoderKind::Vaapi: return "h264_vaapi";
    case EncoderKind::Nvenc: return "h264_nvenc";
    case EncoderKind::Software:
    case EncoderKind::Auto: break;
    }
    return "libx264";
}

AVPixelFormat capturedFormat(const VideoFormat::PixelFormat format) {
    switch (format) {
    case VideoFormat::PixelFormat::Bgra: return AV_PIX_FMT_BGRA;
    case VideoFormat::PixelFormat::Bgrx: return AV_PIX_FMT_BGR0;
    case VideoFormat::PixelFormat::Rgba: return AV_PIX_FMT_RGBA;
    case VideoFormat::PixelFormat::Rgbx: return AV_PIX_FMT_RGB0;
    }
    return AV_PIX_FMT_BGR0;
}

bool nvidiaDriver(const std::string& renderNode) {
    std::error_code error;
    const auto name = std::filesystem::path(renderNode).filename().string();
    const auto driver = std::filesystem::read_symlink(
        "/sys/class/drm/" + name + "/device/driver", error);
    return !error && driver.filename().string().starts_with("nvidia");
}

}  // namespace

std::vector<std::string> vaapiEncodeCandidates() {
    std::vector<std::string> nodes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("renderD") && !nvidiaDriver(entry.path().string())) {
            nodes.push_back(entry.path().string());
        }
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

FitRect letterboxRect(const int sourceWidth, const int sourceHeight, const int targetWidth,
                      const int targetHeight) {
    if (sourceWidth <= 0 || sourceHeight <= 0) return {0, 0, targetWidth, targetHeight};
    const double scale = std::min(static_cast<double>(targetWidth) / sourceWidth,
                                  static_cast<double>(targetHeight) / sourceHeight);
    int width = std::max(2, static_cast<int>(sourceWidth * scale) & ~1);
    int height = std::max(2, static_cast<int>(sourceHeight * scale) & ~1);
    width = std::min(width, targetWidth & ~1);
    height = std::min(height, targetHeight & ~1);
    return {((targetWidth - width) / 2) & ~1, ((targetHeight - height) / 2) & ~1, width, height};
}

/// Everything tied to one opened encoder instance and input format.
struct AvCodecState {
    EncoderKind kind = EncoderKind::Software;
    AVCodecContext* context = nullptr;
    AVBufferRef* hwDevice = nullptr;
    SwsContext* scaler = nullptr;
    AVFrame* converted = nullptr;  // CPU frame in the encoder's (or upload) format
    VideoFormat input;
    AVPixelFormat inputFormat = AV_PIX_FMT_NONE;
    AVPixelFormat swFormat = AV_PIX_FMT_NONE;
    FitRect fit;
    bool passthrough = false;  // hand the captured buffer to the encoder as-is
    std::int64_t lastPts = -1;
    std::int64_t firstCaptureMs = -1;
    std::string parameterSets;
    AVPacket* packet = nullptr;

    ~AvCodecState() {
        if (context) avcodec_free_context(&context);
        if (hwDevice) av_buffer_unref(&hwDevice);
        if (scaler) sws_freeContext(scaler);
        if (converted) av_frame_free(&converted);
        if (packet) av_packet_free(&packet);
    }
};

namespace {

std::unique_ptr<AvCodecState> openCodec(const EncoderChoice& choice,
                                            const VideoFormat& input, const int outWidth,
                                            const int outHeight, const int fps,
                                            const int bitrate) {
    const AVCodec* encoder = avcodec_find_encoder_by_name(codecName(choice.kind));
    if (encoder == nullptr) {
        throw std::runtime_error(std::string("FFmpeg has no ") + codecName(choice.kind));
    }
    auto codec = std::make_unique<AvCodecState>();
    codec->kind = choice.kind;
    codec->input = input;
    codec->inputFormat = capturedFormat(input.pixelFormat);
    codec->fit = letterboxRect(input.width, input.height, outWidth, outHeight);

    auto* context = avcodec_alloc_context3(encoder);
    codec->context = context;
    context->width = outWidth;
    context->height = outHeight;
    context->time_base = AVRational{1, 1000};  // PTS are capture milliseconds
    context->framerate = AVRational{std::max(1, fps), 1};
    context->gop_size = std::max(1, fps) * 60;
    context->max_b_frames = 0;
    context->bit_rate = bitrate;
    context->rc_max_rate = bitrate;
    context->rc_buffer_size = std::max(1, bitrate / std::max(1, fps) * 2);
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->profile = AV_PROFILE_H264_HIGH;
    // SPS/PPS stay in-band (no AV_CODEC_FLAG_GLOBAL_HEADER) so every IDR is
    // self-contained for the receiver.

    if (choice.kind == EncoderKind::Nvenc) {
        // NVENC converts RGB to YUV on the GPU; give it the capture format.
        codec->swFormat = codec->inputFormat;
        context->pix_fmt = codec->swFormat;
        av_opt_set(context->priv_data, "preset", "p1", 0);
        av_opt_set(context->priv_data, "tune", "ull", 0);
        av_opt_set(context->priv_data, "rc", "cbr", 0);
        av_opt_set_int(context->priv_data, "zerolatency", 1, 0);
        av_opt_set_int(context->priv_data, "delay", 0, 0);
        av_opt_set_int(context->priv_data, "forced-idr", 1, 0);
        av_opt_set_int(context->priv_data, "rc-lookahead", 0, 0);
    } else if (choice.kind == EncoderKind::Vaapi) {
        int result = av_hwdevice_ctx_create(&codec->hwDevice, AV_HWDEVICE_TYPE_VAAPI,
                                            choice.device.c_str(), nullptr, 0);
        if (result < 0) {
            throw std::runtime_error("cannot open VA-API device " + choice.device + ": "
                                     + avError(result));
        }
        AVBufferRef* frames = av_hwframe_ctx_alloc(codec->hwDevice);
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
        framesContext->format = AV_PIX_FMT_VAAPI;
        framesContext->sw_format = AV_PIX_FMT_NV12;
        framesContext->width = outWidth;
        framesContext->height = outHeight;
        framesContext->initial_pool_size = 4;
        result = av_hwframe_ctx_init(frames);
        if (result < 0) {
            av_buffer_unref(&frames);
            throw std::runtime_error("cannot create VA-API frames: " + avError(result));
        }
        context->hw_frames_ctx = frames;
        context->pix_fmt = AV_PIX_FMT_VAAPI;
        codec->swFormat = AV_PIX_FMT_NV12;
        av_opt_set_int(context->priv_data, "async_depth", 1, 0);
        av_opt_set(context->priv_data, "rc_mode", "CBR", 0);
    } else {
        codec->swFormat = AV_PIX_FMT_YUV420P;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        av_opt_set(context->priv_data, "preset", "ultrafast", 0);
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
        av_opt_set_int(context->priv_data, "forced-idr", 1, 0);
    }

    int result = avcodec_open2(context, encoder, nullptr);
    if (result < 0 && choice.kind != EncoderKind::Software) {
        // Some VA-API drivers lack High; Main still decodes everywhere.
        context->profile = AV_PROFILE_H264_MAIN;
        result = avcodec_open2(context, encoder, nullptr);
    }
    if (result < 0) {
        throw std::runtime_error(std::string("cannot open ") + codecName(choice.kind) + ": "
                                 + avError(result));
    }

    codec->passthrough = codec->swFormat == codec->inputFormat && input.width == outWidth
        && input.height == outHeight;
    if (!codec->passthrough) {
        codec->converted = av_frame_alloc();
        codec->converted->format = codec->swFormat;
        codec->converted->width = outWidth;
        codec->converted->height = outHeight;
        if (av_frame_get_buffer(codec->converted, 0) < 0) {
            throw std::runtime_error("cannot allocate encoder frame");
        }
        const ptrdiff_t linesizes[4] = {codec->converted->linesize[0], codec->converted->linesize[1],
                                        codec->converted->linesize[2], codec->converted->linesize[3]};
        av_image_fill_black(codec->converted->data, linesizes, codec->swFormat, AVCOL_RANGE_MPEG,
                            outWidth, outHeight);
        codec->scaler = sws_getContext(input.width, input.height, codec->inputFormat,
                                       codec->fit.width, codec->fit.height, codec->swFormat,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (codec->scaler == nullptr) throw std::runtime_error("cannot create scaler");
    }
    codec->packet = av_packet_alloc();
    return codec;
}

/// Pointers into `frame` at the letterbox offset, plane by plane.
void offsetPlanes(const AVFrame* frame, const FitRect& fit, std::uint8_t* planes[4]) {
    const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    for (int plane = 0; plane < 4; ++plane) {
        planes[plane] = frame->data[plane];
        if (planes[plane] == nullptr) continue;
        const bool chroma = plane == 1 || plane == 2;
        const int y = chroma ? fit.y >> descriptor->log2_chroma_h : fit.y;
        int xBytes = 0;
        if (descriptor->flags & AV_PIX_FMT_FLAG_RGB) {
            xBytes = fit.x * descriptor->comp[0].step;
        } else if (chroma && descriptor->nb_components >= 3
                   && descriptor->comp[1].plane == descriptor->comp[2].plane) {
            xBytes = (fit.x >> descriptor->log2_chroma_w) * descriptor->comp[1].step;  // NV12 UV
        } else {
            xBytes = chroma ? fit.x >> descriptor->log2_chroma_w : fit.x;
        }
        planes[plane] += static_cast<std::ptrdiff_t>(y) * frame->linesize[plane] + xBytes;
    }
}

/// Encodes one frame and returns the access units it produced.
std::vector<AccessUnit> encodeFrame(AvCodecState& codec, CapturedFrame& captured,
                                    const bool forceKeyframe) {
    AVFrame* frame = av_frame_alloc();
    if (codec.passthrough) {
        // Hand the capture buffer over without copying; the buffer owns it.
        auto* owned = new std::string(std::move(captured.bytes));
        AVBufferRef* buffer = av_buffer_create(
            reinterpret_cast<std::uint8_t*>(owned->data()), owned->size(),
            [](void* opaque, std::uint8_t*) { delete static_cast<std::string*>(opaque); },
            owned, 0);
        frame->buf[0] = buffer;
        frame->data[0] = buffer->data;
        frame->linesize[0] = captured.format.stride;
        frame->format = codec.swFormat;
        frame->width = captured.format.width;
        frame->height = captured.format.height;
    } else {
        // The encoder may still reference last frame's buffer; copy-on-write.
        if (!av_frame_is_writable(codec.converted)) av_frame_make_writable(codec.converted);
        const std::uint8_t* source[4] = {
            reinterpret_cast<const std::uint8_t*>(captured.bytes.data()), nullptr, nullptr, nullptr};
        const int sourceStride[4] = {captured.format.stride, 0, 0, 0};
        std::uint8_t* target[4]{};
        offsetPlanes(codec.converted, codec.fit, target);
        sws_scale(codec.scaler, source, sourceStride, 0, captured.format.height, target,
                  codec.converted->linesize);
        if (codec.kind == EncoderKind::Vaapi) {
            frame->format = AV_PIX_FMT_VAAPI;
            int result = av_hwframe_get_buffer(codec.context->hw_frames_ctx, frame, 0);
            if (result >= 0) result = av_hwframe_transfer_data(frame, codec.converted, 0);
            if (result < 0) {
                av_frame_free(&frame);
                throw std::runtime_error("VA-API upload failed: " + avError(result));
            }
        } else {
            av_frame_ref(frame, codec.converted);
        }
    }
    if (codec.firstCaptureMs < 0) codec.firstCaptureMs = captured.capturedAtMs;
    auto pts = captured.capturedAtMs - codec.firstCaptureMs;
    if (pts <= codec.lastPts) pts = codec.lastPts + 1;
    codec.lastPts = pts;
    frame->pts = pts;
    if (forceKeyframe) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
    }

    int result = avcodec_send_frame(codec.context, frame);
    av_frame_free(&frame);
    if (result < 0) throw std::runtime_error("encoder rejected a frame: " + avError(result));

    std::vector<AccessUnit> units;
    while ((result = avcodec_receive_packet(codec.context, codec.packet)) >= 0) {
        units.push_back(normalizeAccessUnit(
            std::string_view(reinterpret_cast<const char*>(codec.packet->data),
                             static_cast<std::size_t>(codec.packet->size)),
            codec.parameterSets));
        av_packet_unref(codec.packet);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        throw std::runtime_error("encoder failed: " + avError(result));
    }
    return units;
}

QString cachePath() {
    const auto configured = qEnvironmentVariable("XDG_CACHE_HOME");
    return (configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured)
        + QStringLiteral("/opendisplay/encoders.json");
}

/// Median send-to-packet latency (ms) over synthetic moving frames, or
/// nullopt when this encoder cannot be used here.
std::optional<double> benchmark(const EncoderChoice& choice, const int width, const int height,
                                const int fps, const int bitrate) {
    try {
        VideoFormat format{.width = width, .height = height, .stride = width * 4, .fps = fps,
                           .pixelFormat = VideoFormat::PixelFormat::Bgrx};
        auto codec = openCodec(choice, format, width, height, fps, bitrate);
        std::vector<double> samples;
        for (int index = 0; index < 24; ++index) {
            CapturedFrame frame;
            frame.format = format;
            frame.capturedAtMs = index * 16;
            frame.bytes.assign(static_cast<std::size_t>(width) * height * 4, '\0');
            // A moving bar and noise-ish fill keep the encoder honest.
            for (int y = 0; y < height; y += 2) {
                auto* row = frame.bytes.data() + static_cast<std::size_t>(y) * width * 4;
                for (int x = (index * 37) % width, end = std::min(width, x + width / 4); x < end; ++x) {
                    row[x * 4] = static_cast<char>(x ^ y ^ index);
                    row[x * 4 + 1] = static_cast<char>(x + index);
                }
            }
            const auto started = Clock::now();
            const auto units = encodeFrame(*codec, frame, index == 0);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started);
            if (index >= 4 && !units.empty()) samples.push_back(elapsed.count());
        }
        if (samples.size() < 10) return std::nullopt;
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    } catch (const std::exception& error) {
        debug(std::string("Encoder benchmark skipped ") + codecName(choice.kind)
              + (choice.device.empty() ? "" : " on " + choice.device) + ": " + error.what());
        return std::nullopt;
    }
}

}  // namespace

EncoderChoice chooseAutoEncoder(const int width, const int height, const int fps,
                                const int bitrate) {
    const auto key = QStringLiteral("%1x%2@%3").arg(width).arg(height).arg(fps);
    QJsonObject cache;
    {
        QFile file(cachePath());
        if (file.open(QIODevice::ReadOnly)) cache = QJsonDocument::fromJson(file.readAll()).object();
    }
    const auto cached = cache.value(key).toObject();
    if (!cached.isEmpty()) {
        const auto name = cached.value(QStringLiteral("encoder")).toString();
        EncoderChoice choice{.kind = name == QStringLiteral("h264_nvenc") ? EncoderKind::Nvenc
                                     : name == QStringLiteral("h264_vaapi") ? EncoderKind::Vaapi
                                                                           : EncoderKind::Software,
                             .device = cached.value(QStringLiteral("device")).toString().toStdString()};
        if (choice.kind != EncoderKind::Vaapi || std::filesystem::exists(choice.device)) return choice;
    }

    std::vector<EncoderChoice> candidates;
    if (std::filesystem::exists("/dev/nvidiactl")) candidates.push_back({EncoderKind::Nvenc, {}});
    for (const auto& node : vaapiEncodeCandidates()) candidates.push_back({EncoderKind::Vaapi, node});
    candidates.push_back({EncoderKind::Software, {}});

    log("Benchmarking H.264 encoders at " + key.toStdString() + " (first run only)…");
    std::optional<EncoderChoice> best;
    double bestMs = 0;
    QJsonObject results;
    for (const auto& candidate : candidates) {
        const auto median = benchmark(candidate, width, height, fps, bitrate);
        const auto label = std::string(codecName(candidate.kind))
            + (candidate.device.empty() ? "" : " (" + candidate.device + ")");
        if (!median) continue;
        log("  " + label + ": " + std::to_string(*median).substr(0, 5) + " ms per frame");
        results.insert(QString::fromStdString(label), *median);
        if (!best || *median < bestMs) {
            best = candidate;
            bestMs = *median;
        }
    }
    if (!best) throw std::runtime_error("no usable H.264 encoder");
    cache.insert(key, QJsonObject{{QStringLiteral("encoder"), codecName(best->kind)},
                                  {QStringLiteral("device"), QString::fromStdString(best->device)},
                                  {QStringLiteral("medianMs"), bestMs},
                                  {QStringLiteral("results"), results}});
    QDir().mkpath(QFileInfo(cachePath()).absolutePath());
    QSaveFile file(cachePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(cache).toJson());
        file.commit();
    }
    return *best;
}

std::unique_ptr<VideoEncoder> makeVideoEncoder() {
    const char* backend = std::getenv("OPENDISPLAY_ENCODER_BACKEND");
    if (backend != nullptr && std::string_view(backend) == "cli") {
        return std::make_unique<FfmpegEncoder>();
    }
    return std::make_unique<AvEncoder>();
}

AvEncoder::AvEncoder() = default;

AvEncoder::~AvEncoder() { stop(); }

void AvEncoder::start(EncoderConfig config, FrameCallback callback) {
    stop();
    av_log_set_level(verboseLogging ? AV_LOG_INFO : AV_LOG_ERROR);
    config_ = std::move(config);
    callback_ = std::move(callback);
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        selectedName_.clear();
    }
    worker_ = std::thread(&AvEncoder::run, this);
}

void AvEncoder::submit(CapturedFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        pending_ = std::move(frame);
    }
    condition_.notify_one();
}

void AvEncoder::requestKeyframe() { keyframeRequested_.store(true); }

void AvEncoder::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        pending_.reset();
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
    codec_.reset();
}

std::string AvEncoder::selectedEncoder() const {
    std::lock_guard lock(mutex_);
    if (!selectedName_.empty()) return selectedName_;
    switch (config_.kind) {
    case EncoderKind::Nvenc: return "h264_nvenc";
    case EncoderKind::Vaapi: return "h264_vaapi";
    case EncoderKind::Software: return "libx264";
    case EncoderKind::Auto: break;
    }
    return "auto";
}

void AvEncoder::run() {
    std::optional<EncoderChoice> choice;
    try {
        for (;;) {
            CapturedFrame frame;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] { return !running_ || pending_.has_value(); });
                if (!running_) break;
                frame = std::move(*pending_);
                pending_.reset();
            }
            if (!choice) {
                if (config_.kind == EncoderKind::Auto) {
                    choice = chooseAutoEncoder(config_.outputWidth, config_.outputHeight,
                                               config_.fps, config_.bitrate);
                } else {
                    choice = EncoderChoice{.kind = config_.kind, .device = config_.vaapiDevice};
                    if (choice->kind == EncoderKind::Vaapi && choice->device.empty()) {
                        const auto nodes = vaapiEncodeCandidates();
                        if (nodes.empty()) throw std::runtime_error("no VA-API render node");
                        choice->device = nodes.front();
                    }
                }
                std::lock_guard lock(mutex_);
                selectedName_ = std::string(codecName(choice->kind))
                    + (choice->device.empty() ? "" : " (" + choice->device + ")");
            }
            const bool formatChanged = codec_
                && (frame.format.width != codec_->input.width
                    || frame.format.height != codec_->input.height
                    || frame.format.stride != codec_->input.stride
                    || frame.format.pixelFormat != codec_->input.pixelFormat);
            bool forceKeyframe = keyframeRequested_.exchange(false);
            if (!codec_ || formatChanged) {
                codec_ = openCodec(*choice, frame.format, config_.outputWidth,
                                   config_.outputHeight, config_.fps, config_.bitrate);
                log("Encoding in-process with " + selectedName_ + ' '
                    + std::to_string(config_.outputWidth) + 'x'
                    + std::to_string(config_.outputHeight)
                    + (codec_->passthrough ? " (zero-copy input)" : ""));
                forceKeyframe = true;
            }
            const auto capturedAt = frame.capturedAtMs;
            for (auto& unit : encodeFrame(*codec_, frame, forceKeyframe)) {
                if (callback_) {
                    callback_(EncodedFrame{.capturedAtMs = capturedAt, .keyframe = unit.keyframe,
                                           .annexB = std::move(unit.bytes)});
                }
            }
        }
    } catch (const std::exception& error) {
        log(std::string("Encoder error: ") + error.what());
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    codec_.reset();
}

}  // namespace od
