#include "Theme.h"

#include <imgui.h>

namespace reportpanel {
namespace {

ImVec4 Blend(ImVec4 base, ImVec4 tint, float amount) {
    return ImVec4(base.x + (tint.x - base.x) * amount,
                  base.y + (tint.y - base.y) * amount,
                  base.z + (tint.z - base.z) * amount,
                  base.w + (tint.w - base.w) * amount);
}

} // namespace

void ApplyDarkTheme(const AppSettings& settings) {
    auto& style = ImGui::GetStyle();
    const ImVec4 accent(settings.accentR, settings.accentG, settings.accentB, 1.0f);
    style.WindowRounding = 10.0f;
    style.ChildRounding = 9.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.6f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 13.0f);
    style.FramePadding = ImVec2(11.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.Colors[ImGuiCol_WindowBg] = Blend(ImVec4(0.055f, 0.047f, 0.078f, 1.0f), accent, 0.08f);
    style.Colors[ImGuiCol_ChildBg] = Blend(ImVec4(0.075f, 0.065f, 0.104f, 1.0f), accent, 0.12f);
    style.Colors[ImGuiCol_PopupBg] = Blend(ImVec4(0.065f, 0.055f, 0.09f, 0.98f), accent, 0.12f);
    style.Colors[ImGuiCol_FrameBg] = Blend(ImVec4(0.12f, 0.10f, 0.17f, 1.0f), accent, 0.20f);
    style.Colors[ImGuiCol_FrameBgHovered] = Blend(ImVec4(0.20f, 0.15f, 0.27f, 1.0f), accent, 0.28f);
    style.Colors[ImGuiCol_FrameBgActive] = Blend(ImVec4(0.23f, 0.16f, 0.33f, 1.0f), accent, 0.38f);
    style.Colors[ImGuiCol_Border] = Blend(ImVec4(0.23f, 0.18f, 0.33f, 1.0f), accent, 0.55f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.24f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.92f, 0.98f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.51f, 0.64f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_SliderGrabActive] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
    style.Colors[ImGuiCol_Separator] = Blend(ImVec4(0.22f, 0.17f, 0.30f, 1.0f), accent, 0.40f);
    style.Colors[ImGuiCol_ScrollbarBg] = Blend(ImVec4(0.06f, 0.05f, 0.08f, 0.7f), accent, 0.10f);
    style.Colors[ImGuiCol_ScrollbarGrab] = Blend(ImVec4(0.30f, 0.25f, 0.38f, 1.0f), accent, 0.45f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = Blend(ImVec4(0.40f, 0.33f, 0.50f, 1.0f), accent, 0.65f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = accent;
    style.Colors[ImGuiCol_Button] = Blend(ImVec4(0.15f, 0.11f, 0.22f, 1.0f), accent, 0.18f);
    style.Colors[ImGuiCol_ButtonHovered] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.12f);
    style.Colors[ImGuiCol_ButtonActive] = accent;
    style.Colors[ImGuiCol_Header] = Blend(ImVec4(0.13f, 0.10f, 0.18f, 1.0f), accent, 0.30f);
    style.Colors[ImGuiCol_HeaderHovered] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.14f);
    style.Colors[ImGuiCol_HeaderActive] = accent;
    style.Colors[ImGuiCol_Tab] = Blend(ImVec4(0.11f, 0.09f, 0.15f, 1.0f), accent, 0.24f);
    style.Colors[ImGuiCol_TabHovered] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.14f);
    style.Colors[ImGuiCol_TabActive] = Blend(ImVec4(0.17f, 0.13f, 0.23f, 1.0f), accent, 0.45f);
    style.Colors[ImGuiCol_TabUnfocused] = Blend(ImVec4(0.09f, 0.07f, 0.12f, 1.0f), accent, 0.14f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = Blend(ImVec4(0.13f, 0.10f, 0.18f, 1.0f), accent, 0.28f);
    style.Colors[ImGuiCol_ResizeGrip] = Blend(accent, ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.35f);
    style.Colors[ImGuiCol_ResizeGripHovered] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.10f);
    style.Colors[ImGuiCol_ResizeGripActive] = accent;
    style.Colors[ImGuiCol_NavHighlight] = accent;
    style.Colors[ImGuiCol_TextSelectedBg] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
    style.Colors[ImGuiCol_DragDropTarget] = Blend(accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
}

} // namespace reportpanel
