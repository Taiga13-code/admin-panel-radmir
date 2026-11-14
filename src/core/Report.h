#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace reportpanel {

enum class ReportStatus {
    Unanswered,
    Answered,
    Blocked
};

enum class ReportEventKind {
    NewReport,
    Answered,
    Blocked,
    AdminReply,
    PlayerDisconnected
};

struct ParsedEvent {
    ReportEventKind kind{ReportEventKind::NewReport};
    bool isHistorical{false};
    bool countsAsAdminReply{false};
    std::string eventKey;
    std::string timeText;
    std::string adminName;
    std::string playerName;
    std::optional<std::uint32_t> playerId;
    std::string message;
};

struct Report {
    std::uint64_t localId{0};
    std::uint32_t playerId{0};
    std::string playerName;
    std::string message;
    std::string timeText;
    ReportStatus status{ReportStatus::Unanswered};
    std::string sourceKey;
    std::chrono::steady_clock::time_point highlightedUntil{};
    bool playerDisconnected{false};
};

inline const char* StatusLabel(ReportStatus status) {
    switch (status) {
    case ReportStatus::Answered: return "Отвечен";
    case ReportStatus::Blocked: return "Заблокирован";
    default: return "Не отвечен";
    }
}

} // namespace reportpanel
