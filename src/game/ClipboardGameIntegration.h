#pragma once

#include "IGameIntegration.h"
#include <string>

namespace reportpanel {

class ClipboardGameIntegration final : public IGameIntegration {
public:
    [[nodiscard]] GameActionResult PrepareSpectate(const Report& report) override;
    [[nodiscard]] GameActionResult SendPrivateMessage(const Report& report, std::string_view text) override;

private:
    [[nodiscard]] GameActionResult SendGameCommand(const std::string& commandUtf8);
};

} // namespace reportpanel