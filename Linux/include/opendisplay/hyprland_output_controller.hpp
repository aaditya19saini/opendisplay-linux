#pragma once

#include "opendisplay/display_layout.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace od {

std::vector<DisplayOutput> parseHyprlandOutputs(std::string_view json);
bool hyprlandCommandResponseAccepted(bool processSucceeded, std::string_view output);
std::string hyprlandMonitorExpression(const std::string& outputName,
                                      const DisplayLayout& layout,
                                      int transform = -1);
std::string hyprlandFocusExpression(const std::string& outputName);

/// Lua file the user's hyprland.lua can load so OpenDisplay's monitor rules
/// survive config reloads (no mode flip, no encoder restart):
///   pcall(dofile, "<state>/opendisplay/hyprland.lua")
std::string hyprlandPersistentRulesPath();
/// Contents of that file for an active session.
std::string hyprlandPersistentRules(const std::string& outputName, const DisplayLayout& layout,
                                    const DisplayOutput& reference);
/// True when the user's Hyprland config loads the persistent rules file.
bool hyprlandConfigLoadsPersistentRules();

class HyprlandOutputController {
public:
    std::vector<DisplayOutput> outputs() const;
    DisplayOutput create(const std::string& outputName,
                         const DisplayLayout& layout,
                         const DisplayOutput& detectedReference) const;
    /// Re-installs the reference pin and the virtual-output rule, e.g. after a
    /// Hyprland config reload dropped the runtime rules.
    void reapply(const std::string& outputName, const DisplayLayout& layout,
                 const DisplayOutput& reference) const;
    void focus(const std::string& outputName) const;
    void remove(const std::string& outputName) const;
    void reload() const;
    /// Writes the persistent rules file for the active session; clear removes it.
    void writePersistentRules(const std::string& outputName, const DisplayLayout& layout,
                              const DisplayOutput& reference) const;
    void clearPersistentRules() const;

private:
    std::string applyRule(const std::string& name, const DisplayLayout& ruleLayout,
                          int transform) const;
    std::string pinReference(const DisplayOutput& reference) const;
};

}  // namespace od
