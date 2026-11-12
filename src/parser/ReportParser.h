#pragma once

#include "../core/Report.h"

#include <optional>
#include <string>
#include <string_view>

namespace reportpanel {

class ReportParser {
public:
    [[nodiscard]] std::optional<ParsedEvent> Parse(
        std::string_view utf8Line,
        std::string eventKey,
        std::string_view adminNick = {}) const;
};

} // namespace reportpanel
