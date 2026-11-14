#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace reportpanel {

// Пресет причины: кнопка в окне наказания подставляет причину и время.
struct PunishReason {
    std::string reason;
    int time{0}; // минуты; 0 — время не задаётся
};

// Готовое наказание: кнопка на репорте, шаблон команды с подстановкой
// {id} / {time} / {reason}.
struct PunishForm {
    std::string label;       // «Мут», «Варн»…
    std::string command;     // "/mute {id} {time} {reason}"
    bool needsTime{false};
    bool needsReason{false};
    bool createsTimer{true}; // заводить обратный отсчёт после отправки
    int defaultTime{30};
    std::vector<PunishReason> reasons; // пресеты причин из forms.json
};

// Подсказка ответа: если в тексте репорта есть ключевые слова, окно ответа
// предлагает готовый ответ кнопкой.
struct ReplySuggestion {
    std::vector<std::wstring> keywordsLower; // ключевые слова (нижний регистр)
    std::string answer;                      // готовый ответ (UTF-8)
};

// Строка справочника команд.
struct CommandRef {
    std::string section;      // раздел (может быть пустым)
    std::string command;      // "/check"
    std::string description;  // «Статистика игрока»
};

// Таймер наказания: обратный отсчёт мута/деморгана.
struct PunishTimer {
    std::string playerName;
    std::uint32_t playerId{0};
    std::string label;       // «Мут»
    std::chrono::steady_clock::time_point expiresAt;
};

// Загружает настраиваемые данные из файлов рядом с плагином:
//   forms.json       — шаблоны наказаний;
//   filters.json     — слова рекламы и мата для подсветки репортов;
//   suggestions.json — подсказки ответов по ключевым словам;
//   commands.json    — справочник команд;
//   notes.txt        — заметки админа;
//   sound\*.mp3      — доступные звуки уведомлений.
// Все файлы можно редактировать без пересборки плагина.
class ToolkitStore {
public:
    void Initialize(const std::filesystem::path& pluginDirectory);

    void RescanSounds();
    void ReloadForms();
    void ReloadFilters();
    void ReloadSuggestions();
    void ReloadCommands();
    void LoadNotes();
    void SaveNotes() const;

    [[nodiscard]] const PunishForm* FindForm(std::string_view label) const;

    // 0 — совпадений нет, 1 — реклама, 2 — мат.
    [[nodiscard]] int MatchesFilters(const std::string& reportText,
                                     std::string* matchedWord = nullptr) const;

    // До трёх подходящих подсказок, отсортированных по числу совпадений.
    [[nodiscard]] std::vector<const ReplySuggestion*> SuggestFor(
        const std::string& reportText) const;

    [[nodiscard]] const std::filesystem::path& PluginDirectory() const {
        return pluginDirectory_;
    }

    std::vector<PunishForm> forms;
    std::vector<ReplySuggestion> suggestions;
    std::vector<CommandRef> commands;
    std::vector<std::wstring> adWords;    // нижний регистр
    std::vector<std::wstring> badWords;   // нижний регистр
    std::vector<std::string> soundFiles;  // имена файлов sound\*.mp3
    std::string notes;                    // содержимое заметок (UTF-8)

private:
    std::filesystem::path pluginDirectory_;
};

} // namespace reportpanel
