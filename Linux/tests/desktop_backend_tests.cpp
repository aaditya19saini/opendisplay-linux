#include "opendisplay/desktop_backend_factory.hpp"
#include "opendisplay/hyprland_output_controller.hpp"

#include <cassert>
#include <cmath>
#include <string>

namespace {

void detectsDesktopWithoutChangingLegacyFallback() {
    using od::CompositorKind;
    assert(od::detectCompositor(CompositorKind::Auto, "Hyprland", "", "")
           == CompositorKind::Hyprland);
    assert(od::detectCompositor(CompositorKind::Auto, "", "instance", "")
           == CompositorKind::Hyprland);
    assert(od::detectCompositor(CompositorKind::Auto, "KDE", "", "true")
           == CompositorKind::Kde);
    assert(od::detectCompositor(CompositorKind::Auto, "unknown", "", "")
           == CompositorKind::Kde);
    assert(od::detectCompositor(CompositorKind::Kde, "Hyprland", "instance", "")
           == CompositorKind::Kde);
}

void parsesHyprctlMonitorJson() {
    const std::string json = R"([{
        "id": 1, "name": "eDP-1", "width": 2560, "height": 1600,
        "refreshRate": 60.003,
        "x": -1707, "y": 0, "scale": 1.5, "transform": 0,
        "physicalWidth": 380, "physicalHeight": 240, "focused": true,
        "disabled": false,
        "availableModes": ["2560x1600@60.00100Hz", "1920x1200@60.00000Hz"]
    }])";
    const auto outputs = od::parseHyprlandOutputs(json);
    assert(outputs.size() == 1);
    assert(outputs.front().name == "eDP-1");
    assert(outputs.front().logicalGeometry.x == -1707);
    assert(outputs.front().logicalGeometry.width == 1707);
    assert(std::abs(outputs.front().refreshRate - 60.003) < 0.001);
    assert(outputs.front().physicalSize.has_value());
    assert(outputs.front().physicalSize->widthMm == 380);
    assert(outputs.front().focused);
    assert(outputs.front().modes.size() == 2);
    assert(std::abs(outputs.front().modes.front().refreshRate - 60.001) < 0.001);
}

void producesCurrentHyprlandMonitorExpression() {
    od::DisplayLayout layout;
    layout.resolution = {.width = 2420, .height = 1668};
    layout.logicalGeometry = {.x = 1707, .y = 233, .width = 1210, .height = 834};
    layout.scale = 2.0;
    layout.refreshRate = 60;
    const auto expression = od::hyprlandMonitorExpression("OpenDisplay-42", layout);
    assert(expression == "hl.monitor({ output = \"OpenDisplay-42\", mode = "
                         "\"2420x1668@60\", position = \"1707x233\", scale = 2 })");
}

void preservesReferenceTransformInMonitorExpression() {
    od::DisplayLayout layout;
    layout.resolution = {.width = 1600, .height = 2560};
    layout.logicalGeometry = {.x = 0, .y = 0, .width = 1000, .height = 1600};
    layout.scale = 1.6;
    layout.refreshRate = 60;
    const auto expression = od::hyprlandMonitorExpression("eDP-1", layout, 1);
    assert(expression.find("transform = 1") != std::string::npos);
}

void rejectsSemanticHyprctlErrorsWithZeroExitStatus() {
    assert(od::hyprlandCommandResponseAccepted(true, "ok"));
    assert(od::hyprlandCommandResponseAccepted(true, ""));
    assert(!od::hyprlandCommandResponseAccepted(
        true, "eval is only supported with the lua config manager"));
    assert(!od::hyprlandCommandResponseAccepted(false, "ok"));
}

void producesCurrentHyprlandFocusExpression() {
    assert(od::hyprlandFocusExpression("eDP-1")
           == "hl.dispatch(hl.dsp.focus({ monitor = \"eDP-1\" }))");
}

void producesPersistentRulesFile() {
    od::DisplayOutput reference;
    reference.name = "eDP-1";
    reference.resolution = {.width = 1920, .height = 1080};
    reference.scale = 1.25;
    reference.refreshRate = 144.003;
    reference.logicalGeometry = {.x = 0, .y = 0, .width = 1536, .height = 864};
    od::DisplayLayout layout;
    layout.resolution = {.width = 2224, .height = 1668};
    layout.scale = 2.0;
    layout.refreshRate = 60;
    layout.logicalGeometry = {.x = 1536, .y = 30, .width = 1112, .height = 834};
    const auto rules = od::hyprlandPersistentRules("OpenDisplay-uab", layout, reference);
    // Reference is pinned first, then the virtual output; one rule per line.
    const auto pin = rules.find("output = \"eDP-1\", mode = \"1920x1080@144\", position = \"0x0\", scale = 1.25");
    const auto virt = rules.find("output = \"OpenDisplay-uab\", mode = \"2224x1668@60\", position = \"1536x30\", scale = 2");
    assert(pin != std::string::npos && virt != std::string::npos && pin < virt);
    assert(rules.starts_with("--"));
    assert(od::hyprlandPersistentRulesPath().ends_with("/opendisplay/hyprland.lua"));
    assert(rules.find("workspace_rule") == std::string::npos);

    const auto withWorkspace = od::hyprlandPersistentRules("OpenDisplay-uab", layout, reference, "5");
    assert(withWorkspace.find("hl.workspace_rule({ workspace = \"5\", monitor = \"OpenDisplay-uab\", default = true })")
           > withWorkspace.find("OpenDisplay-uab\", mode"));
}

void mapsWorkspaceSettings() {
    assert(od::hyprlandWorkspaceId("", "iPad").empty());
    assert(od::hyprlandWorkspaceId("7", "iPad") == "7");
    assert(od::hyprlandWorkspaceId("dedicated", "iPad") == "name:iPad");
    assert(od::hyprlandWorkspaceId("dedicated", "Aaditya's iPad Pro") == "name:AadityasiPadPro");
    assert(od::hyprlandWorkspaceId("dedicated", "'\"") == "name:OpenDisplay");
    assert(od::hyprlandMoveWorkspaceExpression("name:iPad", "eDP-1")
           == "hl.dispatch(hl.dsp.workspace.move({ workspace = \"name:iPad\", monitor = \"eDP-1\" }))");
}

}  // namespace

int main() {
    detectsDesktopWithoutChangingLegacyFallback();
    parsesHyprctlMonitorJson();
    producesCurrentHyprlandMonitorExpression();
    preservesReferenceTransformInMonitorExpression();
    rejectsSemanticHyprctlErrorsWithZeroExitStatus();
    producesCurrentHyprlandFocusExpression();
    producesPersistentRulesFile();
    mapsWorkspaceSettings();
}
