#include "ReportPanelUi.h"

#include "../platform/Encoding.h"
#include "../game/GameInputState.h"
#include "../themes/Theme.h"

#include <Windows.h>
#include <commdlg.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <vector>
#include <chrono>

namespace reportpanel {
namespace {

ImVec4 Accent(const AppSettings& settings, float alpha = 1.0f) {
    return ImVec4(settings.accentR, settings.accentG, settings.accentB, alpha);
}

ImVec4 AccentSurface(const AppSettings& settings, ImVec4 base, float amount) {
    const auto accent = Accent(settings);
    return ImVec4(base.x + (accent.x - base.x) * amount,
                  base.y + (accent.y - base.y) * amount,
                  base.z + (accent.z - base.z) * amount,
                  base.w);
}

ImU32 ColorU32(ImVec4 color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto lower = [](std::wstring_view value) {
        const int required = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE,
            value.data(), static_cast<int>(value.size()), nullptr, 0,
            nullptr, nullptr, 0);
        if (required <= 0) return std::wstring(value);
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE,
            value.data(), static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr, 0);
        return result;
    };
    const auto h = lower(Utf8ToWide(haystack));
    const auto n = lower(Utf8ToWide(needle));
    return h.find(n) != std::wstring::npos;
}

void DrawClippedText(ImDrawList* draw, ImVec2 min, ImVec2 max,
                     ImVec2 position, ImU32 color, const char* text) {
    draw->PushClipRect(min, max, true);
    draw->AddText(position, color, text);
    draw->PopClipRect();
}

std::string LocalIdText(std::uint64_t id) {
    char value[32]{};
    std::snprintf(value, sizeof(value), "#%05llu",
                  static_cast<unsigned long long>(id));
    return value;
}

template <std::size_t N>
void CopyToBuffer(std::array<char, N>& target, std::string_view value) {
    target.fill('\0');
    const auto count = std::min(value.size(), target.size() - 1);
    std::copy_n(value.data(), count, target.data());
}

} // namespace

ReportPanelUi::ReportPanelUi(ReportManager& reports, IReportSource& source,
                             IGameIntegration& game, AppSettings& settings,
                             HWND hostWindow,
                             std::function<void()> settingsChanged,
                             std::function<void(const std::filesystem::path&)> sourcePathChanged,
                             std::function<void(std::string)> adminNickChanged,
                             std::function<void()> closePanel)
    : reports_(reports), source_(source), game_(game), settings_(settings),
      hostWindow_(hostWindow),
      settingsChanged_(std::move(settingsChanged)),
      sourcePathChanged_(std::move(sourcePathChanged)),
      adminNickChanged_(std::move(adminNickChanged)),
      closePanel_(std::move(closePanel)) {
    const auto path = WideToUtf8(settings_.chatlogPath.wstring());
    const auto count = std::min(path.size(), pathBuffer_.size() - 1);
    std::copy_n(path.data(), count, pathBuffer_.data());
    const auto nickCount = std::min(settings_.adminNick.size(), adminNickBuffer_.size() - 1);
    std::copy_n(settings_.adminNick.data(), nickCount, adminNickBuffer_.data());
}

ReportPanelUi::~ReportPanelUi() = default;

void ReportPanelUi::OnPanelVisibility(bool visible) {
    if (visible) followDisabled_ = false;
    if (!visible) {
        g_panelTextInputActive.store(false, std::memory_order_relaxed);
        CancelReplyComposer();
        replyPopupRequested_ = false;
        replyBuffer_.fill('\0');
        if (std::string(adminNickBuffer_.data()) != settings_.adminNick)
            ApplyAdminNick();
    }
}

void ReportPanelUi::ShowToast(std::string message) {
    toast_ = std::move(message);
    toastUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void ReportPanelUi::ApplyPath(const std::filesystem::path& path) {
    if (path.empty()) return;
    settings_.chatlogPath = path;
    if (sourcePathChanged_) sourcePathChanged_(path);
    const auto value = WideToUtf8(path.wstring());
    CopyToBuffer(pathBuffer_, value);
    if (settingsChanged_) settingsChanged_();
    ShowToast("Путь к chatlog обновлён.");
}

void ReportPanelUi::ApplyAdminNick() {
    std::string nick(adminNickBuffer_.data());
    const auto first = nick.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) nick.clear();
    else nick = nick.substr(first, nick.find_last_not_of(" \t\r\n") - first + 1);
    if (nick == settings_.adminNick) return;

    settings_.adminNick = std::move(nick);
    if (adminNickChanged_) adminNickChanged_(settings_.adminNick);
    if (settingsChanged_) settingsChanged_();
    CopyToBuffer(adminNickBuffer_, settings_.adminNick);
    ShowToast(settings_.adminNick.empty()
        ? "Подсчёт ответов выключен: укажите ник администратора."
        : "Ник администратора сохранён; пересчитываю недавние ответы.");
}

void ReportPanelUi::CancelReplyComposer() {
    replyTarget_.reset();
    replyInputNeedsFocus_ = false;
    replyInputWasActive_ = false;
    g_panelTextInputActive.store(false, std::memory_order_relaxed);
}

bool ReportPanelUi::IsAnswered(const Report& report) const {
    return report.status != ReportStatus::Unanswered;
}

bool ReportPanelUi::Matches(const Report& report) const {
    if (filter_ == Filter::Unanswered && IsAnswered(report)) return false;
    if (filter_ == Filter::Answered && !IsAnswered(report)) return false;

    const std::string query(searchBuffer_.data());
    if (query.empty()) return true;
    return ContainsInsensitive(report.playerName, query) ||
           ContainsInsensitive(report.message, query) ||
           std::to_string(report.playerId).find(query) != std::string::npos ||
           LocalIdText(report.localId).find(query) != std::string::npos;
}

void ReportPanelUi::RenderHeader() {
    const auto draw = ImGui::GetWindowDrawList();
    const auto start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const auto accentWash = ColorU32(Accent(settings_, 0.15f));
    const auto clearWash = ColorU32(Accent(settings_, 0.015f));
    draw->AddRectFilledMultiColor(start, ImVec2(start.x + width, start.y + 49.0f),
        accentWash, clearWash, clearWash, accentWash);
    draw->AddRectFilledMultiColor(
        ImVec2(start.x, start.y + 48.0f), ImVec2(start.x + width, start.y + 49.5f),
        ColorU32(Accent(settings_, 0.68f)), ColorU32(Accent(settings_, 0.10f)),
        ColorU32(Accent(settings_, 0.10f)), ColorU32(Accent(settings_, 0.68f)));

    ImGui::SetCursorScreenPos(start);
    const float dragWidth = moveMode_
        ? std::max(110.0f, width - 205.0f)
        : std::min(130.0f, width - 205.0f);
    ImGui::InvisibleButton("##header_drag", ImVec2(dragWidth, 48.0f));
    if (moveMode_ && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 moved(ImGui::GetWindowPos().x + ImGui::GetIO().MouseDelta.x,
                           ImGui::GetWindowPos().y + ImGui::GetIO().MouseDelta.y);
        ImGui::SetWindowPos(moved);
    }

    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.16f,
                  ImVec2(start.x + 4.0f, start.y + 9.0f),
                  ColorU32(Accent(settings_)), "Thelp");

    const auto pending = reports_.UnansweredCount();
    char countText[64]{};
    std::snprintf(countText, sizeof(countText), "%zu неотвеченных", pending);
    const float pendingX = std::clamp(width * 0.265f, 150.0f, 265.0f);
    ImGui::SetCursorScreenPos(ImVec2(start.x + pendingX, start.y + 9.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(settings_, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(settings_, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
    ImGui::Button(countText, ImVec2(160.0f, 31.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    const std::string todayText = settings_.adminNick.empty()
        ? "Укажите ник в настройках"
        : "Репортов сегодня: " + std::to_string(reports_.AdminRepliesToday());
    const float todayX = pendingX + 174.0f;
    const float todayWidth = std::clamp(width - todayX - 100.0f, 150.0f, 176.0f);
    ImGui::SetCursorScreenPos(ImVec2(start.x + todayX, start.y + 12.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,
        AccentSurface(settings_, ImVec4(0.12f, 0.10f, 0.17f, 0.92f), 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        AccentSurface(settings_, ImVec4(0.17f, 0.13f, 0.23f, 0.98f), 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
    ImGui::Button(todayText.c_str(), ImVec2(todayWidth, 31.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::SetCursorScreenPos(ImVec2(start.x + width - 90.0f, start.y + 9.0f));
    if (ImGui::Button(settingsOpen_ ? "Закрыть" : "Настройки", ImVec2(86.0f, 34.0f))) {
        settingsOpen_ = !settingsOpen_;
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 50.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(1.0f, 5.0f));
}

void ReportPanelUi::RenderFilters() {
    const auto drawFilter = [&](const char* label, Filter value) {
        const bool active = filter_ == value;
        ImGui::PushStyleColor(ImGuiCol_Button,
            active ? Accent(settings_) : AccentSurface(settings_, ImVec4(0.12f, 0.10f, 0.17f, 1.0f), 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(settings_, 0.78f));
        if (ImGui::Button(label, ImVec2(112.0f, 34.0f))) filter_ = value;
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 7.0f);
    };

    drawFilter("Все", Filter::All);
    drawFilter("Неотвеченные", Filter::Unanswered);
    drawFilter("Отвеченные", Filter::Answered);

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Поиск по ID, нику или тексту…",
                             searchBuffer_.data(), searchBuffer_.size());
}

void ReportPanelUi::RenderReportList(float availableWidth, float availableHeight) {
    ImGui::BeginChild("##reports", ImVec2(availableWidth, availableHeight), false);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float fullWidth = ImGui::GetContentRegionAvail().x;

    const float scale = std::clamp(fullWidth / 900.0f, 0.62f, 1.0f);
    const float idWidth = 82.0f * scale;
    const float playerWidth = 210.0f * scale;
    const float timeWidth = 92.0f * scale;
    const float statusWidth = 115.0f * scale;
    const float followWidth = std::max(62.0f, 72.0f * scale);
    const float replyWidth = std::max(70.0f, 80.0f * scale);
    const float actionWidth = followWidth + replyWidth + 5.0f;
    const float messageWidth = std::max(70.0f,
        fullWidth - idWidth - playerWidth - timeWidth - statusWidth - actionWidth -
        42.0f * scale);
    // Кнопки прижаты к левому краю строки: новые репорты сдвигают только
    // вертикаль, поэтому по кнопкам не приходится «перенацеливаться».
    const float xAction = origin.x + 10.0f;
    const float xId = xAction + actionWidth + 16.0f * scale;
    const float xPlayer = xId + idWidth;
    const float xMessage = xPlayer + playerWidth;
    const float xTime = xMessage + messageWidth;
    const float xStatus = xTime + timeWidth;

    const auto draw = ImGui::GetWindowDrawList();
    const auto headerColor = IM_COL32(157, 145, 182, 255);
    const float headerY = origin.y + 6.0f;
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xAction, headerY), headerColor, "ДЕЙСТВИЯ");
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xId, headerY), headerColor, "ID репорта");
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xPlayer, headerY), headerColor, "ИГРОК");
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xMessage, headerY), headerColor, "Текст репорта");
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xTime, headerY), headerColor, "ВРЕМЯ");
    DrawClippedText(draw, origin, ImVec2(origin.x + fullWidth, origin.y + 30.0f),
                    ImVec2(xStatus, headerY), headerColor, "СТАТУС");
    ImGui::Dummy(ImVec2(1.0f, 28.0f));
    ImGui::Separator();

    const auto items = reports_.Snapshot();
    const std::string query(searchBuffer_.data());
    if (cachedSnapshot_ != items || cachedSearch_ != query || cachedFilter_ != filter_) {
        filteredReports_.clear();
        filteredReports_.reserve(items->size());
        for (auto it = items->rbegin(); it != items->rend(); ++it) {
            if (Matches(*it)) filteredReports_.push_back(&*it);
        }
        cachedSnapshot_ = items;
        cachedSearch_ = query;
        cachedFilter_ = filter_;
    }
    const float rowHeight = 58.0f;
    const int visible = static_cast<int>(filteredReports_.size());
    const float animationTime = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ImGuiListClipper clipper;
    clipper.Begin(visible, rowHeight);
    while (clipper.Step()) {
      for (int itemIndex = clipper.DisplayStart; itemIndex < clipper.DisplayEnd; ++itemIndex) {
        const auto& report = *filteredReports_[static_cast<std::size_t>(itemIndex)];

        const auto rowMin = ImGui::GetCursorScreenPos();
        const auto rowMax = ImVec2(rowMin.x + fullWidth, rowMin.y + rowHeight - 5.0f);
        const bool hovered = ImGui::IsMouseHoveringRect(rowMin, rowMax, true);
        const bool markAnswered = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        const bool unanswered = report.status == ReportStatus::Unanswered;
        const bool isNew = settings_.notifyNew && unanswered &&
            std::chrono::steady_clock::now() < report.highlightedUntil;
        const float pulse = 0.58f + 0.42f *
            (0.5f + 0.5f * std::sin(animationTime * 4.2f));
        const ImVec4 cardBase = unanswered
            ? ImVec4(0.075f, 0.055f, 0.10f, 1.0f)
            : ImVec4(0.065f, 0.060f, 0.09f, 1.0f);
        const ImVec4 cardTint = isNew
            ? AccentSurface(settings_, cardBase, 0.48f)
            : unanswered ? AccentSurface(settings_, cardBase, 0.28f)
                         : AccentSurface(settings_, cardBase, 0.17f);
        const ImU32 background = ColorU32(cardTint);
        const ImVec2 glowMin(rowMin.x - 2.0f, rowMin.y - 1.0f);
        const ImVec2 glowMax(rowMax.x + 2.0f, rowMax.y + 1.0f);
        draw->AddRectFilled(ImVec2(rowMin.x, rowMin.y + 3.0f),
                            ImVec2(rowMax.x, rowMax.y + 3.0f),
                            IM_COL32(0, 0, 0, 54), 10.0f);
        if (isNew || (unanswered && hovered)) {
            const float glowAlpha = isNew ? 0.035f + pulse * 0.035f : 0.055f;
            draw->AddRectFilled(glowMin, glowMax,
                ColorU32(Accent(settings_, glowAlpha)), 11.0f);
        }
        draw->AddRectFilled(rowMin, rowMax, background, 9.0f);
        draw->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y),
            unanswered ? IM_COL32(245, 63, 101, isNew ? 245 : 220)
                       : ColorU32(Accent(settings_, 0.55f)), 8.0f);
        const float borderAlpha = isNew ? 0.28f + pulse * 0.30f
            : hovered ? 0.40f : unanswered ? 0.16f : 0.08f;
        draw->AddRect(rowMin, rowMax, ColorU32(Accent(settings_, borderAlpha)),
                      9.0f, 0, hovered || isNew ? 1.25f : 0.8f);

        const float textY = rowMin.y + (rowHeight - ImGui::GetFontSize()) * 0.5f - 2.0f;
        const auto localId = LocalIdText(report.localId);
        const auto idSize = ImGui::CalcTextSize(localId.c_str());
        const ImVec2 idBadgeMin(xId - 5.0f, textY - 3.0f);
        const ImVec2 idBadgeMax(xId + idSize.x + 8.0f, textY + idSize.y + 3.0f);
        draw->AddRectFilled(idBadgeMin, idBadgeMax,
                            ColorU32(Accent(settings_, 0.13f)), 6.0f);
        draw->AddRect(idBadgeMin, idBadgeMax,
                      ColorU32(Accent(settings_, 0.30f)), 6.0f);
        DrawClippedText(draw, ImVec2(xId - 5.0f, rowMin.y),
                        ImVec2(xPlayer - 6.0f, rowMax.y),
                        ImVec2(xId, textY), ColorU32(Accent(settings_)), localId.c_str());

        const ImU32 mainText = IM_COL32(238, 234, 245, 255);
        DrawClippedText(draw, ImVec2(xPlayer, rowMin.y), ImVec2(xMessage - 10.0f, rowMax.y),
                        ImVec2(xPlayer, textY), mainText, report.playerName.c_str());
        char playerIdText[32]{};
        std::snprintf(playerIdText, sizeof(playerIdText), "[ID %u]", report.playerId);
        draw->AddText(ImVec2(xPlayer, textY + ImGui::GetFontSize() + 1.0f),
                      IM_COL32(139, 129, 161, 255), playerIdText);

        DrawClippedText(draw, ImVec2(xMessage, rowMin.y), ImVec2(xTime - 10.0f, rowMax.y),
                        ImVec2(xMessage, textY), mainText, report.message.c_str());
        DrawClippedText(draw, ImVec2(xTime, rowMin.y), ImVec2(xStatus - 8.0f, rowMax.y),
                        ImVec2(xTime, textY), IM_COL32(169, 157, 192, 255),
                        report.timeText.c_str());

        ImVec4 statusColor(0.93f, 0.26f, 0.35f, 1.0f);
        if (report.status == ReportStatus::Answered) statusColor = ImVec4(0.35f, 0.77f, 0.48f, 1.0f);
        if (report.status == ReportStatus::Blocked) statusColor = ImVec4(0.97f, 0.63f, 0.23f, 1.0f);
        const char* statusLabel = StatusLabel(report.status);
        if (report.playerDisconnected) {
            statusColor = ImVec4(0.95f, 0.82f, 0.22f, 1.0f);  // Yellow
            statusLabel = "Вышел из игры";
        }
        const ImVec2 statusTextSize = ImGui::CalcTextSize(statusLabel);
        const float badgeWidth = statusTextSize.x + 25.0f;
        const ImVec2 badgeMin(xStatus + std::max(0.0f, (statusWidth - badgeWidth) * 0.5f),
                              rowMin.y + (rowHeight - 26.0f) * 0.5f - 2.0f);
        const ImVec2 badgeMax(badgeMin.x + badgeWidth, badgeMin.y + 26.0f);
        draw->AddRectFilled(badgeMin, badgeMax,
            ColorU32(ImVec4(statusColor.x, statusColor.y, statusColor.z, 0.16f)), 13.0f);
        draw->AddRect(badgeMin, badgeMax,
            ColorU32(ImVec4(statusColor.x, statusColor.y, statusColor.z, 0.24f)), 13.0f);
        draw->AddCircleFilled(ImVec2(badgeMin.x + 11.0f, (badgeMin.y + badgeMax.y) * 0.5f),
                              3.5f, ColorU32(statusColor));
        draw->AddText(ImVec2(badgeMin.x + 19.0f,
                      badgeMin.y + (26.0f - statusTextSize.y) * 0.5f),
                      ColorU32(statusColor), statusLabel);

        ImGui::SetCursorScreenPos(ImVec2(xAction - 1.0f, rowMin.y + 10.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(settings_, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(settings_, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(settings_));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        const auto replyId = std::to_string(report.localId);
        ImGui::PushID(replyId.c_str());
        ImGui::BeginDisabled(followDisabled_);
        const bool followClicked = ImGui::Button("Следить", ImVec2(followWidth, 31.0f));
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 5.0f);
        const bool replyClicked = ImGui::Button("Ответить", ImVec2(replyWidth, 31.0f));
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        if (followClicked) {
            // Disable follow buttons immediately to prevent multiple clicks in the same frame
            followDisabled_ = true;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSpectateClickTime_ < spectateCooldownMs_) {
                ShowToast("Слишком быстро, повторите через 0.5 секунды");
                // Keep button disabled; it will be re‑enabled after cooldown expires
                lastSpectateClickTime_ = now;
                continue;
            }
            lastSpectateClickTime_ = now;
            const auto result = game_.PrepareSpectate(report);
            ShowToast(result.message);
            if (result.succeeded && closePanel_) {
                OnPanelVisibility(false);
                closePanel_();
            } else {
                // If spectate failed, allow trying again
                followDisabled_ = false;
            }
        }
        // Auto re‑enable follow button after cooldown expires
        if (followDisabled_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSpectateClickTime_ >= spectateCooldownMs_) {
                followDisabled_ = false;
            }
        }
        if (replyClicked) {
            replyTarget_ = report;
            replyBuffer_.fill('\0');
            ++replySessionId_;
            replyPopupRequested_ = true;
            replyInputNeedsFocus_ = true;
            replyInputWasActive_ = false;
            // Make the low-level hook translate characters immediately, before
            // the next rendered frame updates its state.
            g_panelTextInputActive.store(true, std::memory_order_relaxed);
        }
        if (markAnswered && report.status == ReportStatus::Unanswered) {
            if (reports_.MarkAnswered(report.localId))
                ShowToast("Репорт отмечен как отвеченный.");
        }
        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::TextWrapped("%s", report.message.c_str());
            ImGui::TextDisabled("%s  •  ID игрока %u  •  %s",
                                report.playerName.c_str(), report.playerId,
                                report.timeText.c_str());
            ImGui::EndTooltip();
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, rowMax.y + 4.0f));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
      }
    }

    if (visible == 0) {
        const auto message = items->empty()
            ? "Пока нет записей репортов в chatlog."
            : "Нет обращений по выбранному фильтру.";
        ImGui::Dummy(ImVec2(1.0f, 24.0f));
        ImGui::SetCursorPosX(14.0f);
        ImGui::TextDisabled("%s", message);
    }

    ImGui::EndChild();
}

void ReportPanelUi::RenderSettings(float width, float height) {
    ImGui::BeginChild("##settings", ImVec2(width, height), true,
                      0);
    ImGui::TextUnformatted("ВНЕШНИЙ ВИД");
    ImGui::Separator();

    if (ImGui::ColorEdit3("Акцентный цвет", &settings_.accentR,
                          ImGuiColorEditFlags_NoInputs)) {
        if (settingsChanged_) settingsChanged_();
    }
    const ImVec4 presets[] = {
        ImVec4(0.58f, 0.24f, 0.96f, 1.0f),
        ImVec4(0.20f, 0.55f, 0.98f, 1.0f),
        ImVec4(0.18f, 0.72f, 0.46f, 1.0f),
        ImVec4(0.96f, 0.42f, 0.22f, 1.0f),
        ImVec4(0.92f, 0.20f, 0.58f, 1.0f)
    };
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, presets[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
            std::min(1.0f, presets[i].x + 0.12f),
            std::min(1.0f, presets[i].y + 0.12f),
            std::min(1.0f, presets[i].z + 0.12f), 1.0f));
        if (ImGui::SmallButton("  ")) {
            settings_.accentR = presets[i].x;
            settings_.accentG = presets[i].y;
            settings_.accentB = presets[i].z;
            if (settingsChanged_) settingsChanged_();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PopID();
    }
    ImGui::NewLine();

    ImGui::Text("Непрозрачность: %d%%", settings_.transparency);
    if (ImGui::SliderInt("##opacity", &settings_.transparency, 30, 100)) {
        if (settingsChanged_) settingsChanged_();
    }
    ImGui::Text("Размер интерфейса: %d", settings_.fontSize);
    if (ImGui::SliderInt("##font", &settings_.fontSize, 12, 24)) {
        if (settingsChanged_) settingsChanged_();
    }
    if (ImGui::Checkbox("Подсвечивать новые", &settings_.notifyNew)) {
        if (settingsChanged_) settingsChanged_();
    }

    ImGui::Dummy(ImVec2(1.0f, 10.0f));
    ImGui::TextUnformatted("СЧЁТЧИК РЕПОРТОВ");
    ImGui::Separator();
    ImGui::TextWrapped("Укажи точный ник администратора из chatlog.");
    ImGui::SetNextItemWidth(-1.0f);
    const bool nickSubmitted = ImGui::InputTextWithHint("##admin_nick", "Имя_Фамилия",
        adminNickBuffer_.data(), adminNickBuffer_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    if (nickSubmitted || ImGui::IsItemDeactivatedAfterEdit()) ApplyAdminNick();
    if (ImGui::Button("Сохранить ник", ImVec2(-1.0f, 30.0f))) ApplyAdminNick();
    ImGui::Text("Репортов сегодня: %zu", reports_.AdminRepliesToday());

    ImGui::Dummy(ImVec2(1.0f, 10.0f));
    ImGui::TextUnformatted("ИСТОЧНИК");
    ImGui::Separator();
    ImGui::TextWrapped("%s", source_.Status().c_str());
    ImGui::TextWrapped("Chatlog читается без изменения файла.");
    ImGui::Dummy(ImVec2(1.0f, 4.0f));

    if (ImGui::Button("Выбрать chatlog", ImVec2(-1.0f, 34.0f))) {
        OPENFILENAMEW dialog{};
        wchar_t fileName[1024]{};
        const auto widePath = Utf8ToWide(pathBuffer_.data());
        std::copy_n(widePath.data(), std::min(widePath.size(), std::size(fileName) - 1), fileName);
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFile = fileName;
        dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
        dialog.lpstrFilter = L"Chatlog (*.txt)\0*.txt\0Все файлы (*.*)\0*.*\0";
        dialog.nFilterIndex = 1;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = L"Выберите RADMIR chatlog";
        if (GetOpenFileNameW(&dialog)) ApplyPath(std::filesystem::path(fileName));
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", pathBuffer_.data(), pathBuffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        ApplyPath(std::filesystem::path(Utf8ToWide(pathBuffer_.data())));
    }
    if (ImGui::Button("Применить путь", ImVec2(-1.0f, 30.0f))) {
        ApplyPath(std::filesystem::path(Utf8ToWide(pathBuffer_.data())));
    }

    ImGui::Dummy(ImVec2(1.0f, 12.0f));
    ImGui::TextUnformatted("ИНТЕГРАЦИЯ");
    ImGui::Separator();
    ImGui::TextWrapped("«Следить» отправляет /sp ID и закрывает панель. «Ответить» позволяет написать /pm ID текст, не закрывая панель.");

    ImGui::Dummy(ImVec2(1.0f, 8.0f));
    ImGui::Separator();
    ImGui::TextUnformatted("КЛАВИША ОТКРЫТИЯ ПАНЕЛИ");
    ImGui::Separator();
    // Show readable name for common keys
    const auto keyName = [](int vk) -> std::string {
        if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
        if (vk == VK_INSERT) return "Insert";
        if (vk == VK_DELETE) return "Delete";
        if (vk == VK_HOME) return "Home";
        if (vk == VK_END) return "End";
        if (vk == VK_PRIOR) return "PageUp";
        if (vk == VK_NEXT) return "PageDown";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
        return std::string(buf);
    };
    ImGui::Text("Текущая: %s (код %d)", keyName(settings_.toggleKey).c_str(), settings_.toggleKey);
    int keyInput = settings_.toggleKey;
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputInt("##toggleKey", &keyInput, 0, 0)) {
        if (keyInput >= 1 && keyInput <= 254) {
            settings_.toggleKey = keyInput;
            if (settingsChanged_) settingsChanged_();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F1=F11=0x7A");
    ImGui::EndChild();
}

void ReportPanelUi::RenderReplyComposer() {
    const std::string popupId = "Ответить###reply_compose_" +
        std::to_string(replySessionId_);
    const char* const popupLabel = popupId.c_str();

    if (!replyTarget_) {
        // OnPanelVisibility(false) cancels the target without an ImGui frame,
        // so a modal popup can still sit on the popup stack after the panel is
        // reopened. Close it before it blocks every interaction.
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                        ImGuiPopupFlags_AnyPopupLevel)) {
            ImGui::ClosePopupToLevel(0, false);
        }
        return;
    }

    if (replyPopupRequested_) {
        ImGui::OpenPopup(popupLabel);
        replyPopupRequested_ = false;
    } else if (!ImGui::IsPopupOpen(popupLabel)) {
        // Dismissed by Escape or lost when the panel was hidden. Do not leave
        // the panel in a state where all game keys are swallowed with no editor.
        CancelReplyComposer();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(440.0f, 225.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(popupLabel, &open,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushID(static_cast<int>(replySessionId_));
        ImGui::Text("Ответ игроку %s [ID %u]", replyTarget_->playerName.c_str(),
                    replyTarget_->playerId);
        const unsigned int inputLangId =
            g_effectiveInputLangId.load(std::memory_order_relaxed);
        const char* layoutTag = inputLangId == 0x0419 ? "RU"
            : inputLangId == 0x0409 ? "EN" : "??";
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", layoutTag);
        ImGui::TextDisabled("Будет отправлено: /pm %u <текст>", replyTarget_->playerId);

        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
        }
        // Фокус устанавливается только при явном запросе (открытие попапа или
        // неудачная отправка), если в предыдущем кадре поле не было активно.
        bool wasActive = replyInputWasActive_;
        if (replyInputNeedsFocus_ && !wasActive) {
            ImGui::SetKeyboardFocusHere();
            replyInputNeedsFocus_ = false;
        }
        bool submitOnEnter = false;
        const auto enterFilter = [](ImGuiInputTextCallbackData* data) -> int {
            if (data->EventChar == L'\n' || data->EventChar == L'\r') {
                if (ImGui::GetIO().KeyShift) return 0;
                *static_cast<bool*>(data->UserData) = true;
                return 1;
            }
            return 0;
        };
        ImGui::InputTextMultiline("##reply_text", replyBuffer_.data(),
            replyBuffer_.size(), ImVec2(-1.0f, 92.0f),
            ImGuiInputTextFlags_CallbackCharFilter, enterFilter, &submitOnEnter);

        // Remember whether the input field had focus this frame.
        // Used to avoid calling SetKeyboardFocusHere() when the user
        // is already typing, which would reset the cursor and wipe text.
        replyInputWasActive_ = ImGui::IsItemActive();
        replyInputNeedsFocus_ = false;

        const auto send = [&] {
            const auto text = std::string(replyBuffer_.data());
            const auto result = game_.SendPrivateMessage(*replyTarget_, text);
            ShowToast(result.message);
            if (result.succeeded) {
                CancelReplyComposer();
                ImGui::CloseCurrentPopup();
            } else {
                replyInputNeedsFocus_ = true;
            }
        };
        if (submitOnEnter) send();

        if (ImGui::Button("Отправить /pm", ImVec2(156.0f, 34.0f))) send();
        ImGui::SameLine();
        if (ImGui::Button("Отмена", ImVec2(100.0f, 34.0f))) {
            CancelReplyComposer();
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Enter — отправить, Shift+Enter — новая строка");
        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (!open) CancelReplyComposer();
}

void ReportPanelUi::Render() {
    ApplyDarkTheme(settings_);

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(settings_.windowX),
                                   static_cast<float>(settings_.windowY)),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(settings_.windowWidth),
                                    static_cast<float>(settings_.windowHeight)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(640.0f, 420.0f),
        ImVec2(std::max(640.0f, io.DisplaySize.x),
               std::max(420.0f, io.DisplaySize.y)));
    ImGui::Begin("Thelp", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);

    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRect(
        windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
        ColorU32(Accent(settings_, 0.46f)), 10.0f, 0, 1.2f);
    const int windowX = static_cast<int>(std::lround(windowPos.x));
    const int windowY = static_cast<int>(std::lround(windowPos.y));
    const int windowWidth = static_cast<int>(std::lround(windowSize.x));
    const int windowHeight = static_cast<int>(std::lround(windowSize.y));
    if (windowX != settings_.windowX || windowY != settings_.windowY ||
        windowWidth != settings_.windowWidth || windowHeight != settings_.windowHeight) {
        settings_.windowX = windowX;
        settings_.windowY = windowY;
        settings_.windowWidth = windowWidth;
        settings_.windowHeight = windowHeight;
        if (settingsChanged_) settingsChanged_();
    }

    RenderHeader();
    RenderFilters();

    const float bodyWidth = ImGui::GetContentRegionAvail().x;
    const float bodyHeight = std::max(80.0f,
        ImGui::GetContentRegionAvail().y - 82.0f);
    const float settingsWidth = settingsOpen_
        ? std::clamp(bodyWidth * 0.31f, 230.0f, 310.0f) : 0.0f;
    const float listWidth = settingsOpen_
        ? std::max(300.0f, bodyWidth - settingsWidth - 10.0f) : bodyWidth;

    ImGui::BeginGroup();
    RenderReportList(listWidth, bodyHeight);
    if (settingsOpen_) {
        ImGui::SameLine(0.0f, 10.0f);
        RenderSettings(settingsWidth, bodyHeight);
    }
    ImGui::EndGroup();

    RenderReplyComposer();
    ImGui::Separator();
    ImGui::BeginChild("##footer_toolbar", ImVec2(0.0f, 45.0f), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(ImVec4(0.35f, 0.78f, 0.50f, 1.0f), "●");
    ImGui::SameLine(0.0f, 7.0f);
    ImGui::TextUnformatted("Chatlog отслеживается");
    ImGui::SameLine(0.0f, 18.0f);
    if (ImGui::Button(moveMode_ ? "Перемещение: вкл." : "Переместить",
                      ImVec2(145.0f, 32.0f))) {
        moveMode_ = !moveMode_;
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::Button("Сменить тему", ImVec2(132.0f, 32.0f))) {
        const ImVec4 presets[] = {
            ImVec4(0.58f, 0.24f, 0.96f, 1.0f),
            ImVec4(0.20f, 0.55f, 0.98f, 1.0f),
            ImVec4(0.18f, 0.72f, 0.46f, 1.0f),
            ImVec4(0.96f, 0.42f, 0.22f, 1.0f),
            ImVec4(0.92f, 0.20f, 0.58f, 1.0f)
        };
        int closest = 0;
        float closestDistance = 100.0f;
        for (int index = 0; index < 5; ++index) {
            const float dr = settings_.accentR - presets[index].x;
            const float dg = settings_.accentG - presets[index].y;
            const float db = settings_.accentB - presets[index].z;
            const float distance = dr * dr + dg * dg + db * db;
            if (distance < closestDistance) {
                closestDistance = distance;
                closest = index;
            }
        }
        const auto& next = presets[(closest + 1) % 5];
        settings_.accentR = next.x;
        settings_.accentG = next.y;
        settings_.accentB = next.z;
        if (settingsChanged_) settingsChanged_();
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::Button("Дополнительно", ImVec2(145.0f, 32.0f))) {
        settingsOpen_ = !settingsOpen_;
    }
    ImGui::EndChild();
    ImGui::TextDisabled("F11 — открыть / скрыть   •   «Следить» — /sp   •   «Ответить» — /pm   •   ПКМ — отметить ответ");
    if (std::chrono::steady_clock::now() < toastUntil_) {
        const auto textSize = ImGui::CalcTextSize(toast_.c_str());
        const auto pos = ImVec2(windowPos.x + windowSize.x - textSize.x - 20.0f,
                                windowPos.y + windowSize.y - 34.0f);
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(pos.x - 10.0f, pos.y - 5.0f),
            ImVec2(pos.x + textSize.x + 10.0f, pos.y + textSize.y + 5.0f),
            IM_COL32(47, 34, 68, 245), 8.0f);
        ImGui::GetForegroundDrawList()->AddText(pos, IM_COL32(238, 230, 250, 255),
                                                toast_.c_str());
    }
    g_panelTextInputActive.store(io.WantTextInput || ImGui::IsAnyItemActive() ||
        replyTarget_.has_value(),
        std::memory_order_relaxed);
    ImGui::End();
}

} // namespace reportpanel
