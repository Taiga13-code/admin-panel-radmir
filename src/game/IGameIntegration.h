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
};

} // namespace reportpanel
