#include "opendisplay/settings.hpp"

#include <QRegularExpression>

#include <cmath>
#include <stdexcept>
#include <string>

namespace od {
namespace {

QString text(const QVariantMap& values, const char* key, const char* fallback = "") {
    return values.value(QString::fromLatin1(key), QString::fromLatin1(fallback))
        .toString().trimmed();
}

TransportKind transport(const QString& value) {
    if (value == QStringLiteral("auto")) return TransportKind::Auto;
    if (value == QStringLiteral("wifi")) return TransportKind::Wifi;
    if (value == QStringLiteral("usb")) return TransportKind::Usb;
    throw std::runtime_error("Transport must be Auto, Wi-Fi, or USB.");
}

CaptureMode mode(const QString& value) {
    if (value == QStringLiteral("extend")) return CaptureMode::Extend;
    if (value == QStringLiteral("mirror")) return CaptureMode::Mirror;
    throw std::runtime_error("Display mode must be Extend or Mirror.");
}

EncoderKind encoder(const QString& value) {
    if (value == QStringLiteral("auto")) return EncoderKind::Auto;
    if (value == QStringLiteral("vaapi")) return EncoderKind::Vaapi;
    if (value == QStringLiteral("nvenc")) return EncoderKind::Nvenc;
    if (value == QStringLiteral("software")) return EncoderKind::Software;
    throw std::runtime_error("Encoder must be Auto, VA-API, NVENC, or Software.");
}

CompositorKind compositor(const QString& value) {
    if (value == QStringLiteral("auto")) return CompositorKind::Auto;
    if (value == QStringLiteral("kde")) return CompositorKind::Kde;
    if (value == QStringLiteral("hyprland")) return CompositorKind::Hyprland;
    throw std::runtime_error("Compositor must be Auto, KDE, or Hyprland.");
}

ExtendDirection extendDirection(const QString& value) {
    if (value == QStringLiteral("left")) return ExtendDirection::Left;
    if (value == QStringLiteral("right")) return ExtendDirection::Right;
    if (value == QStringLiteral("top")) return ExtendDirection::Top;
    if (value == QStringLiteral("bottom")) return ExtendDirection::Bottom;
    throw std::runtime_error("Extension direction is invalid.");
}

AlignDirection alignDirection(const QString& value) {
    if (value == QStringLiteral("left")) return AlignDirection::Left;
    if (value == QStringLiteral("right")) return AlignDirection::Right;
    if (value == QStringLiteral("top")) return AlignDirection::Top;
    if (value == QStringLiteral("bottom")) return AlignDirection::Bottom;
    if (value == QStringLiteral("center")) return AlignDirection::Center;
    throw std::runtime_error("Monitor alignment is invalid.");
}

PhysicalSize physicalSize(const QString& value, const char* label) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^([0-9]+(?:\.[0-9]+)?)[xX]([0-9]+(?:\.[0-9]+)?)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error(std::string(label) + " must use WIDTHxHEIGHT in millimetres.");
    }
    return PhysicalSize{.widthMm = match.captured(1).toDouble(),
                            .heightMm = match.captured(2).toDouble()};
}

Size pixelSize(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(R"(^(\d+)[xX](\d+)$)"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) {
        throw std::runtime_error("Virtual resolution must use WIDTHxHEIGHT.");
    }
    const Size result{.width = match.captured(1).toInt(),
                          .height = match.captured(2).toInt()};
    if (result.width < 2 || result.height < 2 || result.width > 65'535
        || result.height > 65'535) {
        throw std::runtime_error("Virtual resolution dimensions must be between 2 and 65535.");
    }
    return result;
}

}  // namespace

Options optionsFromSettings(const QVariantMap& values) {
    Options options;
    options.transport = transport(text(values, "transport", "auto"));
    options.mode = mode(text(values, "mode", "extend"));
    options.encoder = encoder(text(values, "encoder", "auto"));
    options.compositor = compositor(text(values, "compositor", "auto"));
    options.host = text(values, "host").toStdString();
    options.udid = text(values, "udid").toStdString();
    options.display.referenceMonitor = text(values, "referenceMonitor").toStdString();
    options.display.extendTo = extendDirection(text(values, "extendTo", "right"));
    options.display.alignTo = alignDirection(text(values, "alignTo", "bottom"));
    options.fps = values.value(QStringLiteral("fps"), 60).toInt();
    const double bitrateMbps = values.value(QStringLiteral("bitrateMbps"), 18.0).toDouble();
    options.bitrate = static_cast<int>(std::lround(bitrateMbps * 1'000'000.0));
    options.input = values.value(QStringLiteral("input"), true).toBool();
    options.verbose = values.value(QStringLiteral("verbose"), false).toBool();

    if (options.fps < 1 || options.fps > 240) {
        throw std::runtime_error("Frame rate must be between 1 and 240.");
    }
    if (bitrateMbps < 1.0 || bitrateMbps > 200.0) {
        throw std::runtime_error("Bitrate must be between 1 and 200 Mbps.");
    }
    const QString referenceSize = text(values, "referenceSizeMm");
    if (!referenceSize.isEmpty()) {
        options.display.referencePhysicalSize = physicalSize(referenceSize, "Reference size");
    }
    const QString receiverSize = text(values, "receiverSizeMm");
    if (!receiverSize.isEmpty()) {
        options.display.receiverPhysicalSize = physicalSize(receiverSize, "Receiver size");
    }
    const QString virtualResolution = text(values, "virtualResolution");
    if (!virtualResolution.isEmpty()) {
        options.display.virtualResolution = pixelSize(virtualResolution);
    }
    const QString displayScale = text(values, "displayScale");
    if (!displayScale.isEmpty()) {
        bool valid = false;
        const double value = displayScale.toDouble(&valid);
        if (!valid || value < 0.5 || value > 4.0) {
            throw std::runtime_error("Display scale must be between 0.5 and 4.0.");
        }
        options.display.virtualScale = value;
    }
    return options;
}


QVariantMap defaultSettings() {
    return {
        {QStringLiteral("transport"), QStringLiteral("auto")},
        {QStringLiteral("encoder"), QStringLiteral("auto")},
        {QStringLiteral("compositor"), QStringLiteral("auto")},
        {QStringLiteral("extendTo"), QStringLiteral("right")},
        {QStringLiteral("alignTo"), QStringLiteral("bottom")},
        {QStringLiteral("fps"), 60},
        {QStringLiteral("bitrateMbps"), 18.0},
        {QStringLiteral("input"), true},
    };
}

QString captureModeName(const CaptureMode mode) {
    return mode == CaptureMode::Extend ? QStringLiteral("extend") : QStringLiteral("mirror");
}

std::optional<CaptureMode> parseCaptureMode(const QString& value) {
    if (value == QStringLiteral("extend")) return CaptureMode::Extend;
    if (value == QStringLiteral("mirror")) return CaptureMode::Mirror;
    return std::nullopt;
}

}  // namespace od
