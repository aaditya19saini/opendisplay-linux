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
/// Contents of that file for an active session; `workspaceId` (optional)
/// adds a rule binding that workspace to the virtual output.
std::string hyprlandPersistentRules(const std::string& outputName, const DisplayLayout& layout,
                                    const DisplayOutput& reference,
                                    const std::string& workspaceId = {});

/// Hyprland workspace for a DisplayOptions::workspace setting: "" for the
/// compositor default, "name:<receiver>" for "dedicated", else the number.
std::string hyprlandWorkspaceId(const std::string& setting, const std::string& receiverName);
std::string hyprlandWorkspaceRuleExpression(const std::string& workspaceId,
                                            const std::string& outputName);
std::string hyprlandMoveWorkspaceExpression(const std::string& workspaceId,
                                            const std::string& monitorName);
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
                              const DisplayOutput& reference,
                              const std::string& workspaceId = {}) const;
    /// Binds the workspace to the output (and makes it the output's default),
    /// then moves it there, creating it when it doesn't exist yet. Best
    /// effort: failures are logged, not thrown.
    void placeWorkspace(const std::string& workspaceId, const std::string& outputName) const;
    /// Moves a workspace to a monitor; false when Hyprland has no such workspace.
    bool moveWorkspace(const std::string& workspaceId, const std::string& monitorName) const;
    void clearPersistentRules() const;

private:
    std::string applyRule(const std::string& name, const DisplayLayout& ruleLayout,
                          int transform) const;
    std::string pinReference(const DisplayOutput& reference) const;
};

}  // namespace od
