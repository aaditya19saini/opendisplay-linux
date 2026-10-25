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

private:
    std::string applyRule(const std::string& name, const DisplayLayout& ruleLayout,
                          int transform) const;
    std::string pinReference(const DisplayOutput& reference) const;
};

}  // namespace od
