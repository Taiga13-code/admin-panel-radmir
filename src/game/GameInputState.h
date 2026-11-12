#pragma once

#include <atomic>
#include <cstdint>

namespace reportpanel {
inline std::atomic<bool> g_gameInputAutomationActive{false};
inline std::atomic<bool> g_panelTextInputActive{false};
inline std::atomic<unsigned int> g_effectiveInputLangId{0};
inline constexpr std::uintptr_t kGameInputAutomationTag = 0x52504E4Cu;
}
