#include "ClipboardGameIntegration.h"
#include "GameInputState.h"
#include "../platform/Encoding.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace reportpanel {
namespace {

bool GameWindowIsActive(HWND gameWindow) {
    if (!IsWindow(gameWindow) || GetForegroundWindow() != gameWindow) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(gameWindow, &processId);
    return processId == GetCurrentProcessId();
}

// Прямая отправка команды через SendInput (быстрая, без буфера обмена)
GameActionResult SubmitCommandImpl(std::string commandUtf8, std::string successMessage) {
    const HWND gameWindow = GetForegroundWindow();
    if (!GameWindowIsActive(gameWindow)) {
        return {false, "Сначала переключись в окно игры."};
    }

    if (g_gameInputAutomationActive.exchange(true)) {
        return {false, "Предыдущая команда ещё отправляется."};
    }

    const auto releaseAutomation = [] {
        g_gameInputAutomationActive.store(false);
    };
    // Clear any existing clipboard data to avoid leftover text.
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
    const std::wstring command = Utf8ToWide(commandUtf8);
    if (command.empty()) {
        releaseAutomation();
        return {false, "Пустая команда."};
    }

    // Эмулируем нажатие T (открыть чат). Ctrl+V и Enter отправляет поток
    // ниже, уже после того, как команда будет записана в буфер обмена.
    // Раньше Ctrl+V уходил вместе с T, до записи в буфер: первый Ctrl+V
    // вставлял старое содержимое буфера (предыдущую команду), и в чат
    // попадало "/sp 527/sp 527".
    INPUT inputs[2] = {};

    // T down
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 'T';
    inputs[0].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

    // T up
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'T';
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[1].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        releaseAutomation();
        return {false, "Не удалось открыть чат."};
    }

    try {
        std::thread([gameWindow, command, releaseAutomation] {
            // Даём игре время открыть чат после T (50 мс иногда не хватает,
            // и вставка попадает в пустоту).
            Sleep(100);
            if (!GameWindowIsActive(gameWindow)) {
                releaseAutomation();
                return;
            }

            // Вставляем текст через буфер обмена (быстрее чем по буквам)
            if (!OpenClipboard(nullptr)) {
                releaseAutomation();
                return;
            }
            EmptyClipboard();
            const std::size_t bytes = (command.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!memory) {
                CloseClipboard();
                releaseAutomation();
                return;
            }
            void* target = GlobalLock(memory);
            if (!target) {
                GlobalFree(memory);
                CloseClipboard();
                releaseAutomation();
                return;
            }
            std::memcpy(target, command.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
                CloseClipboard();
                releaseAutomation();
                return;
            }
            CloseClipboard();

            Sleep(50);
            if (!GameWindowIsActive(gameWindow)) {
                releaseAutomation();
                return;
            }

            // Ctrl+V
            INPUT paste[4] = {};
            paste[0].type = INPUT_KEYBOARD;
            paste[0].ki.wVk = VK_CONTROL;
            paste[0].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

            paste[1].type = INPUT_KEYBOARD;
            paste[1].ki.wVk = 'V';
            paste[1].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

            paste[2].type = INPUT_KEYBOARD;
            paste[2].ki.wVk = 'V';
            paste[2].ki.dwFlags = KEYEVENTF_KEYUP;
            paste[2].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

            paste[3].type = INPUT_KEYBOARD;
            paste[3].ki.wVk = VK_CONTROL;
            paste[3].ki.dwFlags = KEYEVENTF_KEYUP;
            paste[3].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

            if (SendInput(4, paste, sizeof(INPUT)) != 4) {
                releaseAutomation();
                return;
            }

            Sleep(50);
            if (GameWindowIsActive(gameWindow)) {
                // Enter
                INPUT enter[2] = {};
                enter[0].type = INPUT_KEYBOARD;
                enter[0].ki.wVk = VK_RETURN;
                enter[0].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

                enter[1].type = INPUT_KEYBOARD;
                enter[1].ki.wVk = VK_RETURN;
                enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
                enter[1].ki.dwExtraInfo = static_cast<ULONG_PTR>(kGameInputAutomationTag);

                SendInput(2, enter, sizeof(INPUT));
                // Clear clipboard after sending command to avoid leftovers
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    CloseClipboard();
                }
            }
            releaseAutomation();
        }).detach();
    } catch (...) {
        releaseAutomation();
        return {false, "Не удалось запустить отправку команды."};
    }

    return {true, std::move(successMessage)};
}

} // namespace

GameActionResult ClipboardGameIntegration::PrepareSpectate(const Report& report) {
    return SubmitCommandImpl("/sp " + std::to_string(report.playerId),
                         "Отправляю /sp " + std::to_string(report.playerId) + "…");
}

GameActionResult ClipboardGameIntegration::SendPrivateMessage(
    const Report& report, std::string_view text) {
    std::string command = "/pm " + std::to_string(report.playerId) + " " + std::string(text);
    return SendGameCommand(command);
}

GameActionResult ClipboardGameIntegration::SendGameCommand(const std::string& commandUtf8) {
    return SubmitCommandImpl(commandUtf8, "Отправляю команду: " + commandUtf8);
}

} // namespace reportpanel