#pragma once

#include "../core/IReportSource.h"
#include "../core/ReportManager.h"
#include "../game/IGameIntegration.h"
#include "../settings/Settings.h"

#include <Windows.h>
#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace reportpanel {

class ReportPanelUi {
public:
    ReportPanelUi(ReportManager& reports, IReportSource& source,
                  IGameIntegration& game, AppSettings& settings,
                  HWND hostWindow,
                  std::function<void()> settingsChanged,
                  std::function<void(const std::filesystem::path&)> sourcePathChanged,
                  std::function<void(std::string)> adminNickChanged,
                  std::function<void()> closePanel);
    ~ReportPanelUi();

    void Render();
    void OnPanelVisibility(bool visible);
    [[nodiscard]] bool SettingsOpen() const { return settingsOpen_; }

private:
    enum class Filter {
        All,
        Unanswered,
        Answered
    };

    void RenderHeader();
    void RenderFilters();
    void RenderReportList(float availableWidth, float availableHeight);
    void RenderReplyComposer();
    void RenderSettings(float width, float height);
    void ShowToast(std::string message);
    void ApplyPath(const std::filesystem::path& path);
    void ApplyAdminNick();
    void CancelReplyComposer();
    [[nodiscard]] bool Matches(const Report& report) const;
    [[nodiscard]] bool IsAnswered(const Report& report) const;

    ReportManager& reports_;
    IReportSource& source_;
    IGameIntegration& game_;
    AppSettings& settings_;
    HWND hostWindow_{nullptr};
    std::function<void()> settingsChanged_;
    std::function<void(const std::filesystem::path&)> sourcePathChanged_;
    std::function<void(std::string)> adminNickChanged_;
    std::function<void()> closePanel_;
    std::array<char, 512> searchBuffer_{};
    std::array<char, 2048> pathBuffer_{};
    std::array<char, 64> adminNickBuffer_{};
    std::array<char, 1024> replyBuffer_{};
    std::optional<Report> replyTarget_;
    std::vector<const Report*> filteredReports_;
    std::shared_ptr<const std::vector<Report>> cachedSnapshot_;
    std::string cachedSearch_;
    Filter cachedFilter_{Filter::All};
    Filter filter_{Filter::All};
    bool settingsOpen_{false};
    bool moveMode_{false};
    bool replyPopupRequested_{false};
    bool replyInputNeedsFocus_{false};
    bool replyInputWasActive_{false};
    std::uint64_t replySessionId_{0};
    std::string toast_;
    std::chrono::steady_clock::time_point toastUntil_{};
    std::chrono::steady_clock::time_point lastSpectateClickTime_{std::chrono::steady_clock::time_point::min()};
    bool followDisabled_{false};
    static constexpr std::chrono::milliseconds spectateCooldownMs_{800};
};

} // namespace reportpanel
