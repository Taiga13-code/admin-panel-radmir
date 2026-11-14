#pragma once

#include "../core/IReportSource.h"
#include "../parser/ReportParser.h"
#include "../platform/Encoding.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace reportpanel {

class ChatlogReader final : public IReportSource {
public:
    using EventHandler = std::function<void(const ParsedEvent&)>;

    ChatlogReader(std::filesystem::path chatlogPath,
                  std::filesystem::path cursorPath,
                  EventHandler handler,
                  bool replayRecentOnStart = false);
    ~ChatlogReader() override;

    void Start() override;
    void Stop() noexcept override;
    void SetPath(std::filesystem::path path);
    void SetAdminNick(std::string nick);
    [[nodiscard]] std::string Status() const override;

private:
    struct Cursor {
        std::string path;
        std::string fileIdentity;
        std::uint64_t generation{0};
        std::uint64_t offset{0};
        std::uint64_t checkpointHash{0};
        std::uint64_t checkpointBytes{0};
    };

    void Run();
    bool ReadAvailable(HANDLE file, const std::filesystem::path& path,
                       std::uint64_t fileSize, const std::string& identity);
    bool InitializeFile(HANDLE file, const std::filesystem::path& path,
                        std::uint64_t fileSize, const std::string& identity);
    void SaveCursor(HANDLE file, const std::filesystem::path& path,
                    const std::string& identity, std::uint64_t fileSize);
    [[nodiscard]] bool VerifyCheckpoint(HANDLE file, std::uint64_t offset) const;
    [[nodiscard]] std::size_t FindLineEnd() const;
    [[nodiscard]] std::size_t DelimiterLength() const;
    void ProcessLine(std::string line, std::uint64_t lineOffset,
                     const std::filesystem::path& path,
                     const std::string& identity);
    void SetStatus(std::string status);
    [[nodiscard]] std::filesystem::path CurrentPath() const;

    std::filesystem::path cursorPath_;
    EventHandler handler_;
    ReportParser parser_;

    mutable std::mutex mutex_;
    std::filesystem::path chatlogPath_;
    bool replayRecentOnStart_{false};
    std::string adminNick_;
    std::string status_{"Ожидание chatlog"};
    std::atomic<bool> running_{false};
    std::thread worker_;

    Cursor cursor_;
    bool cursorLoaded_{false};
    bool fileInitialized_{false};
    bool initialScanComplete_{false};
    bool cursorNeedsSave_{false};
    std::atomic<bool> replayRecentRequested_{false};
    TextEncoding encoding_{TextEncoding::Detect};
    std::uint64_t nextReadOffset_{0};
    std::uint64_t pendingStartOffset_{0};
    std::uint64_t committedOffset_{0};
    std::string pendingBytes_;
};

} // namespace reportpanel
