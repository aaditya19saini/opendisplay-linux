#pragma once

#include "opendisplay/types.hpp"

#include <QString>
#include <QVariantMap>

#include <optional>

namespace od {

/// Validates a settings map (as stored by the daemon and edited by clients)
/// and converts it to session options. Throws std::runtime_error with a
/// user-facing message on invalid values.
Options optionsFromSettings(const QVariantMap& values);
QVariantMap defaultSettings();

QString captureModeName(CaptureMode mode);
std::optional<CaptureMode> parseCaptureMode(const QString& value);

}  // namespace od
