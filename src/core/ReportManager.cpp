#include "ReportManager.h"
#include "../settings/JsonFile.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace reportpanel {
namespace {

constexpr std::size_t kAnswerKeyLimit = 12000;
constexpr std::size_t kAnswerKeyRetain = 10000;

const char* ToString(ReportStatus status) {
    switch (status) {
    case ReportStatus::Answered: return "answered";
    case ReportStatus::Blocked: return "blocked";
    default: return "unanswered";
    }
}

ReportStatus StatusFromString(const std::string& value) {
    if (value == "answered") return ReportStatus::Answered;
    if (value == "blocked") return ReportStatus::Blocked;
    return ReportStatus::Unanswered;
}

bool SamePlayer(const Report& report, const ParsedEvent& event) {
    if (event.playerId && report.playerId == *event.playerId) return true;
    if (event.playerName.empty() || report.playerName.size() != event.playerName.size()) return false;
    for (std::size_t i = 0; i < report.playerName.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(report.playerName[i])) !=
            std::tolower(static_cast<unsigned char>(event.playerName[i]))) return false;
    }
    return true;
}

std::string MoscowDayKey() {
    const auto shifted = std::chrono::system_clock::now() + std::chrono::hours(3);
    const std::time_t value = std::chrono::system_clock::to_time_t(shifted);
    std::tm date{};
    gmtime_s(&date, &value);
    std::ostringstream result;
    result << std::put_time(&date, "%Y-%m-%d");
    return result.str();
}

} // namespace

ReportManager::ReportManager(std::filesystem::path storePath)
    : storePath_(std::move(storePath)) {
    Load();
    unansweredCount_ = static_cast<std::size_t>(std::count_if(reports_.begin(), reports_.end(),
        [](const Report& report) { return report.status == ReportStatus::Unanswered; }));
    snapshot_ = std::make_shared<const std::vector<Report>>(reports_);
    persistenceThread_ = std::thread(&ReportManager::PersistenceLoop, this);
}

ReportManager::~ReportManager() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    saveCondition_.notify_one();
    if (persistenceThread_.joinable()) persistenceThread_.join();
    (void)Save();
}

void ReportManager::Load() {
    try {
        const auto data = ReadJsonFile(storePath_);
        if (!data.is_object()) return;
        adminReplyDay_ = data.value("adminReplyDay", std::string{});
        adminRepliesToday_ = data.value("adminRepliesToday", std::size_t{0});
        if (data.contains("adminReplyKeys") && data["adminReplyKeys"].is_array()) {
            for (const auto& item : data["adminReplyKeys"]) {
                if (item.is_string()) adminReplyKeys_.insert(item.get<std::string>());
            }
        }
        if (data.contains("answerKeys") && data["answerKeys"].is_array()) {
            for (const auto& item : data["answerKeys"]) {
                if (!item.is_string()) continue;
                const auto key = item.get<std::string>();
                if (key.empty()) continue;
                if (answerKeys_.insert(key).second)
                    answerKeyOrder_.push_back(key);
            }
        }
        if (adminReplyDay_ != MoscowDayKey()) {
            adminReplyDay_ = MoscowDayKey();
            adminRepliesToday_ = 0;
            adminReplyKeys_.clear();
        }
        if (!data.contains("reports") || !data["reports"].is_array()) return;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& item : data["reports"]) {
            if (!item.is_object()) continue;
            Report report;
            report.localId = item.value("localId", std::uint64_t{0});
            report.playerId = item.value("playerId", std::uint32_t{0});
            report.playerName = item.value("playerName", std::string{});
            report.message = item.value("message", std::string{});
            report.timeText = item.value("timeText", std::string{});
            report.status = StatusFromString(item.value("status", std::string{}));
            report.sourceKey = item.value("sourceKey", std::string{});
            report.highlightedUntil = now;
            report.playerDisconnected = item.value("playerDisconnected", false);
            if (report.localId == 0 || report.sourceKey.empty()) continue;
            seenKeys_.insert(report.sourceKey);
            nextId_ = std::max(nextId_, report.localId + 1);
            reports_.push_back(std::move(report));
        }
        std::sort(reports_.begin(), reports_.end(),
                  [](const Report& a, const Report& b) { return a.localId < b.localId; });
        if (reports_.size() > 5000) {
            const auto removeCount = reports_.size() - 5000;
            reports_.erase(reports_.begin(), reports_.begin() + static_cast<std::ptrdiff_t>(removeCount));
            seenKeys_.clear();
            for (const auto& report : reports_) seenKeys_.insert(report.sourceKey);
        }
    } catch (...) {
        reports_.clear();
        seenKeys_.clear();
        nextId_ = 1;
    }
}

void ReportManager::Apply(const ParsedEvent& event) {
    std::scoped_lock lock(mutex_);
    bool countChanged = false;
    if (event.kind == ReportEventKind::AdminReply || event.countsAsAdminReply) {
        const auto today = MoscowDayKey();
        if (adminReplyDay_ != today) {
            adminReplyDay_ = today;
            adminRepliesToday_ = 0;
            adminReplyKeys_.clear();
            countChanged = true;
        }
        if (!event.eventKey.empty() && !adminReplyKeys_.contains(event.eventKey)) {
            adminReplyKeys_.insert(event.eventKey);
            ++adminRepliesToday_;
            countChanged = true;
        }
    }
    if (event.kind == ReportEventKind::AdminReply) {
        if (!countChanged) return;
        dirty_ = true;
        ++revision_;
        lastMutation_ = std::chrono::steady_clock::now();
        saveCondition_.notify_one();
        return;
    }

    bool changed = false;
    bool answerRecorded = false;
    if (event.kind == ReportEventKind::PlayerDisconnected) {
        bool anyMarked = false;
        for (auto it = reports_.rbegin(); it != reports_.rend(); ++it) {
            if (it->status == ReportStatus::Unanswered && SamePlayer(*it, event)) {
                it->playerDisconnected = true;
                anyMarked = true;
            }
        }
        if (!anyMarked) return;
        changed = true;
    } else if (event.kind == ReportEventKind::NewReport) {
        if (event.eventKey.empty() || seenKeys_.contains(event.eventKey) || !event.playerId) return;
        Report report;
        report.localId = nextId_++;
        report.playerId = *event.playerId;
        report.playerName = event.playerName;
        report.message = event.message;
        report.timeText = event.timeText;
        report.status = ReportStatus::Unanswered;
        report.sourceKey = event.eventKey;
        const auto now = std::chrono::steady_clock::now();
        report.highlightedUntil = event.isHistorical
            ? now : now + std::chrono::seconds(9);
        seenKeys_.insert(report.sourceKey);
        reports_.push_back(std::move(report));
        changed = true;
    } else {
        const bool alreadyProcessed = !event.eventKey.empty() &&
            (seenKeys_.contains(event.eventKey) ||
             answerKeys_.contains(event.eventKey));
        if (!alreadyProcessed) {
            for (auto it = reports_.rbegin(); it != reports_.rend(); ++it) {
                if (it->status == ReportStatus::Unanswered && SamePlayer(*it, event)) {
                    it->status = event.kind == ReportEventKind::Blocked
                        ? ReportStatus::Blocked : ReportStatus::Answered;
                    changed = true;
                    break;
                }
            }
            if (!event.eventKey.empty()) {
                // Remember the line even when it matched no report: a replayed
                // tail must not apply this answer to a later report later on.
                answerKeys_.insert(event.eventKey);
                answerKeyOrder_.push_back(event.eventKey);
                answerRecorded = true;
                if (answerKeyOrder_.size() > kAnswerKeyLimit) {
                    const auto removeCount =
                        answerKeyOrder_.size() - kAnswerKeyRetain;
                    for (std::size_t index = 0; index < removeCount; ++index) {
                        answerKeys_.erase(answerKeyOrder_.front());
                        answerKeyOrder_.pop_front();
                    }
                }
            }
        }
    }
    if (!changed && !countChanged && !answerRecorded) return;
    if (changed) {
        if (reports_.size() > 5000) {
            const auto removeCount = reports_.size() - 5000;
            reports_.erase(reports_.begin(), reports_.begin() + static_cast<std::ptrdiff_t>(removeCount));
            seenKeys_.clear();
            for (const auto& report : reports_) seenKeys_.insert(report.sourceKey);
        }
        unansweredCount_ = static_cast<std::size_t>(std::count_if(reports_.begin(), reports_.end(),
            [](const Report& report) { return report.status == ReportStatus::Unanswered; }));
        snapshot_ = std::make_shared<const std::vector<Report>>(reports_);
    }
    dirty_ = true;
    ++revision_;
    lastMutation_ = std::chrono::steady_clock::now();
    saveCondition_.notify_one();
}

bool ReportManager::MarkAnswered(std::uint64_t localId) {
    std::scoped_lock lock(mutex_);
    for (auto& report : reports_) {
        if (report.localId == localId && report.status == ReportStatus::Unanswered) {
            report.status = ReportStatus::Answered;
            if (unansweredCount_ > 0) --unansweredCount_;
            snapshot_ = std::make_shared<const std::vector<Report>>(reports_);
            dirty_ = true;
            ++revision_;
            lastMutation_ = std::chrono::steady_clock::now();
            saveCondition_.notify_one();
            return true;
        }
    }
    return false;
}

void ReportManager::ClearAll() {
    std::scoped_lock lock(mutex_);
    reports_.clear();
    seenKeys_.clear();
    nextId_ = 1;
    unansweredCount_ = 0;
    snapshot_ = std::make_shared<const std::vector<Report>>();
    dirty_ = true;
    ++revision_;
    lastMutation_ = std::chrono::steady_clock::now();
    saveCondition_.notify_one();
}

std::shared_ptr<const std::vector<Report>> ReportManager::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

std::size_t ReportManager::UnansweredCount() const {
    std::scoped_lock lock(mutex_);
    return unansweredCount_;
}

std::size_t ReportManager::AdminRepliesToday() const {
    std::scoped_lock lock(mutex_);
    return adminReplyDay_ == MoscowDayKey() ? adminRepliesToday_ : 0;
}

bool ReportManager::Save() {
    std::vector<Report> snapshot;
    std::vector<std::string> adminReplyKeys;
    std::vector<std::string> answerKeys;
    std::string adminReplyDay;
    std::size_t adminRepliesToday = 0;
    std::uint64_t revision = 0;
    {
        std::scoped_lock lock(mutex_);
        if (!dirty_ && !reports_.empty()) return true;
        snapshot = reports_;
        adminReplyKeys.assign(adminReplyKeys_.begin(), adminReplyKeys_.end());
        answerKeys.assign(answerKeyOrder_.begin(), answerKeyOrder_.end());
        adminReplyDay = adminReplyDay_;
        adminRepliesToday = adminRepliesToday_;
        revision = revision_;
    }

    std::scoped_lock persistenceLock(persistenceMutex_);
    nlohmann::json data;
    data["reports"] = nlohmann::json::array();
    data["adminReplyDay"] = adminReplyDay;
    data["adminRepliesToday"] = adminRepliesToday;
    data["adminReplyKeys"] = nlohmann::json::array();
    for (const auto& key : adminReplyKeys) data["adminReplyKeys"].push_back(key);
    data["answerKeys"] = nlohmann::json::array();
    for (const auto& key : answerKeys) data["answerKeys"].push_back(key);
    for (const auto& report : snapshot) {
        data["reports"].push_back({
            {"localId", report.localId},
            {"playerId", report.playerId},
            {"playerName", report.playerName},
            {"message", report.message},
            {"timeText", report.timeText},
            {"status", ToString(report.status)},
            {"sourceKey", report.sourceKey},
            {"playerDisconnected", report.playerDisconnected}
        });
    }
    try {
        WriteJsonFileAtomic(storePath_, data);
        std::scoped_lock lock(mutex_);
        if (revision_ == revision) dirty_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

void ReportManager::PersistenceLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        saveCondition_.wait(lock, [this] { return stopping_ || dirty_; });
        if (stopping_) break;

        const auto due = lastMutation_ + std::chrono::milliseconds(750);
        const bool changedAgain = saveCondition_.wait_until(lock, due, [this, due] {
            return stopping_ || lastMutation_ + std::chrono::milliseconds(750) > due;
        });
        if (stopping_) break;
        if (changedAgain) continue;

        lock.unlock();
        const bool saved = Save();
        lock.lock();
        if (!saved && !stopping_) {
            saveCondition_.wait_for(lock, std::chrono::seconds(5),
                                    [this] { return stopping_; });
            lastMutation_ = std::chrono::steady_clock::now();
        }
    }
}

} // namespace reportpanel
