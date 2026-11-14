#include "ToolkitStore.h"

#include "../platform/Encoding.h"
#include "../settings/JsonFile.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace reportpanel {
namespace {

std::wstring ToLower(std::wstring_view value) {
    const int required = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE,
        value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr, 0);
    if (required <= 0) return std::wstring(value);
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE,
        value.data(), static_cast<int>(value.size()), result.data(), required,
        nullptr, nullptr, 0);
    return result;
}

void WriteIfMissing(const std::filesystem::path& path, const nlohmann::json& value) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) return;
    try {
        WriteJsonFileAtomic(path, value);
    } catch (...) {
    }
}

nlohmann::json DefaultForms() {
    // RADMIR: шаблоны, причины и тайминги из рабочего AHK админа (R-RP 04).
    // Полный настраиваемый список — в forms.json рядом с плагином.
    const char* const json = R"JSON(
{
  "forms": [
    {"label": "Реп-мут", "template": "/rmute {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 60, "reasons": [
      {"reason": "Оскорбления в репорт", "time": 60},
      {"reason": "Оскорбления в репорт", "time": 120},
      {"reason": "Оскорбления в репорт", "time": 360},
      {"reason": "Мат в репорт", "time": 30},
      {"reason": "Мат в репорт", "time": 60},
      {"reason": "Мат в репорт", "time": 120},
      {"reason": "Капс в репорт", "time": 30},
      {"reason": "Капс в репорт", "time": 60},
      {"reason": "Оффтоп в репорт", "time": 15},
      {"reason": "Оффтоп в репорт", "time": 30}
    ]},
    {"label": "Мут", "template": "/mute {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 60, "reasons": [
      {"reason": "Бред /edit", "time": 60},
      {"reason": "nonRP (Бред /me | /do | /try)", "time": 15},
      {"reason": "Оскорбление в ООС чат", "time": 30},
      {"reason": "Оскорбление в ООС чат", "time": 60},
      {"reason": "Оскорбление администрации", "time": 120},
      {"reason": "Flood", "time": 15},
      {"reason": "MG", "time": 60},
      {"reason": "Caps в ООС чат", "time": 60},
      {"reason": "Транслит", "time": 15},
      {"reason": "Казино в деморгане", "time": 30},
      {"reason": "Оффтоп в /d или /dd", "time": 60},
      {"reason": "Неотредактированное объявление", "time": 30}
    ]},
    {"label": "Войс-мут", "template": "/v_mute {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 60, "reasons": [
      {"reason": "Музыкант", "time": 60},
      {"reason": "Бред в ГЧ", "time": 60}
    ]},
    {"label": "Фам-мут", "template": "/fmute {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 5760, "timer": false, "reasons": [
      {"reason": "Оскорбление родных", "time": 5760},
      {"reason": "Оскорбление проекта", "time": 5760}
    ]},
    {"label": "Деморган", "template": "/jail {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 120, "reasons": [
      {"reason": "DM", "time": 60},
      {"reason": "DM", "time": 120},
      {"reason": "DM", "time": 180},
      {"reason": "DM рабочего", "time": 240},
      {"reason": "DB", "time": 30},
      {"reason": "DB", "time": 60},
      {"reason": "DB", "time": 120},
      {"reason": "PG", "time": 60},
      {"reason": "PG", "time": 120},
      {"reason": "RK", "time": 60},
      {"reason": "RK", "time": 120},
      {"reason": "SK", "time": 120},
      {"reason": "TK", "time": 120},
      {"reason": "DMcar", "time": 30},
      {"reason": "DMcar", "time": 60},
      {"reason": "Гражданский на ВЧ", "time": 30},
      {"reason": "Гражданский на ВЧ", "time": 60},
      {"reason": "Езда по полям", "time": 30},
      {"reason": "Езда по полям", "time": 60},
      {"reason": "встречка", "time": 30},
      {"reason": "встречка", "time": 60},
      {"reason": "nonRP угон", "time": 15},
      {"reason": "nonRP угон", "time": 30},
      {"reason": "nonRP казино", "time": 30},
      {"reason": "Провокация на DM", "time": 5},
      {"reason": "Провокация на DM", "time": 10},
      {"reason": "NonRP езда", "time": 60},
      {"reason": "NonRP поведение", "time": 60},
      {"reason": "кач зп", "time": 120},
      {"reason": "+с", "time": 120},
      {"reason": "Отводы", "time": 120},
      {"reason": "Сбив анимации", "time": 120},
      {"reason": "Багоюз", "time": 120},
      {"reason": "Багоюз ПТ", "time": 120},
      {"reason": "Бег с Б/П", "time": 60},
      {"reason": "DM gang", "time": 120},
      {"reason": "BH во время стрельбы", "time": 30},
      {"reason": "nonRP /escort", "time": 120},
      {"reason": "nonRP /givelic", "time": 120},
      {"reason": "nonRP /arrest", "time": 120},
      {"reason": "Уход в интерьер от смерти", "time": 240},
      {"reason": "Похищение вне 18:00-23:00", "time": 120},
      {"reason": "Урон/убийство после ограбления", "time": 180},
      {"reason": "Ложная жалоба на МВД/ФСБ", "time": 120}
    ]},
    {"label": "Варн", "template": "/warn {id} {reason}", "reason": true, "reasons": [
      {"reason": "SK"},
      {"reason": "TK"},
      {"reason": "таран"},
      {"reason": "DB in ZZ"},
      {"reason": "Уход от РП процесса"},
      {"reason": "ВЧ без маски"},
      {"reason": "Срыв захвата района/особняка"},
      {"reason": "DM gang"},
      {"reason": "Нападение без маски при задержании"},
      {"reason": "Запрещенное оружие на захвате"},
      {"reason": "Маска/анимации после 5 минуты"},
      {"reason": "Труднодоступная территория после 6 минуты"},
      {"reason": "Взаимодействие с т/с"},
      {"reason": "Ограбление инкассации без маски"},
      {"reason": "Перехват поставки без маски"},
      {"reason": "Контрабанда без маски"},
      {"reason": "Похищение в зеленой зоне"},
      {"reason": "Похищение/ограбление без маски"},
      {"reason": "Не обезврежен свидетель"},
      {"reason": "Функционал фракции без формы"},
      {"reason": "nonRP /escort"},
      {"reason": "nonRP /givelic"},
      {"reason": "nonRP /arrest"},
      {"reason": "Выход из игры находясь в стадии смерти"}
    ]},
    {"label": "Кик", "template": "/kick {id} {reason}", "reason": true, "reasons": [
      {"reason": "помеха мз"},
      {"reason": "после кпз"},
      {"reason": "nonRP (оружие в зз)"},
      {"reason": "AFK во время захвата"},
      {"reason": "200+ пинг на захвате"}
    ]},
    {"label": "Бан", "template": "/ban {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 10, "timer": false, "reasons": [
      {"reason": "Оскорбление нации", "time": 5},
      {"reason": "Оскорбление нации", "time": 10},
      {"reason": "Оскорбление нации", "time": 15}
    ]},
    {"label": "С-бан", "template": "/sban {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 1, "timer": false, "reasons": [
      {"reason": "cheat", "time": 1},
      {"reason": "бот", "time": 1},
      {"reason": "реклама", "time": 1}
    ]},
    {"label": "Офф-бан", "template": "/soffban {name} {reason}", "reason": true, "timer": false, "reasons": [
      {"reason": "бот"},
      {"reason": "cheat"}
    ]},
    {"label": "К-джейл", "template": "/kjail {id} {time} {reason}", "time": true, "reason": true, "defaultTime": 5252, "timer": false, "reasons": [
      {"reason": "до бана", "time": 5252}
    ]},
    {"label": "Слэп", "template": "/slap {id}"},
    {"label": "Фриз", "template": "/freeze {id}"}
  ]
}
)JSON";
    // allow_exceptions = false: при опечатке вернём пустой объект, а не уроним
    // плагин на старте.
    auto parsed = nlohmann::json::parse(json, nullptr, false, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

nlohmann::json DefaultFilters() {
    return nlohmann::json{
        {"adWords", nlohmann::json::array({
            "продам", "продаю", "продажа", "продать", "куплю", "купить",
            "покупка", "скуплю", "обменяю", "обмен", "аренда", "донат",
            "реальных денег", "реальные деньги", "за рубли", "вконтакте",
            "vk.com", "vk.me", "телеграм", "telegram", "t.me", "дискорд",
            "discord", "скайп", "skype",
        })},
        {"badWords", nlohmann::json::array({
            "хуй", "пизд", "бля", "сука", "еба", "ёба", "мраз", "гонд",
            "урод", "долбо", "пидор", "лох",
        })},
    };
}

nlohmann::json DefaultSuggestions() {
    // RADMIR (R-RP 04): типовые ответы на репорты + фирменная подпись.
    // Полный настраиваемый список — в suggestions.json рядом с плагином.
    const char* const json = R"JSON(
{
  "suggestions": [
    {"keywords": ["дм", "убил", "стреля", "атака", "атакует", "спавнкилл", "спавн килл"],
     "answer": "Здравствуйте. Нарушитель наказан по форме ДМ, приложите видео для ускорения проверки. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["дб", "сбил", "таранит", "давит"], "answer": "Здравствуйте. За DB наказываю, док-ва уже смотрю. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["чит", "читер", "читак", "инжект", "aim", "wh"], "answer": "Здравствуйте. Нарушитель проверяется, при подтверждении накажу. Док-ва можете скинуть в репорт. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["багоюз", "баг юз", "дюп", "дюпает"], "answer": "Здравствуйте. Багоюзер наказан, спасибо за бдительность. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["оскорб", "мат", "мама", "родные", "троллинг"], "answer": "Здравствуйте. Нарушитель замучен, оскорбления у нас недопустимы. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["оскорбление нации", "нация", "расизм"], "answer": "Здравствуйте. За оскорбление нации нарушитель получил бан. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["капс"], "answer": "Здравствуйте. Нарушитель наказан по форме капс, выключите капс в чате. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["флуд", "флад", "flood", "спамит"], "answer": "Здравствуйте. Нарушитель замучен за флуд. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["мг", "mg", "метагейм"], "answer": "Здравствуйте. Нарушитель наказан по форме MG, это чат-информация. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["нонрп", "nonrp", "non rp", "нрп", "non rp поведение"], "answer": "Здравствуйте. Нарушитель наказан по форме NonRP. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["pg", "пж", "povergame", "повер"], "answer": "Здравствуйте. Нарушитель наказан по форме PG. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["скам", "кинул", "обманул", "не отдал", "мошенни"], "answer": "Здравствуйте. Скам наказывается баном, приложите скриншоты сделки. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["реклам", "продам сервер", "айпи", "ip", "дс сервер"], "answer": "Здравствуйте. Реклама на проекте запрещена, нарушитель получил бан. Спасибо за бдительность. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["казино", "рулетк", "казин"], "answer": "Здравствуйте. Нарушитель наказан по форме казино в деморгане. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["украл тачку", "угон", "угнал", "машину угнал"], "answer": "Здравствуйте. Нарушитель наказан по форме NonRP угон. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["встреч", "по встречке"], "answer": "Здравствуйте. Нарушитель наказан по форме встречка. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["поля", "по полям", "поле едет"], "answer": "Здравствуйте. Нарушитель наказан по форме езда по полям. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["захват", "альянс", "ганзахват", "бизнес захват"], "answer": "Здравствуйте. Захват объявляется за 5 минут в /f чате, таймер смотреть /capture_timer. Нарушения формы — пишите в репорт. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["амуниция", "аммо", "закуп"], "answer": "Здравствуйте. Захват амуниции проходит по правилам формы, нарушения смотрите в /capture_timer и пишите в репорт. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["тюнинг", "обвес", "не могу поставить"], "answer": "Здравствуйте. Тюнинг доступен в автосалоне/на СТО, если не ставится — приложите скрин и опишите шаги. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["верните", "украли аккаунт", "взлом"], "answer": "Здравствуйте. По вопросам взлома и восстановлений — раздел «Жалобы и восстановления» на форуме, приложите док-ва владения. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["баг", "ошибка сервера", "не работает", "не могу"], "answer": "Здравствуйте. Спасибо, передам разработчикам. Опишите шаги, как повторить баг, и приложите скриншот. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["помогите", "не знаю", "новеньк", "как играт", "начать"], "answer": "Здравствуйте. Откройте /help — основные команды и навигация для новичков, стартовые задания в /menu. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["спавн", "респавн", "машину забрали", "пропала машина"], "answer": "Здравствуйте. Транспорт ищите на штрафстоянке (/tp list у гос.фракций), или воспользуйтесь /findcar. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["лид", "лидер", "замена", "слетел"], "answer": "Здравствуйте. Вопросы по лидерству решаются через зам.лидера или куратора фракции. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["жалоба", "модера", "админа", "разбан", "наказание несправедлив"], "answer": "Здравствуйте. По обжалованию наказаний — раздел «Жалобы на администрацию» на форуме, приложите док-ва. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["продам", "куплю", "обмен", "пополню", "обменяю"], "answer": "Здравствуйте. Обмены и продажа вне серверной экономики запрещены, торгуйтесь в /trade или через РП процесс. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["варн", "кпз", "деморган", "когда выйду"], "answer": "Здравствуйте. Ваше наказание истекает по таймеру, точное время видно в /stats. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["пинг", "лаг", "фризит", "выкинуло"], "answer": "Здравствуйте. По техническим проблемам попробуйте перезапуск игры и роутера, если не помогает — тикет на форуме. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["американ", "русс", "национальн"], "answer": "Здравствуйте. Оскорбления по национальному признаку недопустимы, нарушитель наказан. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["фракц", "форма", "заполн", "приём"], "answer": "Здравствуйте. Собеседования и приём во фракции — по расписанию лидера, смотрите /f чат или форум. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["биз", "заправк", "продам бизнес"], "answer": "Здравствуйте. Бизнесы продаются через /sellbiz, стоимость устанавливает владелец. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["офлайн", "смерть", "респавн насильно", "вытащите"], "answer": "Здравствуйте. Если вы застряли — напишите /q, зайдите снова, или просите админа на респавн через репорт. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["nonrp еда", "поесть", "голод"], "answer": "Здравствуйте. NonRP поведение в еде/снаряжении наказывается, приложите док-ва. | Приятной игры на R-RP 04 by A.Taiga ^.^"},
    {"keywords": ["инкас", "инкассация", "банк"],
     "answer": "Здравствуйте. Ограбление инкассации проходит по форме с маской и в разрешенное время, нарушения — в репорт. | Приятной игры на R-RP 04 by A.Taiga ^.^"}
  ]
}
)JSON";
    auto parsed = nlohmann::json::parse(json, nullptr, false, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

nlohmann::json DefaultCommands() {
    // RADMIR (R-RP 04): админ-команды из рабочего AHK. Разделы:
    // «Администрация», «Ответы игрокам», «МП и объявления».
    const char* const json = R"JSON(
{
  "commands": [
    {"section": "Администрация", "cmd": "/check", "desc": "Статистика игрока"},
    {"section": "Администрация", "cmd": "/checkpass", "desc": "Паспорт игрока"},
    {"section": "Администрация", "cmd": "/checkpunish", "desc": "История наказаний игрока"},
    {"section": "Администрация", "cmd": "/checklic", "desc": "Лицензии игрока"},
    {"section": "Администрация", "cmd": "/checkskills", "desc": "Скиллы и улучшения игрока"},
    {"section": "Администрация", "cmd": "/warns", "desc": "Варны игрока"},
    {"section": "Администрация", "cmd": "/alist", "desc": "Список администраторов онлайн"},
    {"section": "Администрация", "cmd": "/gethere", "desc": "Телепортировать игрока к себе"},
    {"section": "Администрация", "cmd": "/goto", "desc": "Телепорт к игроку"},
    {"section": "Администрация", "cmd": "/gotocar", "desc": "Телепорт к машине"},
    {"section": "Администрация", "cmd": "/getherecar", "desc": "Телепортировать машину к себе"},
    {"section": "Администрация", "cmd": "/gethereall", "desc": "Собрать всех игроков к себе"},
    {"section": "Администрация", "cmd": "/sp", "desc": "Следить за игроком"},
    {"section": "Администрация", "cmd": "/re", "desc": "Выйти из режима слежки"},
    {"section": "Администрация", "cmd": "/flip", "desc": "Перевернуть и починить транспорт"},
    {"section": "Администрация", "cmd": "/freeze", "desc": "Заморозить игрока"},
    {"section": "Администрация", "cmd": "/unfreeze", "desc": "Разморозить игрока"},
    {"section": "Администрация", "cmd": "/slap", "desc": "Подбросить игрока"},
    {"section": "Администрация", "cmd": "/jail", "desc": "Деморган: /jail id минуты причина"},
    {"section": "Администрация", "cmd": "/kjail", "desc": "К-джейл до бана: /kjail id 5252 причина"},
    {"section": "Администрация", "cmd": "/mute", "desc": "Мут чата: /mute id минуты причина"},
    {"section": "Администрация", "cmd": "/rmute", "desc": "Мут репорта: /rmute id минуты причина"},
    {"section": "Администрация", "cmd": "/v_mute", "desc": "Войс-мут: /v_mute id минуты причина"},
    {"section": "Администрация", "cmd": "/fmute", "desc": "Фам-мут: /fmute id 5760 причина"},
    {"section": "Администрация", "cmd": "/warn", "desc": "Варн: /warn id причина"},
    {"section": "Администрация", "cmd": "/kick", "desc": "Кик: /kick id причина"},
    {"section": "Администрация", "cmd": "/ban", "desc": "Бан: /ban id дни причина"},
    {"section": "Администрация", "cmd": "/sban", "desc": "С-бан: /sban id 1 причина"},
    {"section": "Администрация", "cmd": "/soffban", "desc": "Офф-бан: /soffban ник причина"},
    {"section": "Администрация", "cmd": "/jaillist", "desc": "Список заключённых"},
    {"section": "Администрация", "cmd": "/hp", "desc": "Выдать HP: /hp id значение"},
    {"section": "Администрация", "cmd": "/veh", "desc": "Заспавнить машину: /veh id модель 0 1 1 1"},
    {"section": "Администрация", "cmd": "/a", "desc": "Админ-чат"},
    {"section": "Администрация", "cmd": "/templeader", "desc": "Временный лидер фракции"},
    {"section": "Администрация", "cmd": "/tplist", "desc": "Список точек телепорта"},
    {"section": "Администрация", "cmd": "/call 01", "desc": "Служба 01 (медики)"},
    {"section": "Администрация", "cmd": "/call 02", "desc": "Служба 02 (полиция)"},
    {"section": "Администрация", "cmd": "/call 03", "desc": "Служба 03 (ФСБ/авто)"},
    {"section": "Ответы игрокам", "cmd": "/msg", "desc": "Личное сообщение игроку"},
    {"section": "Ответы игрокам", "cmd": "/ot", "desc": "Ответить на репорт (чат)"},
    {"section": "МП и объявления", "cmd": "/mp start", "desc": "Начать мероприятие"},
    {"section": "МП и объявления", "cmd": "/mp finish", "desc": "Завершить мероприятие"},
    {"section": "МП и объявления", "cmd": "/capture_timer", "desc": "Таймер захвата"},
    {"section": "МП и объявления", "cmd": "/help", "desc": "Справка для игроков"}
  ]
}
)JSON";
    auto parsed = nlohmann::json::parse(json, nullptr, false, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

} // namespace

void ToolkitStore::Initialize(const std::filesystem::path& pluginDirectory) {
    pluginDirectory_ = pluginDirectory;
    std::error_code ec;
    std::filesystem::create_directories(pluginDirectory_ / L"sound", ec);

    WriteIfMissing(pluginDirectory_ / L"forms.json", DefaultForms());
    WriteIfMissing(pluginDirectory_ / L"filters.json", DefaultFilters());
    WriteIfMissing(pluginDirectory_ / L"suggestions.json", DefaultSuggestions());
    WriteIfMissing(pluginDirectory_ / L"commands.json", DefaultCommands());

    ReloadForms();
    ReloadFilters();
    ReloadSuggestions();
    ReloadCommands();
    RescanSounds();
    LoadNotes();
}

void ToolkitStore::ReloadForms() {
    forms.clear();
    try {
        const auto json = ReadJsonFile(pluginDirectory_ / L"forms.json");
        if (!json.contains("forms") || !json["forms"].is_array()) return;
        for (const auto& item : json["forms"]) {
            if (!item.is_object()) continue;
            PunishForm form;
            form.label = item.value("label", std::string());
            form.command = item.value("template", std::string());
            form.needsTime = item.value("time", false);
            form.needsReason = item.value("reason", false);
            form.createsTimer = item.value("timer", true);
            form.defaultTime = std::clamp(item.value("defaultTime", 30), 1, 10080);
            if (item.contains("reasons") && item["reasons"].is_array()) {
                for (const auto& preset : item["reasons"]) {
                    if (!preset.is_object()) continue;
                    PunishReason reason;
                    reason.reason = preset.value("reason", std::string());
                    reason.time = std::clamp(preset.value("time", 0), 0, 10080);
                    if (!reason.reason.empty()) {
                        form.reasons.push_back(std::move(reason));
                    }
                }
            }
            if (!form.label.empty() && !form.command.empty()) {
                forms.push_back(std::move(form));
            }
        }
    } catch (...) {
    }
}

void ToolkitStore::ReloadFilters() {
    adWords.clear();
    badWords.clear();
    try {
        const auto json = ReadJsonFile(pluginDirectory_ / L"filters.json");
        const auto readList = [&json](const char* key, std::vector<std::wstring>& target) {
            if (!json.contains(key) || !json[key].is_array()) return;
            for (const auto& item : json[key]) {
                if (!item.is_string()) continue;
                auto word = ToLower(Utf8ToWide(item.get<std::string>()));
                if (!word.empty()) target.push_back(std::move(word));
            }
        };
        readList("adWords", adWords);
        readList("badWords", badWords);
    } catch (...) {
    }
}

void ToolkitStore::ReloadSuggestions() {
    suggestions.clear();
    try {
        const auto json = ReadJsonFile(pluginDirectory_ / L"suggestions.json");
        if (!json.contains("suggestions") || !json["suggestions"].is_array()) return;
        for (const auto& item : json["suggestions"]) {
            if (!item.is_object()) continue;
            ReplySuggestion suggestion;
            suggestion.answer = item.value("answer", std::string());
            if (item.contains("keywords") && item["keywords"].is_array()) {
                for (const auto& keyword : item["keywords"]) {
                    if (!keyword.is_string()) continue;
                    auto lowered = ToLower(Utf8ToWide(keyword.get<std::string>()));
                    if (!lowered.empty()) suggestion.keywordsLower.push_back(std::move(lowered));
                }
            }
            if (!suggestion.answer.empty() && !suggestion.keywordsLower.empty()) {
                suggestions.push_back(std::move(suggestion));
            }
        }
    } catch (...) {
    }
}

void ToolkitStore::ReloadCommands() {
    commands.clear();
    try {
        const auto json = ReadJsonFile(pluginDirectory_ / L"commands.json");
        if (!json.contains("commands") || !json["commands"].is_array()) return;
        for (const auto& item : json["commands"]) {
            if (!item.is_object()) continue;
            CommandRef entry;
            entry.section = item.value("section", std::string());
            entry.command = item.value("cmd", std::string());
            entry.description = item.value("desc", std::string());
            if (!entry.command.empty()) commands.push_back(std::move(entry));
        }
    } catch (...) {
    }
}

void ToolkitStore::RescanSounds() {
    soundFiles.clear();
    std::error_code ec;
    const auto dir = pluginDirectory_ / L"sound";
    if (!std::filesystem::exists(dir, ec) || ec) return;

    std::vector<std::wstring> names;
    std::filesystem::directory_iterator iterator(dir, ec);
    if (ec) return;
    for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(ec)) {
        if (ec) break;
        if (!iterator->is_regular_file(ec) || ec) continue;
        auto extension = iterator->path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension != L".mp3") continue;
        names.push_back(iterator->path().filename().wstring());
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) soundFiles.push_back(WideToUtf8(name));
}

void ToolkitStore::LoadNotes() {
    notes.clear();
    std::error_code ec;
    const auto path = pluginDirectory_ / L"notes.txt";
    if (!std::filesystem::exists(path, ec) || ec) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    const std::string content{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>{}};
    // Файл может быть сохранён блокнотом в ANSI — определяем кодировку.
    notes = DecodeLineToUtf8(content, TextEncoding::Detect);
}

void ToolkitStore::SaveNotes() const {
    std::error_code ec;
    std::filesystem::create_directories(pluginDirectory_, ec);
    std::ofstream output(pluginDirectory_ / L"notes.txt",
                         std::ios::binary | std::ios::trunc);
    if (!output) return;
    output.write(notes.data(), static_cast<std::streamsize>(notes.size()));
}

const PunishForm* ToolkitStore::FindForm(std::string_view label) const {
    for (const auto& form : forms) {
        if (form.label == label) return &form;
    }
    return nullptr;
}

int ToolkitStore::MatchesFilters(const std::string& reportText,
                                 std::string* matchedWord) const {
    if (adWords.empty() && badWords.empty()) return 0;
    const auto lowered = ToLower(Utf8ToWide(reportText));
    for (const auto& word : adWords) {
        if (lowered.find(word) != std::wstring::npos) {
            if (matchedWord) *matchedWord = WideToUtf8(word);
            return 1;
        }
    }
    for (const auto& word : badWords) {
        if (lowered.find(word) != std::wstring::npos) {
            if (matchedWord) *matchedWord = WideToUtf8(word);
            return 2;
        }
    }
    return 0;
}

std::vector<const ReplySuggestion*> ToolkitStore::SuggestFor(
    const std::string& reportText) const {
    const auto lowered = ToLower(Utf8ToWide(reportText));
    std::vector<std::pair<int, const ReplySuggestion*>> scored;
    for (const auto& suggestion : suggestions) {
        int score = 0;
        for (const auto& keyword : suggestion.keywordsLower) {
            if (lowered.find(keyword) != std::wstring::npos) ++score;
        }
        if (score > 0) scored.emplace_back(score, &suggestion);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const ReplySuggestion*> result;
    for (std::size_t i = 0; i < scored.size() && i < 3; ++i) {
        result.push_back(scored[i].second);
    }
    return result;
}

} // namespace reportpanel
