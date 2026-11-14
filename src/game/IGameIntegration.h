#pragma once

#include "../core/Report.h"

#include <string>
#include <string_view>

namespace reportpanel {

struct GameActionResult {
    bool succeeded{false};
    std::string message;
};

class IGameIntegration {
public:
    virtual ~IGameIntegration() = default;
    [[nodiscard]] virtual GameActionResult PrepareSpectate(const Report& report) = 0;
    [[nodiscard]] virtual GameActionResult SendPrivateMessage(
        const Report& report, std::string_view text) = 0;
    // Произвольная команда (/mute, /warn, /kick…) без привязки к репорту.
    [[nodiscard]] virtual GameActionResult SendCommand(std::string_view commandUtf8) {
        (void)commandUtf8;
        return {false, "Отправка команд недоступна."};
    }
};

} // namespace reportpanel
