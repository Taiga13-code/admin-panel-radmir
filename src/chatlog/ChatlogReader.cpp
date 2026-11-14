#include "ChatlogReader.h"

#include "../platform/Encoding.h"
#include "../settings/JsonFile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>

namespace reportpanel {
namespace {

bool ReadAt(HANDLE file, std::uint64_t offset, void* buffer,
            DWORD requested, DWORD& readBytes) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    readBytes = 0;
    return ReadFile(file, buffer, requested, &readBytes, nullptr) != FALSE;
}

std::uint64_t HashBytes(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : bytes) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
           static_cast<std::uint64_t>(info.nFileSizeLow);
}

std::string FileIdentity(const BY_HANDLE_FILE_INFORMATION& info) {
    const auto index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
                       static_cast<std::uint64_t>(info.nFileIndexLow);
    return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(index);
}

TextEncoding DetectEncoding(HANDLE file) {
    std::array<unsigned char, 4> header{};
    DWORD count = 0;
    if (!ReadAt(file, 0, header.data(), static_cast<DWORD>(header.size()), count)) {
        return TextEncoding::Detect;
    }
    if (count >= 2 && header[0] == 0xFF && header[1] == 0xFE)
        return TextEncoding::Utf16LittleEndian;
    if (count >= 2 && header[0] == 0xFE && header[1] == 0xFF)
        return TextEncoding::Utf16BigEndian;
    if (count >= 3 && header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF)
        return TextEncoding::Utf8;
    return TextEncoding::Detect;
}

std::uint64_t RecentWindowStart(HANDLE file, std::uint64_t fileSize,
                                TextEncoding encoding) {
    constexpr std::uint64_t replayWindowBytes = 256 * 1024;
    const auto start = fileSize > replayWindowBytes ? fileSize - replayWindowBytes : 0;
    if (start == 0) return 0;

    const auto length = static_cast<DWORD>(fileSize - start);
    std::string tail(length, '\0');
    DWORD readBytes = 0;
    if (!ReadAt(file, start, tail.data(), length, readBytes) || readBytes != length) {
        return fileSize;
    }

    std::size_t delimiter = std::string::npos;
    std::size_t delimiterBytes = 1;
    if (encoding == TextEncoding::Utf16LittleEndian) {
        delimiter = tail.find(std::string("\n\0", 2));
        delimiterBytes = 2;
    } else if (encoding == TextEncoding::Utf16BigEndian) {
        delimiter = tail.find(std::string("\0\n", 2));
        delimiterBytes = 2;
    } else {
        delimiter = tail.find('\n');
    }
    return delimiter == std::string::npos
        ? fileSize : start + delimiter + delimiterBytes;
}

bool IsRecentTimestamp(std::string_view value) {
    if (value.size() != 8 || value[2] != ':' || value[5] != ':') return false;
    const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != 2 && i != 5 && !digit(value[i])) return false;
    }
    const int hour = (value[0] - '0') * 10 + (value[1] - '0');
    const int minute = (value[3] - '0') * 10 + (value[4] - '0');
    const int second = (value[6] - '0') * 10 + (value[7] - '0');
    if (hour > 23 || minute > 59 || second > 59) return false;

    SYSTEMTIME local{};
    GetLocalTime(&local);
    const int nowSeconds = static_cast<int>(local.wHour) * 3600 +
        static_cast<int>(local.wMinute) * 60 + static_cast<int>(local.wSecond);
    const int lineSeconds = hour * 3600 + minute * 60 + second;
    const int age = (nowSeconds - lineSeconds + 24 * 3600) % (24 * 3600);
    return age <= 15 * 60;
}

} // namespace

ChatlogReader::ChatlogReader(std::filesystem::path chatlogPath,
                             std::filesystem::path cursorPath,
                             EventHandler handler,
                             bool replayRecentOnStart)
    : cursorPath_(std::move(cursorPath)),
      handler_(std::move(handler)),
      chatlogPath_(std::move(chatlogPath)),
      replayRecentOnStart_(replayRecentOnStart) {
    try {
        const auto saved = ReadJsonFile(cursorPath_);
        if (saved.is_object()) {
            cursor_.path = saved.value("path", std::string{});
            cursor_.fileIdentity = saved.value("fileIdentity", std::string{});
            cursor_.generation = saved.value("generation", std::uint64_t{0});
            cursor_.offset = saved.value("offset", std::uint64_t{0});
            cursor_.checkpointHash = saved.value("checkpointHash", std::uint64_t{0});
            cursor_.checkpointBytes = saved.value("checkpointBytes", std::uint64_t{0});
            cursorLoaded_ = !cursor_.path.empty();
        }
    } catch (...) {
        cursor_ = {};
        cursorLoaded_ = false;
    }
}

ChatlogReader::~ChatlogReader() {
    Stop();
}

void ChatlogReader::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread(&ChatlogReader::Run, this);
}

void ChatlogReader::Stop() noexcept {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void ChatlogReader::SetPath(std::filesystem::path path) {
    {
        std::scoped_lock lock(mutex_);
        if (path == chatlogPath_) return;
        chatlogPath_ = std::move(path);
    }
    SetStatus("Путь к chatlog изменён");
}

void ChatlogReader::SetAdminNick(std::string nick) {
    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        changed = adminNick_ != nick;
        adminNick_ = std::move(nick);
    }
    // A nick may be configured after the reader has already passed its lines.
    // Re-scan only a bounded recent tail so its reply counter can catch up.
    if (changed && running_.load(std::memory_order_relaxed)) {
        replayRecentRequested_.store(true, std::memory_order_relaxed);
    }
}

std::string ChatlogReader::Status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

std::filesystem::path ChatlogReader::CurrentPath() const {
    std::scoped_lock lock(mutex_);
    return chatlogPath_;
}

void ChatlogReader::SetStatus(std::string status) {
    std::scoped_lock lock(mutex_);
    status_ = std::move(status);
}

std::size_t ChatlogReader::FindLineEnd() const {
    if (encoding_ == TextEncoding::Utf16LittleEndian)
        return pendingBytes_.find(std::string("\n\0", 2));
    if (encoding_ == TextEncoding::Utf16BigEndian)
        return pendingBytes_.find(std::string("\0\n", 2));
    return pendingBytes_.find('\n');
}

std::size_t ChatlogReader::DelimiterLength() const {
    return (encoding_ == TextEncoding::Utf16LittleEndian ||
            encoding_ == TextEncoding::Utf16BigEndian) ? 2 : 1;
}

bool ChatlogReader::VerifyCheckpoint(HANDLE file, std::uint64_t offset) const {
    if (cursor_.checkpointBytes == 0) return true;
    const auto start = offset >= cursor_.checkpointBytes
        ? offset - cursor_.checkpointBytes : 0;
    const auto bytesToRead = static_cast<DWORD>(std::min<std::uint64_t>(
        cursor_.checkpointBytes, offset - start));
    std::string sample(bytesToRead, '\0');
    DWORD readBytes = 0;
    if (!ReadAt(file, start, sample.data(), bytesToRead, readBytes) ||
        readBytes != bytesToRead) return false;
    return HashBytes(sample) == cursor_.checkpointHash;
}

bool ChatlogReader::InitializeFile(HANDLE file, const std::filesystem::path& path,
                                   std::uint64_t fileSize,
                                   const std::string& identity) {
    const auto pathKey = WideToUtf8(path.lexically_normal().wstring());
    encoding_ = DetectEncoding(file);
    bool resume = cursorLoaded_ && cursor_.path == pathKey &&
                  cursor_.fileIdentity == identity &&
                  cursor_.offset <= fileSize &&
                  VerifyCheckpoint(file, cursor_.offset);
    const bool replayRecent = replayRecentOnStart_ && !initialScanComplete_;

    if (!resume) {
        const bool sameSource = cursorLoaded_ && cursor_.path == pathKey;
        const auto generation = sameSource ? cursor_.generation + 1 : 0;
        cursor_ = {};
        cursor_.path = pathKey;
        cursor_.fileIdentity = identity;
        cursor_.generation = generation;
        // Replay a bounded recent tail only during a still-valid session; once the
        // retention window expires, start at EOF instead of importing old history.
        cursor_.offset = replayRecent
            ? RecentWindowStart(file, fileSize, encoding_) : fileSize;
        cursorNeedsSave_ = true;
    } else if (replayRecent) {
        // Recover lines that arrived shortly before the ASI finished attaching.
        // Existing events are de-duplicated by their path/identity/offset key.
        cursor_.offset = RecentWindowStart(file, fileSize, encoding_);
        cursor_.checkpointBytes = 0;
        cursor_.checkpointHash = 0;
        cursorNeedsSave_ = true;
    }

    pendingBytes_.clear();
    nextReadOffset_ = cursor_.offset;
    pendingStartOffset_ = cursor_.offset;
    committedOffset_ = cursor_.offset;
    fileInitialized_ = true;
    cursorLoaded_ = true;
    return true;
}

void ChatlogReader::ProcessLine(std::string line, std::uint64_t lineOffset,
                                const std::filesystem::path& path,
                                const std::string& identity) {
    auto decoded = DecodeLineToUtf8(line, encoding_);
    if (decoded.empty()) return;
    if (!decoded.empty() && decoded.back() == '\r') decoded.pop_back();

    const auto sourcePath = WideToUtf8(path.lexically_normal().wstring());
    // The offset alone is not stable when the chatlog is rewritten in place:
    // a different line can occupy the same offset. Mixing in the line content
    // keeps replay keys stable for an unchanged line while still producing a
    // new key if the content actually changed.
    const auto key = sourcePath + "|" + identity + "|" +
                     std::to_string(cursor_.generation) + "|" +
                     std::to_string(lineOffset) + "|" +
                     std::to_string(HashBytes(decoded));
    std::string adminNick;
    {
        std::scoped_lock lock(mutex_);
        adminNick = adminNick_;
    }
    if (const auto event = parser_.Parse(decoded, key, adminNick)) {
        if (replayRecentOnStart_ && !initialScanComplete_ &&
            !IsRecentTimestamp(event->timeText)) return;
        try {
            auto annotated = *event;
            annotated.isHistorical = !initialScanComplete_;
            if (handler_) handler_(annotated);
        } catch (...) {
        }
    }
}

void ChatlogReader::SaveCursor(HANDLE file, const std::filesystem::path& path,
                               const std::string& identity,
                               std::uint64_t fileSize) {
    const auto offset = committedOffset_;
    const auto pathKey = WideToUtf8(path.lexically_normal().wstring());
    if (!cursorNeedsSave_ && offset == cursor_.offset &&
        identity == cursor_.fileIdentity && pathKey == cursor_.path) return;
    const auto sampleBytes = std::min<std::uint64_t>(offset, 128);
    const auto sampleStart = offset - sampleBytes;
    std::string sample(static_cast<std::size_t>(sampleBytes), '\0');
    DWORD readBytes = 0;
    if (sampleBytes != 0 &&
        (!ReadAt(file, sampleStart, sample.data(),
                 static_cast<DWORD>(sample.size()), readBytes) ||
         readBytes != sample.size())) {
        return;
    }

    cursor_.path = pathKey;
    cursor_.fileIdentity = identity;
    cursor_.offset = offset;
    cursor_.checkpointBytes = sampleBytes;
    cursor_.checkpointHash = HashBytes(sample);

    nlohmann::json json;
    json["path"] = cursor_.path;
    json["fileIdentity"] = cursor_.fileIdentity;
    json["generation"] = cursor_.generation;
    json["offset"] = cursor_.offset;
    json["checkpointBytes"] = cursor_.checkpointBytes;
    json["checkpointHash"] = cursor_.checkpointHash;
    json["observedSize"] = fileSize;
    try {
        WriteJsonFileAtomic(cursorPath_, json);
        cursorNeedsSave_ = false;
    } catch (...) {
    }
}

bool ChatlogReader::ReadAvailable(HANDLE file, const std::filesystem::path& path,
                                  std::uint64_t fileSize,
                                  const std::string& identity) {
    if (replayRecentRequested_.exchange(false, std::memory_order_relaxed)) {
        replayRecentOnStart_ = true;
        fileInitialized_ = false;
        initialScanComplete_ = false;
    }
    if (!fileInitialized_ && !InitializeFile(file, path, fileSize, identity)) return false;

    if (cursor_.fileIdentity != identity || fileSize < nextReadOffset_ ||
        !VerifyCheckpoint(file, committedOffset_)) {
        const auto pathKey = WideToUtf8(path.lexically_normal().wstring());
        const bool sameSource = cursorLoaded_ && cursor_.path == pathKey;
        cursor_.generation = sameSource ? cursor_.generation + 1 : 0;
        cursor_.fileIdentity = identity;
        cursor_.path = pathKey;
        encoding_ = DetectEncoding(file);
        // Re-read from a line boundary in the new file instead of jumping to
        // EOF: if the chatlog was truncated and already received lines between
        // two polls, they would otherwise be lost. On a plain truncation the
        // window starts at zero and nothing is skipped.
        cursor_.offset = RecentWindowStart(file, fileSize, encoding_);
        cursor_.checkpointBytes = 0;
        cursor_.checkpointHash = 0;
        fileInitialized_ = false;
        initialScanComplete_ = false;
        cursorNeedsSave_ = true;
        if (!InitializeFile(file, path, fileSize, identity)) return false;
    }

    std::array<char, 65536> buffer{};
    while (nextReadOffset_ < fileSize) {
        const auto remaining = fileSize - nextReadOffset_;
        const DWORD request = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD count = 0;
        if (!ReadAt(file, nextReadOffset_, buffer.data(), request, count)) return false;
        if (count == 0) break;

        if (pendingBytes_.empty()) pendingStartOffset_ = nextReadOffset_;
        pendingBytes_.append(buffer.data(), count);
        nextReadOffset_ += count;

        while (true) {
            const auto end = FindLineEnd();
            if (end == std::string::npos) break;
            const auto delimiter = DelimiterLength();
            std::string line = pendingBytes_.substr(0, end);
            const auto lineOffset = pendingStartOffset_;
            pendingBytes_.erase(0, end + delimiter);
            pendingStartOffset_ += end + delimiter;
            ProcessLine(std::move(line), lineOffset, path, identity);
            committedOffset_ = pendingStartOffset_;
        }

        constexpr std::size_t maximumLineBytes = 128 * 1024;
        if (pendingBytes_.size() > maximumLineBytes) {
            pendingBytes_.clear();
            pendingStartOffset_ = nextReadOffset_;
            committedOffset_ = nextReadOffset_;
        }
    }

    SaveCursor(file, path, identity, fileSize);
    initialScanComplete_ = true;
    // A replay is a one-shot recovery scan. Keeping this set would make every
    // later re-initialization (truncation, path change) replay the tail again.
    // SetAdminNick() explicitly raises it again when a recount is needed.
    replayRecentOnStart_ = false;
    SetStatus("Chatlog отслеживается");
    return true;
}

void ChatlogReader::Run() {
    std::filesystem::path activePath;
    while (running_.load()) {
        const auto path = CurrentPath();
        if (path != activePath) {
            activePath = path;
            fileInitialized_ = false;
            initialScanComplete_ = false;
            pendingBytes_.clear();
            nextReadOffset_ = pendingStartOffset_ = committedOffset_ = 0;
        }
        if (path.empty()) {
            SetStatus("Укажите путь к chatlog в настройках");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            continue;
        }

        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            SetStatus("Ожидание chatlog: " + WideToUtf8(path.wstring()));
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            continue;
        }

        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) {
            CloseHandle(file);
            SetStatus("Не удалось прочитать свойства chatlog");
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            continue;
        }

        const auto size = FileSize(info);
        const auto identity = FileIdentity(info);
        const bool ok = ReadAvailable(file, path, size, identity);
        CloseHandle(file);
        if (!ok) SetStatus("Ошибка чтения chatlog; повторная попытка");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
}

} // namespace reportpanel
