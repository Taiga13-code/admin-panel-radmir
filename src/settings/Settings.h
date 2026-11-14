#pragma once

#include <filesystem>
#include <string>

namespace reportpanel {

struct AppSettings {
    std::filesystem::path chatlogPath;
    std::string adminNick;
    int transparency{94};
    int fontSize{17};
    float accentR{0.58f};
    float accentG{0.24f};
    float accentB{0.96f};
    bool alwaysOnTop{true};
    bool notifyNew{true};
    bool soundNewReport{true};
    std::string soundName; // имя файла в sound\ рядом с плагином (пусто = системный)
    int windowX{100};
    int windowY{80};
    int windowWidth{1040};
    int windowHeight{640};
    int toggleKey{0x7A}; // VK_F11
};

class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path settingsPath);

    [[nodiscard]] AppSettings Load() const;
    void Save(const AppSettings& settings) const;
    [[nodiscard]] const std::filesystem::path& Path() const { return settingsPath_; }

    [[nodiscard]] static std::filesystem::path LocalDataDirectory();
    [[nodiscard]] static std::filesystem::path DefaultChatlogPath();

private:
    std::filesystem::path settingsPath_;
};

} // namespace reportpanel
