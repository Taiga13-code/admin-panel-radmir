#include "ReportPanelUi.h"

#include "../platform/Encoding.h"
#include "../game/GameInputState.h"
#include "../themes/Theme.h"

#include <Windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
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

void ReplaceAll(std::string& text, std::string_view token, std::string_view value) {
    if (token.empty()) return;
    std::string result;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto found = text.find(token, pos);
        if (found == std::string::npos) {
            result.append(text, pos, std::string::npos);
            break;
        }
        result.append(text, pos, found - pos);
        result.append(value);
        pos = found + token.size();
    }
    text = std::move(result);
}

std::string Trimmed(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

// Закрывает только «свои» попапы (по префиксу имени окна), не трогая чужие.
// Раньше ветка сброса ответа вызывала ClosePopupToLevel(0), который убивал
// ЛЮБОЙ открытый попап — включая окно «Наказать», поэтому кнопка выглядела
// мёртвой: попап открывался и умирал на следующем кадре.
void CloseStalePopupsByPrefix(std::string_view prefix) {
    ImGuiContext& g = *GImGui;
    for (int level = static_cast<int>(g.OpenPopupStack.size()) - 1; level >= 0;
         --level) {
        const ImGuiWindow* popup = g.OpenPopupStack[level].Window;
        if (popup && popup->Name &&
            std::string_view(popup->Name).starts_with(prefix)) {
            ImGui::ClosePopupToLevel(level, true);
            return;
        }
    }
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
    toolkit_.Initialize(g_pluginDirectory.empty()
        ? std::filesystem::path(L".")
        : g_pluginDirectory);
    CopyToBuffer(notesBuffer_, toolkit_.notes);
}

ReportPanelUi::~ReportPanelUi() = default;

void ReportPanelUi::OnPanelVisibility(bool visible) {
    if (visible) followDisabled_ = false;
    if (!visible) {
        g_panelTextInputActive.store(false, std::memory_order_relaxed);
        CancelReplyComposer();
        replyPopupRequested_ = false;
        replyBuffer_.fill('\0');
        CancelPunishComposer();
        punishPopupRequested_ = false;
        // Вспомогательные попапы тоже закрываются (стек почистится при
        // следующем рендере — ветки !*_Open_ вызывают CloseStalePopupsByPrefix).
        timersOpen_ = false;
        timersPopupRequested_ = false;
        notesOpen_ = false;
        notesPopupRequested_ = false;
        commandsOpen_ = false;
        commandsPopupRequested_ = false;
        SaveNotesIfDirty();
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
    replySuggestions_.clear();
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

void ReportPanelUi::PlayNotificationSound(const std::string& soundName) const {
    if (!soundName.empty() && !g_pluginDirectory.empty()) {
        const std::wstring fullPath =
            (g_pluginDirectory / L"sound" / Utf8ToWide(soundName)).wstring();
        mciSendStringW(L"close reportSound", nullptr, 0, nullptr);
        std::wstring openCmd = L"open \"" + fullPath + L"\" type mpegvideo alias reportSound";
        mciSendStringW(openCmd.c_str(), nullptr, 0, nullptr);
        mciSendStringW(L"play reportSound", nullptr, 0, nullptr);
        return;
    }
    PlaySoundW(reinterpret_cast<const wchar_t*>(L"SystemAsterisk"),
               nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
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
    const float punishWidth = std::max(64.0f, 74.0f * scale);
    const float actionWidth = followWidth + replyWidth + punishWidth + 10.0f;
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

        // Фильтр рекламы/мата — подсветка репорта оранжевой меткой.
        std::string filterWord;
        const int filterHit = toolkit_.MatchesFilters(report.message, &filterWord);

        // Звуковое уведомление о новом репорте — один раз на каждый новый репорт.
        // Если репорт перестал быть новым, убираем его из множества, чтобы звук
        // мог сыграть снова при следующем новом репорте с тем же ID.
        if (isNew && settings_.soundNewReport) {
            if (announcedNewReportIds_.insert(report.playerId).second) {
                PlayNotificationSound(settings_.soundName);
            }
        } else if (!isNew) {
            announcedNewReportIds_.erase(report.playerId);
        }
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

        float messageTextX = xMessage;
        if (filterHit != 0) {
            const char* filterLabel = filterHit == 1 ? "Реклама" : "Мат";
            const ImVec2 filterSize = ImGui::CalcTextSize(filterLabel);
            const ImVec2 chipMin(xMessage, textY - 3.0f);
            const ImVec2 chipMax(xMessage + filterSize.x + 12.0f,
                                 textY + filterSize.y + 3.0f);
            draw->AddRectFilled(chipMin, chipMax, IM_COL32(232, 146, 58, 44), 6.0f);
            draw->AddRect(chipMin, chipMax, IM_COL32(232, 146, 58, 130), 6.0f);
            draw->AddText(ImVec2(chipMin.x + 6.0f, textY),
                          IM_COL32(245, 176, 96, 255), filterLabel);
            messageTextX = chipMax.x + 6.0f;
        }
        DrawClippedText(draw, ImVec2(xMessage, rowMin.y), ImVec2(xTime - 10.0f, rowMax.y),
                        ImVec2(messageTextX, textY), mainText, report.message.c_str());
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
        ImGui::SameLine(0.0f, 5.0f);
        const bool punishClicked = ImGui::Button("Наказать", ImVec2(punishWidth, 31.0f));
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
            replySuggestions_ = toolkit_.SuggestFor(report.message);
            // Make the low-level hook translate characters immediately, before
            // the next rendered frame updates its state.
            g_panelTextInputActive.store(true, std::memory_order_relaxed);
        }
        if (punishClicked) {
            punishTarget_ = report;
            ++punishSessionId_;
            punishPopupRequested_ = true;
            punishFormIndex_ = 0;
            punishMinutes_ = toolkit_.forms.empty() ? 30 : toolkit_.forms.front().defaultTime;
            punishReasonBuffer_.fill('\0');
        }
        if (markAnswered && report.status == ReportStatus::Unanswered) {
            if (reports_.MarkAnswered(report.localId))
                ShowToast("Репорт отмечен как отвеченный.");
        }
        if (hovered) {
            ImGui::BeginTooltip();
            if (filterHit != 0 && !filterWord.empty()) {
                ImGui::TextColored(ImVec4(0.96f, 0.69f, 0.38f, 1.0f),
                                   filterHit == 1 ? "Совпадение со списком рекламы: «%s»"
                                                 : "Совпадение со списком мата: «%s»",
                                   filterWord.c_str());
            }
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
    if (ImGui::Checkbox("Звук на новый репорт", &settings_.soundNewReport)) {
        if (settingsChanged_) settingsChanged_();
    }
    ImGui::TextDisabled("Звук уведомления (MP3 из папки sound\\ рядом с плагином):");
    RenderSoundList();
    if (ImGui::Button("Обновить список", ImVec2(126.0f, 24.0f))) toolkit_.RescanSounds();
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Тест звука", ImVec2(100.0f, 24.0f))) {
        PlayNotificationSound(settings_.soundName);
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
    ImGui::TextUnformatted("УТИЛИТЫ");
    ImGui::Separator();
    ImGui::Text("Наказаний за смену: %zu", punishCount_);
    if (ImGui::Button("Таймеры наказаний", ImVec2(-1.0f, 28.0f))) {
        timersOpen_ = true;
        timersPopupRequested_ = true;
    }
    if (ImGui::Button("Заметки", ImVec2(-1.0f, 28.0f))) {
        notesOpen_ = true;
        notesPopupRequested_ = true;
    }
    if (ImGui::Button("Справочник команд", ImVec2(-1.0f, 28.0f))) {
        commandsOpen_ = true;
        commandsPopupRequested_ = true;
    }

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
        // OnPanelVisibility(false) отменяет цель без кадра ImGui, поэтому
        // наш попап может остаться на стеке после переоткрытия панели.
        // ИСПРАВЛЕНИЕ: закрываем только СВОЙ попап ответа — раньше здесь
        // закрывался любой открытый попап (окно «Наказать» и пр.).
        CloseStalePopupsByPrefix("Ответить###reply_compose_");
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

    ImGui::SetNextWindowSize(ImVec2(460.0f, 380.0f), ImGuiCond_Appearing);
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

        // Подсказка ответа по ключевым словам (suggestions.json).
        if (!replySuggestions_.empty()) {
            ImGui::TextDisabled("Подсказка:");
            int suggestionIndex = 0;
            for (const auto* suggestion : replySuggestions_) {
                std::string label = suggestion->answer;
                if (label.size() > 46) {
                    label.resize(46);
                    label += "..";
                }
                const float buttonWidth =
                    ImGui::CalcTextSize(label.c_str()).x +
                    ImGui::GetStyle().FramePadding.x * 2.0f;
                if (suggestionIndex > 0 &&
                    ImGui::GetContentRegionAvail().x >= buttonWidth + 10.0f) {
                    ImGui::SameLine(0.0f, 6.0f);
                }
                ImGui::PushID(suggestionIndex);
                if (ImGui::SmallButton(label.c_str())) {
                    std::string current(replyBuffer_.data());
                    if (!current.empty()) {
                        while (!current.empty() &&
                               (current.back() == ' ' || current.back() == '\n' ||
                                current.back() == '\r')) {
                            current.pop_back();
                        }
                        current += "\n";
                    }
                    current += suggestion->answer;
                    if (current.size() < replyBuffer_.size()) {
                        std::memcpy(replyBuffer_.data(), current.c_str(),
                                    current.size() + 1);
                        replyInputWasActive_ = true;
                    }
                }
                ImGui::PopID();
                ++suggestionIndex;
            }
        }

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

        // Прямая проверка Enter на клавиатуре — callback не всегда ловит Enter
        // в InputTextMultiline с CallbackCharFilter.
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false) &&
            !ImGui::GetIO().KeyShift) {
            submitOnEnter = true;
        }

        const auto send = [&] {
            std::string text(replyBuffer_.data());
            // ИСПРАВЛЕНИЕ: Убираем символы новой строки, которые ImGui добавляет при нажатии Enter
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
            
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

        // Быстрые кнопки ответов (как в Arizona Tools) — сетка 2 колонки,
        // чтобы длинные подписи гарантированно помещались в окно ответа.
        ImGui::Separator();
        struct QuickReply { const char* label; const char* phrase; };
        const QuickReply quickReplies[] = {
            {"Помощь игроку", "Помощь игроку"},
            {"Да", "Да"},
            {"Нет", "Нет"},
            {"Не знаю", "Не знаю"},
            {"Слежу за ситуацией", "Слежу за ситуацией"},
            {"Доказательства в ЛС", "Доказательства пришлите в ЛС"},
        };
        const float quickSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float quickBtnWidth = (ImGui::GetContentRegionAvail().x - quickSpacing) / 2.0f;
        for (int i = 0; i < IM_ARRAYSIZE(quickReplies); ++i) {
            if (i % 2 != 0) ImGui::SameLine(0.0f, quickSpacing);
            ImGui::PushStyleColor(ImGuiCol_Button, Accent(settings_, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(settings_, 0.65f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(settings_, 1.0f));
            if (ImGui::Button(quickReplies[i].label, ImVec2(quickBtnWidth, 26.0f))) {
                std::string current(replyBuffer_.data());
                std::string phrase(quickReplies[i].phrase);
                std::string combined;
                if (current.empty()) {
                    combined = phrase;
                } else {
                    while (!current.empty() && (current.back() == ' ' || current.back() == '\n' || current.back() == '\r'))
                        current.pop_back();
                    combined = current + "\n" + phrase;
                }
                if (combined.size() < replyBuffer_.size()) {
                    std::memcpy(replyBuffer_.data(), combined.c_str(), combined.size() + 1);
                    replyInputWasActive_ = true;
                    replyInputNeedsFocus_ = false;
                }
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::Separator();
        if (ImGui::Button("Отправить /pm", ImVec2(150.0f, 32.0f))) send();
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Отмена", ImVec2(95.0f, 32.0f))) {
            CancelReplyComposer();
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Enter — отправить, Shift+Enter — новая строка");
        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (!open) CancelReplyComposer();
}

void ReportPanelUi::Tick() {
    // Таймеры наказаний тикают каждый кадр, даже когда панель скрыта —
    // иначе мут «протухнет» молча.
    if (punishTimers_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = punishTimers_.begin(); it != punishTimers_.end();) {
        if (now >= it->expiresAt) {
            ShowToast(it->label + " для «" + it->playerName + "» истёк.");
            it = punishTimers_.erase(it);
        } else {
            ++it;
        }
    }
}

void ReportPanelUi::SaveNotesIfDirty() {
    const std::string current(notesBuffer_.data());
    if (current == toolkit_.notes) return;
    toolkit_.notes = current;
    toolkit_.SaveNotes();
}

void ReportPanelUi::RenderSoundList() {
    ImGui::BeginChild("##sound_list", ImVec2(0.0f, 132.0f), true);
    if (ImGui::Selectable("Системный (динамик Windows)", settings_.soundName.empty())) {
        settings_.soundName.clear();
        if (settingsChanged_) settingsChanged_();
    }
    for (const auto& file : toolkit_.soundFiles) {
        if (ImGui::Selectable(file.c_str(), settings_.soundName == file)) {
            settings_.soundName = file;
            if (settingsChanged_) settingsChanged_();
        }
    }
    if (toolkit_.soundFiles.empty()) {
        ImGui::TextDisabled("Положи MP3 в папку sound\\ и нажми «Обновить список».");
    }
    ImGui::EndChild();
}

void ReportPanelUi::RenderNotesWindow() {
    const char* const popupLabel = "Заметки###notes";
    if (notesPopupRequested_) {
        ImGui::OpenPopup(popupLabel);
        notesPopupRequested_ = false;
    }
    if (!notesOpen_) {
        CloseStalePopupsByPrefix("Заметки###notes");
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(430.0f, 340.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(popupLabel, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Сохраняются в notes.txt рядом с плагином.");
        ImGui::InputTextMultiline("##notes", notesBuffer_.data(), notesBuffer_.size(),
                                  ImVec2(-1.0f, -42.0f));
        if (ImGui::Button("Сохранить", ImVec2(-1.0f, 30.0f))) {
            SaveNotesIfDirty();
            ShowToast("Заметки сохранены в notes.txt.");
        }
        ImGui::EndPopup();
    } else {
        // Попап закрыли (крестик или Escape) — сохраняем и чистим флаг.
        SaveNotesIfDirty();
        notesOpen_ = false;
    }
}

void ReportPanelUi::RenderCommandsWindow() {
    const char* const popupLabel = "Справочник команд###commands";
    if (commandsPopupRequested_) {
        ImGui::OpenPopup(popupLabel);
        commandsPopupRequested_ = false;
    }
    if (!commandsOpen_) {
        CloseStalePopupsByPrefix("Справочник команд###commands");
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(470.0f, 440.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(popupLabel, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##cmd_search", "Поиск по команде или описанию…",
                                 commandSearchBuffer_.data(), commandSearchBuffer_.size());
        ImGui::BeginChild("##cmd_list", ImVec2(0.0f, -40.0f), false);
        const std::string query(commandSearchBuffer_.data());
        std::string lastSection;
        int shown = 0;
        for (const auto& entry : toolkit_.commands) {
            if (!query.empty() && !ContainsInsensitive(entry.command, query) &&
                !ContainsInsensitive(entry.description, query) &&
                !ContainsInsensitive(entry.section, query)) {
                continue;
            }
            if (!entry.section.empty() && entry.section != lastSection) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", entry.section.c_str());
                lastSection = entry.section;
            }
            ImGui::PushID(static_cast<int>(shown++));
            ImGui::TextUnformatted(entry.command.c_str());
            ImGui::SameLine(118.0f);
            ImGui::TextDisabled("%s", entry.description.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Отправить")) {
                const auto result = game_.SendCommand(entry.command);
                ShowToast(result.message);
            }
            ImGui::PopID();
        }
        if (shown == 0) {
            ImGui::TextDisabled("Ничего не найдено. Заполни commands.json рядом с плагином.");
        }
        ImGui::EndChild();

        bool manualSend = ImGui::InputTextWithHint("##cmd_manual", "/команда …",
            commandBuffer_.data(), commandBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Отправить", ImVec2(96.0f, 0.0f))) manualSend = true;
        if (manualSend) {
            const std::string command = Trimmed(std::string_view(commandBuffer_.data()));
            if (!command.empty()) {
                const auto result = game_.SendCommand(command);
                ShowToast(result.message);
            }
        }
        ImGui::EndPopup();
    } else {
        commandsOpen_ = false;
    }
}

void ReportPanelUi::CancelPunishComposer() {
    punishTarget_.reset();
    punishPopupRequested_ = false;
}

std::string ReportPanelUi::BuildPunishCommand(const PunishForm& form) const {
    std::string command = form.command;
    if (!punishTarget_) return command;
    ReplaceAll(command, "{id}", std::to_string(punishTarget_->playerId));
    // {name} — ник цели (офф-бан: /soffban {name} {reason}).
    ReplaceAll(command, "{name}", punishTarget_->playerName);
    ReplaceAll(command, "{time}", std::to_string(punishMinutes_));
    ReplaceAll(command, "{reason}",
               Trimmed(std::string_view(punishReasonBuffer_.data())));
    return command;
}

void ReportPanelUi::RenderPunishComposer() {
    if (!punishTarget_) {
        // Панель могли спрятать без кадра ImGui — убираем только свой попап.
        CloseStalePopupsByPrefix("Наказание###punish_compose_");
        return;
    }
    const std::string popupId = "Наказание###punish_compose_" +
        std::to_string(punishSessionId_);
    const char* const popupLabel = popupId.c_str();

    if (punishPopupRequested_) {
        ImGui::OpenPopup(popupLabel);
        punishPopupRequested_ = false;
    } else if (!ImGui::IsPopupOpen(popupLabel)) {
        CancelPunishComposer();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(470.0f, 520.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(popupLabel, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushID(static_cast<int>(punishSessionId_));
        ImGui::Text("Наказание: %s [ID %u]", punishTarget_->playerName.c_str(),
                    punishTarget_->playerId);

        if (toolkit_.forms.empty()) {
            ImGui::TextDisabled("Формы наказаний не найдены — заполни forms.json.");
        } else {
            const int formIndex = std::clamp(punishFormIndex_, 0,
                static_cast<int>(toolkit_.forms.size()) - 1);
            const PunishForm* form = &toolkit_.forms[static_cast<std::size_t>(formIndex)];

            // Выбор наказания — сетка 3 колонки (попап Combo в оверлее не работает).
            const float formSpacing = ImGui::GetStyle().ItemSpacing.x;
            const float formBtnWidth =
                (ImGui::GetContentRegionAvail().x - formSpacing * 2.0f) / 3.0f;
            for (std::size_t i = 0; i < toolkit_.forms.size(); ++i) {
                if (i % 3 != 0) ImGui::SameLine(0.0f, formSpacing);
                const bool active = (form == &toolkit_.forms[i]);
                ImGui::PushID(static_cast<int>(i));
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, Accent(settings_, 0.95f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(settings_, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(settings_, 1.0f));
                }
                if (ImGui::Button(toolkit_.forms[i].label.c_str(),
                                  ImVec2(formBtnWidth, 0.0f))) {
                    punishFormIndex_ = static_cast<int>(i);
                    punishMinutes_ = toolkit_.forms[i].defaultTime;
                }
                if (active) ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            bool enterPressed = false;
            if (form->needsTime) {
                ImGui::TextDisabled("Время (минуты):");
                static constexpr int presets[] = {10, 30, 60, 120, 180};
                for (const int minutes : presets) {
                    if (minutes != presets[0]) ImGui::SameLine(0.0f, 6.0f);
                    char label[16]{};
                    std::snprintf(label, sizeof(label), "%dм", minutes);
                    ImGui::PushID(minutes);
                    if (ImGui::SmallButton(label)) punishMinutes_ = minutes;
                    ImGui::PopID();
                }
                int minutesInput = punishMinutes_;
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::InputInt("##punish_time", &minutesInput, 0, 0)) {
                    punishMinutes_ = std::clamp(minutesInput, 1, 10080);
                }
            }
            if (form->needsReason) {
                // Пресеты причин из forms.json (клик подставляет причину
                // и время, если у пресета оно задано).
                if (!form->reasons.empty()) {
                    ImGui::TextDisabled("Готовые причины (клик — подставит и время):");
                    ImGui::BeginChild("##punish_reasons", ImVec2(0.0f, 118.0f), true);
                    bool lineHasItem = false;
                    for (std::size_t i = 0; i < form->reasons.size(); ++i) {
                        const auto& preset = form->reasons[i];
                        char label[256]{};
                        if (form->needsTime && preset.time > 0) {
                            std::snprintf(label, sizeof(label), "%s · %dм",
                                          preset.reason.c_str(), preset.time);
                        } else {
                            std::snprintf(label, sizeof(label), "%s",
                                          preset.reason.c_str());
                        }
                        const float buttonWidth =
                            ImGui::CalcTextSize(label).x +
                            ImGui::GetStyle().FramePadding.x * 2.0f;
                        if (lineHasItem && buttonWidth <=
                            ImGui::GetContentRegionAvail().x) {
                            ImGui::SameLine(0.0f, 6.0f);
                        }
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::SmallButton(label)) {
                            CopyToBuffer(punishReasonBuffer_, preset.reason);
                            if (form->needsTime && preset.time > 0) {
                                punishMinutes_ = preset.time;
                            }
                        }
                        ImGui::PopID();
                        lineHasItem = true;
                    }
                    ImGui::EndChild();
                }
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##punish_reason", "Причина наказания…",
                                         punishReasonBuffer_.data(),
                                         punishReasonBuffer_.size());
                enterPressed = ImGui::IsItemFocused() &&
                    ImGui::IsKeyPressed(ImGuiKey_Enter, false) &&
                    !ImGui::GetIO().KeyShift;
            }

            const std::string preview = BuildPunishCommand(*form);
            ImGui::TextDisabled("Будет отправлено: %s", preview.c_str());

            const auto send = [&] {
                if (form->needsReason &&
                    Trimmed(std::string_view(punishReasonBuffer_.data())).empty()) {
                    ShowToast("Укажи причину наказания.");
                    return;
                }
                const auto result = game_.SendCommand(BuildPunishCommand(*form));
                if (!result.succeeded) {
                    ShowToast(result.message);
                    return;
                }
                ++punishCount_;
                const bool startTimer = form->createsTimer && form->needsTime &&
                    punishMinutes_ > 0;
                if (startTimer) {
                    punishTimers_.push_back(PunishTimer{
                        punishTarget_->playerName, punishTarget_->playerId,
                        form->label,
                        std::chrono::steady_clock::now() +
                            std::chrono::minutes(punishMinutes_)});
                }
                const char* timerNote = startTimer ? " (таймер заведён)" : "";
                if (reports_.MarkAnswered(punishTarget_->localId)) {
                    ShowToast(std::string("Наказание отправлено, репорт отмечен отвеченным") +
                              timerNote + ".");
                } else {
                    ShowToast(std::string("Наказание отправлено") + timerNote + ".");
                }
                CancelPunishComposer();
                ImGui::CloseCurrentPopup();
            };

            if (enterPressed) {
                send();
            } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                       ImGui::IsKeyPressed(ImGuiKey_Enter, false) &&
                       !ImGui::GetIO().KeyShift) {
                send();
            }

            ImGui::Separator();
            if (ImGui::Button("Отправить", ImVec2(150.0f, 32.0f))) send();
            ImGui::SameLine(0.0f, 8.0f);
            if (ImGui::Button("Отмена", ImVec2(95.0f, 32.0f))) {
                CancelPunishComposer();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (!open) CancelPunishComposer();
}

void ReportPanelUi::RenderTimersWindow() {
    const char* const popupLabel = "Таймеры наказаний###timers";
    if (timersPopupRequested_) {
        ImGui::OpenPopup(popupLabel);
        timersPopupRequested_ = false;
    }
    if (!timersOpen_ || punishTimers_.empty()) {
        CloseStalePopupsByPrefix("Таймеры наказаний###timers");
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(popupLabel, &open,
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto now = std::chrono::steady_clock::now();
        std::size_t index = 0;
        for (auto it = punishTimers_.begin(); it != punishTimers_.end(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                it->expiresAt - now).count();
            const int minutesLeft = static_cast<int>(remaining / 60);
            const int secondsLeft = static_cast<int>(remaining % 60);
            ImGui::Text("%s · %s [ID %u] · осталось %d:%02d",
                        it->label.c_str(), it->playerName.c_str(), it->playerId,
                        minutesLeft, secondsLeft);
            ImGui::SameLine();
            if (ImGui::SmallButton("Снять")) {
                it = punishTimers_.erase(it);
                ImGui::PopID();
                continue;
            }
            ImGui::PopID();
            ++it;
        }
        ImGui::EndPopup();
    } else {
        timersOpen_ = false;
    }
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
    RenderPunishComposer();
    RenderTimersWindow();
    RenderNotesWindow();
    RenderCommandsWindow();
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
    
    // ИСПРАВЛЕНИЕ: Убрали ImGui::IsAnyItemActive(), чтобы флаг не включался при перетаскивании
    g_panelTextInputActive.store(io.WantTextInput || replyTarget_.has_value() ||
        punishTarget_.has_value(), std::memory_order_relaxed);
    
    ImGui::End();
}

} // namespace reportpanel