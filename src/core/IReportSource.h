#pragma once

#include "Report.h"

#include <string>

namespace reportpanel {

class IReportSource {
public:
    virtual ~IReportSource() = default;
    virtual void Start() = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual std::string Status() const = 0;
};

} // namespace reportpanel
