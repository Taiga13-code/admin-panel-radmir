#pragma once

#include "Report.h"

#include <filesystem>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <memory>
#include <cstddef>
#include <deque>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace reportpanel {

class ReportManager {
public:
    explicit ReportManager(std::filesystem::path storePath);
    ~ReportManager();

    void Apply(const ParsedEvent& event);
    bool MarkAnswered(std::uint64_t localId);
    void ClearAll();
    [[nodiscard]] std::shared_ptr<const std::vector<Report>> Snapshot() const;
    [[nodiscard]] std::size_t UnansweredCount() const;
    [[nodiscard]] std::size_t AdminRepliesToday() const;
    bool Save();

private:
    void Load();

    std::filesystem::path storePath_;
    mutable std::mutex mutex_;
    std::vector<Report> reports_;
    std::shared_ptr<const std::vector<Report>> snapshot_;
    std::unordered_set<std::string> seenKeys_;
    // Answer/block lines already applied. Kept separately from report keys so
    // a replayed chatlog tail cannot mark a newer report from the same player
    // as answered.
    std::unordered_set<std::string> answerKeys_;
    std::deque<std::string> answerKeyOrder_;
    std::unordered_set<std::string> adminReplyKeys_;
    std::string adminReplyDay_;
    std::size_t adminRepliesToday_{0};
    std::uint64_t nextId_{1};
    std::size_t unansweredCount_{0};
    std::uint64_t revision_{0};
    bool dirty_{false};
    bool stopping_{false};
    std::chrono::steady_clock::time_point lastMutation_{};
    std::condition_variable saveCondition_;
    std::mutex persistenceMutex_;
    std::thread persistenceThread_;

    void PersistenceLoop();
};

} // namespace reportpanel
