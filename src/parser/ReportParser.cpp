#include "ReportParser.h"

#include <regex>
#include <cctype>
#include <string>

namespace reportpanel {
namespace {

const std::regex& IncomingPattern() {
    static const std::regex pattern(
        R"(^\[(\d{2}:\d{2}:\d{2})\]\s+((?:\[(?:RADMIR|HASSLE|FAQ)(?:\s+\d+)?\]\s*)+)([^\s\[\]:]+)\[(\d+)\]:\s*(.*)$)",
        std::regex::ECMAScript | std::regex::icase);
    return pattern;
}

const std::regex& AnswerPattern() {
    static const std::regex pattern(
        R"(^\[(\d{2}:\d{2}:\d{2})\]\s+(?:Администратор\s+[^\s\[\]:]+\[\d+\]|Агент поддержки\s+[^\s\[\]:]+\[\d+\])\s+для\s+([^\s\[\]:]+)\[(\d+)\]:)",
        std::regex::ECMAScript | std::regex::icase);
    return pattern;
}

const std::regex& BlockPattern() {
    static const std::regex pattern(
        R"(^\[(\d{2}:\d{2}:\d{2})\]\s+Администратор\s+[^\s\[\]:]+\[\d+\]\s+заблокировал репорт игроку\s+([^\s\[\]:]+)(?:\[(\d+)\])?\s+на\s+\d+\s+мин\.)",
        std::regex::ECMAScript);
    return pattern;
}

const std::regex& AdminReportPattern() {
    // This follows the report action format parsed by Taiga13-code/Radmir-monitor:
    // "Администратор <nick>[id] для <player>[id]: <text>".
    static const std::regex pattern(
        R"(^\[(\d{2}:\d{2}:\d{2})\]\s+(?:\[A\]\s*)?Администратор\s+([^\s\[\]:]+)\[(\d+)\]\s+для\s+([^\s\[\]:]+)\[(\d+)\]:\s*(.*)$)",
        std::regex::ECMAScript | std::regex::icase);
    return pattern;
}

const std::regex& DisconnectPattern() {
    // Disconnect lines seen in SA-MP/RADMIR chatlogs:
    //   "[HH:MM:SS] Nick_Name[12] has left the server (0:1)"
    //   "[HH:MM:SS] Nick_Name[12] вышел из игры"
    //   "[HH:MM:SS] [RADMIR] Игрок Nick[12] покинул сервер"
    // Optional tag/prefix words in front of the nickname are tolerated because
    // some servers announce the disconnect through a labelled channel.
    static const std::regex pattern(
        R"(^\[(\d{2}:\d{2}:\d{2})\]\s+(?:\[[^\]]*\]\s*)*(?:[Ии]грок(?:а)?\s+|Player\s+)?([^\s\[\]():]+)\s*(?:\[(\d+)\]|\((\d+)\))?\s*(?:has\s+left\s+the\s+server|left\s+the\s+server|disconnected|[Вв]ышел\s+из\s+игры|[Пп]окинул\s+(?:сервер|игру)|[Оо]тключился(?:\s+от\s+сервера)?|[Оо]тключ[её]н))",
        std::regex::ECMAScript | std::regex::icase);
    return pattern;
}

const std::regex& ColorPattern();

std::string StripColorTags(std::string value) {
    value = std::regex_replace(value, ColorPattern(), "");
    return value;
}

std::string FoldName(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (first < 0x80) {
            result.push_back(static_cast<char>(std::tolower(first)));
            ++i;
            continue;
        }
        if (i + 1 < value.size() && first == 0xD0) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if (second >= 0x90 && second <= 0x9F) {
                result.push_back(static_cast<char>(0xD0));
                result.push_back(static_cast<char>(second + 0x20));
                i += 2;
                continue;
            }
            if (second >= 0xA0 && second <= 0xAF) {
                result.push_back(static_cast<char>(0xD1));
                result.push_back(static_cast<char>(second - 0x20));
                i += 2;
                continue;
            }
            if (second == 0x81) {
                result.push_back(static_cast<char>(0xD1));
                result.push_back(static_cast<char>(0x91));
                i += 2;
                continue;
            }
        }
        result.push_back(static_cast<char>(first));
        ++i;
    }
    return result;
}

bool EqualsInsensitive(std::string_view left, std::string_view right) {
    return FoldName(left) == FoldName(right);
}

const std::regex& ColorPattern() {
    static const std::regex pattern(R"(\{[0-9A-Fa-f]{6,8}\})", std::regex::ECMAScript);
    return pattern;
}

std::string StripColors(std::string value) {
    value = std::regex_replace(value, ColorPattern(), "");
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    constexpr std::size_t maxBytes = 4096;
    if (value.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 &&
               (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
        if (cut > 0) {
            const auto lead = static_cast<unsigned char>(value[cut - 1]);
            const std::size_t width = lead < 0x80 ? 1 :
                (lead & 0xE0) == 0xC0 ? 2 :
                (lead & 0xF0) == 0xE0 ? 3 :
                (lead & 0xF8) == 0xF0 ? 4 : 1;
            if (cut - 1 + width > maxBytes) --cut;
        }
        value.resize(cut);
    }
    return value;
}

std::optional<std::uint32_t> ParsePlayerId(const std::string& value) {
    try {
        const auto parsed = std::stoul(value);
        if (parsed > 0xFFFFFFFFUL) return std::nullopt;
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::optional<ParsedEvent> ReportParser::Parse(
    std::string_view utf8Line,
    std::string eventKey,
    std::string_view adminNick) const {
    const std::string line(utf8Line);
    const std::string normalized = StripColorTags(line);
    std::smatch match;

    if (!adminNick.empty()) {
        if (std::regex_match(normalized, match, AdminReportPattern()) &&
            EqualsInsensitive(match[2].str(), adminNick)) {
            ParsedEvent event;
            event.kind = ReportEventKind::Answered;
            event.countsAsAdminReply = true;
            event.eventKey = std::move(eventKey);
            event.timeText = match[1].str();
            event.adminName = match[2].str();
            event.playerName = match[4].str();
            event.playerId = ParsePlayerId(match[5].str());
            event.message = StripColors(match[6].str());
            return event;
        }
    }

    if (std::regex_match(normalized, match, IncomingPattern())) {
        const auto playerId = ParsePlayerId(match[4].str());
        if (!playerId) return std::nullopt;
        ParsedEvent event;
        event.kind = ReportEventKind::NewReport;
        event.eventKey = std::move(eventKey);
        event.timeText = match[1].str();
        event.playerName = match[3].str();
        event.playerId = playerId;
        event.message = StripColors(match[5].str());
        if (event.message.empty()) return std::nullopt;
        return event;
    }

    if ((line.find("Администратор") != std::string::npos ||
         line.find("Агент поддержки") != std::string::npos) &&
        std::regex_search(normalized, match, AnswerPattern())) {
        const auto playerId = ParsePlayerId(match[3].str());
        if (!playerId) return std::nullopt;
        ParsedEvent event;
        event.kind = ReportEventKind::Answered;
        event.eventKey = std::move(eventKey);
        event.timeText = match[1].str();
        event.playerName = match[2].str();
        event.playerId = playerId;
        return event;
    }

    if (line.find("заблокировал репорт игроку") != std::string::npos &&
        std::regex_search(normalized, match, BlockPattern())) {
        ParsedEvent event;
        event.kind = ReportEventKind::Blocked;
        event.eventKey = std::move(eventKey);
        event.timeText = match[1].str();
        event.playerName = match[2].str();
        if (match[3].matched) event.playerId = ParsePlayerId(match[3].str());
        return event;
    }

    if (std::regex_search(normalized, match, DisconnectPattern())) {
        const auto idText = match[3].matched ? match[3].str() : match[4].str();
        const auto playerId = ParsePlayerId(idText);
        if (!playerId) return std::nullopt;
        ParsedEvent event;
        event.kind = ReportEventKind::PlayerDisconnected;
        event.eventKey = std::move(eventKey);
        event.timeText = match[1].str();
        event.playerName = match[2].str();
        event.playerId = playerId;
        return event;
    }

    return std::nullopt;
}

} // namespace reportpanel
