#pragma once

#include "LightCpp/Settings/SettingsTypes.hpp"

#include <QJsonObject>

namespace LightCpp::Settings {

// ---------------------------------------------------------------------------
// Serialisation — used by the default-config writer and the round-trip tests.
// ---------------------------------------------------------------------------

[[nodiscard]] QJsonObject toJson(const Toolchain& toolchain);
[[nodiscard]] QJsonObject toJson(const GlobalDefaults& defaults);
[[nodiscard]] QJsonObject toJson(const KitBuildConfiguration& config);
[[nodiscard]] QJsonObject toJson(const Kit& kit);
[[nodiscard]] QJsonObject toJson(const Preferences& prefs);

// ---------------------------------------------------------------------------
// Deserialisation — lenient by design.
//
// Every field is read only when it is present AND of the expected JSON type;
// anything else keeps the struct's default. A partly-broken config therefore
// degrades field by field instead of disabling the whole document, and
// unknown keys are ignored so newer configs still load in an older IDE.
// ---------------------------------------------------------------------------

void fromJson(const QJsonObject& object, Toolchain& out);
void fromJson(const QJsonObject& object, GlobalDefaults& out);
void fromJson(const QJsonObject& object, KitBuildConfiguration& out);
void fromJson(const QJsonObject& object, Kit& out);
void fromJson(const QJsonObject& object, Preferences& out);

}  // namespace LightCpp::Settings
