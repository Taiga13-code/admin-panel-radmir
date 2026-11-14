#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>

namespace reportpanel {
inline std::atomic<bool> g_gameInputAutomationActive{false};
inline std::atomic<bool> g_panelTextInputActive{false};
inline std::atomic<unsigned int> g_effectiveInputLangId{0};
inline std::atomic<HWND> g_gameWindow{nullptr};
inline std::filesystem::path g_pluginDirectory;
inline constexpr std::uintptr_t kGameInputAutomationTag = 0x52504E4Cu;
}
