#include "Settings.h"
#include "JsonFile.h"
#include "../platform/Encoding.h"

#include <ShlObj.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace reportpanel {

SettingsStore::SettingsStore(std::filesystem::path settingsPath)
    : settingsPath_(std::move(settingsPath)) {}

std::filesystem::path SettingsStore::LocalDataDirectory() {
    wchar_t* localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                                nullptr, &localAppData);
    if (FAILED(result) || !localAppData) {
        return std::filesystem::temp_directory_path() / L"ReportPanel";
    }
    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    return path / L"ReportPanel";
}

std::filesystem::path SettingsStore::DefaultChatlogPath() {
    wchar_t* documents = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT,
                                                nullptr, &documents);
    if (FAILED(result) || !documents) {
        return std::filesystem::path(L"");
    }
    std::filesystem::path path(documents);
    CoTaskMemFree(documents);
    return path / L"RADMIR CRMP User Files" / L"SAMP" / L"chatlog.txt";
}

AppSettings SettingsStore::Load() const {
    AppSettings settings;
    settings.chatlogPath = DefaultChatlogPath();
    try {
        const auto json = ReadJsonFile(settingsPath_);
        if (!json.is_object()) return settings;
        if (json.contains("chatlogPath") && json["chatlogPath"].is_string()) {
            settings.chatlogPath = std::filesystem::path(
                Utf8ToWide(json["chatlogPath"].get<std::string>()));
        }
        settings.adminNick = json.value("adminNick", settings.adminNick);
        settings.transparency = std::clamp(json.value("transparency", settings.transparency), 30, 100);
        settings.fontSize = std::clamp(json.value("fontSize", settings.fontSize), 12, 24);
        settings.accentR = std::clamp(json.value("accentR", settings.accentR), 0.0f, 1.0f);
        settings.accentG = std::clamp(json.value("accentG", settings.accentG), 0.0f, 1.0f);
        settings.accentB = std::clamp(json.value("accentB", settings.accentB), 0.0f, 1.0f);
        settings.alwaysOnTop = json.value("alwaysOnTop", settings.alwaysOnTop);
        settings.notifyNew = json.value("notifyNew", settings.notifyNew);
        settings.windowX = json.value("windowX", settings.windowX);
        settings.windowY = json.value("windowY", settings.windowY);
        settings.windowWidth = std::clamp(json.value("windowWidth", settings.windowWidth), 640, 7680);
        settings.windowHeight = std::clamp(json.value("windowHeight", settings.windowHeight), 420, 4320);
        settings.toggleKey = json.value("toggleKey", settings.toggleKey);
        if (settings.toggleKey < 1 || settings.toggleKey > 254)
            settings.toggleKey = VK_F11;
    } catch (...) {
    }
    return settings;
}

void SettingsStore::Save(const AppSettings& settings) const {
    nlohmann::json json;
    json["chatlogPath"] = WideToUtf8(settings.chatlogPath.wstring());
    json["adminNick"] = settings.adminNick;
    json["transparency"] = settings.transparency;
    json["fontSize"] = settings.fontSize;
    json["accentR"] = settings.accentR;
    json["accentG"] = settings.accentG;
    json["accentB"] = settings.accentB;
    json["alwaysOnTop"] = settings.alwaysOnTop;
    json["notifyNew"] = settings.notifyNew;
    json["windowX"] = settings.windowX;
    json["windowY"] = settings.windowY;
    json["windowWidth"] = settings.windowWidth;
    json["windowHeight"] = settings.windowHeight;
    json["toggleKey"] = settings.toggleKey;
    WriteJsonFileAtomic(settingsPath_, json);
}

} // namespace reportpanel
