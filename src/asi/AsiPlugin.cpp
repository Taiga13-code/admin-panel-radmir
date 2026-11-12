#include "../chatlog/ChatlogReader.h"
#include "../core/ReportManager.h"
#include "../game/ClipboardGameIntegration.h"
#include "../game/GameInputState.h"
#include "../settings/Settings.h"
#include "../ui/ReportPanelUi.h"

#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <d3d9.h>
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace reportpanel {
namespace {

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT,
    D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,
    D3DPRESENT_PARAMETERS*);
using ShowCursorFn = int(WINAPI*)(BOOL);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID,
    LPVOID*, LPUNKNOWN);
using DirectInputCreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*,
    REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using DirectInputGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInputDevice8A*, DWORD, LPVOID);
using DirectInputGetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirect3D9*, UINT, D3DDEVTYPE,
    HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice9*);
HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

Direct3DCreate9Fn g_direct3DCreate9 = nullptr;
CreateDeviceFn g_createDevice = nullptr;
EndSceneFn g_endScene = nullptr;
ResetFn g_reset = nullptr;
ShowCursorFn g_showCursor = nullptr;
ClipCursorFn g_clipCursor = nullptr;
SetCursorPosFn g_setCursorPos = nullptr;
SetCursorFn g_setCursor = nullptr;
GetAsyncKeyStateFn g_getAsyncKeyState = nullptr;
GetKeyStateFn g_getKeyState = nullptr;
GetKeyboardStateFn g_getKeyboardState = nullptr;
DirectInput8CreateFn g_directInput8Create = nullptr;
DirectInputCreateDeviceFn g_directInputCreateDevice = nullptr;

struct InputDeviceHook {
    bool mouse{false};
    bool keyboard{false};
    DirectInputGetDeviceStateFn getState{nullptr};
    DirectInputGetDeviceDataFn getData{nullptr};
};

struct DirectInputTargetHook {
    DirectInputGetDeviceStateFn getState{nullptr};
    DirectInputGetDeviceDataFn getData{nullptr};
};

std::atomic<bool> g_panelVisible{false};
std::atomic<bool> g_deviceHooksInstalled{false};
std::atomic<int> g_panelToggleKey{VK_F11};
std::atomic<bool> g_panelToggleDown{false};
std::atomic<bool> g_automationKeyPassthrough{false};
std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<std::uintptr_t> g_layoutOverride{0};
std::atomic<bool> g_capsLockOn{false};
std::atomic<bool> g_numLockOn{false};
std::atomic<bool> g_scrollLockOn{false};
std::atomic<DWORD> g_keyboardHookThreadId{0};
std::atomic<std::uintptr_t> g_keyboardHookModule{0};
thread_local bool t_passthroughInputQueries = false;
HHOOK g_keyboardHook{nullptr};
std::array<std::atomic<bool>, 256> g_pressedKeys{};
std::mutex g_runtimeMutex;
std::mutex g_cursorStateMutex;
std::mutex g_inputDeviceMutex;
std::mutex g_pendingTextMutex;
std::deque<unsigned int> g_pendingTextCharacters;
// Время последнего символа, который перевёл низкоуровневый хук.
// Оконный WM_CHAR того же нажатия считается дубликатом и отбрасывается:
// окно игры ANSI(MBCS), и бэкенд ImGui перекодирует ANSI-байт через
// keyboard code page, из-за чего кириллица превращалась в мусор.
ULONGLONG g_lastTranslatedCharacterTick = 0; // под g_pendingTextMutex
std::mutex g_inputLogMutex;
std::ofstream g_inputLog;
struct PendingKeyEvent { ImGuiKey key; bool down; };
std::deque<PendingKeyEvent> g_pendingKeyEvents;
std::array<bool, 256> g_blockedTextKeys{}; // Only accessed by the low-level hook thread.
std::unordered_map<IDirectInputDevice8A*, InputDeviceHook> g_inputDevices;
std::unordered_map<void*, DirectInputTargetHook> g_directInputTargetHooks;
int g_cursorCountBeforePanel = 0;
RECT g_clipRectBeforePanel{};
bool g_restoreClipRect = false;

constexpr UINT kRefreshKeyboardHookMessage = WM_APP + 0x51;

struct ScopedInputQueryPassthrough {
    ScopedInputQueryPassthrough() : previous_(t_passthroughInputQueries) {
        t_passthroughInputQueries = true;
    }
    ~ScopedInputQueryPassthrough() { t_passthroughInputQueries = previous_; }
    ScopedInputQueryPassthrough(const ScopedInputQueryPassthrough&) = delete;
    ScopedInputQueryPassthrough& operator=(const ScopedInputQueryPassthrough&) = delete;
    bool previous_;
};


bool QueueTranslatedTextCharacter(const KBDLLHOOKSTRUCT& key);
void CycleInputLayout();
void QueueImGuiKeyEvent(ImGuiKey key, bool down);
bool IsKeyDown(int key);
int ResolveVirtualKey(WPARAM key, LPARAM lParam);
void OpenInputLog();
void LogInput(const char* format, ...);

constexpr std::streamoff kInputLogMaxSize = 256 * 1024;

void OpenInputLog() {
    std::lock_guard lock(g_inputLogMutex);
    g_inputLog.close();
    g_inputLog.clear();
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LogInput), &module) ||
        module == nullptr) {
        return;
    }
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(module, modulePath,
            static_cast<DWORD>(std::size(modulePath))) == 0) {
        return;
    }
    std::filesystem::path logPath{modulePath};
    logPath.replace_filename(L"ReportPanel_input.log");
    g_inputLog.open(logPath, std::ios::out | std::ios::trunc);
}

void LogInput(const char* format, ...) {
    std::lock_guard lock(g_inputLogMutex);
    if (!g_inputLog.is_open()) return;
    char line[512]{};
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length > 0) {
        g_inputLog.write(line, std::min<std::size_t>(
            static_cast<std::size_t>(length), sizeof(line) - 1));
    }
    g_inputLog.put('\n');
    g_inputLog.flush();
    if (g_inputLog.tellp() >= kInputLogMaxSize) g_inputLog.close();
}

int WINAPI ShowCursorHook(BOOL show) {
    if (g_panelVisible.load(std::memory_order_relaxed)) return 1;
    return g_showCursor ? g_showCursor(show) : 0;
}

BOOL WINAPI ClipCursorHook(const RECT* rectangle) {
    if (g_panelVisible.load(std::memory_order_relaxed)) return TRUE;
    return g_clipCursor ? g_clipCursor(rectangle) : FALSE;
}

BOOL WINAPI SetCursorPosHook(int x, int y) {
    if (g_panelVisible.load(std::memory_order_relaxed)) return TRUE;
    return g_setCursorPos ? g_setCursorPos(x, y) : FALSE;
}

HCURSOR WINAPI SetCursorHook(HCURSOR cursor) {
    if (g_panelVisible.load(std::memory_order_relaxed)) {
        cursor = LoadCursorW(nullptr, IDC_ARROW);
    }
    return g_setCursor ? g_setCursor(cursor) : cursor;
}

bool IsAutomationKey(int key) {
    return key == 'T' || key == 'V' || key == VK_CONTROL || key == VK_RETURN;
}

bool IsAutomationDirectInputKey(int key) {
    return key == DIK_T || key == DIK_V || key == DIK_LCONTROL ||
           key == DIK_RCONTROL || key == DIK_RETURN;
}

bool IsPanelApiKeyAllowed(int key) {
    if (!g_panelVisible.load(std::memory_order_relaxed)) return true;
    // Commands sent by the panel use injected T / Ctrl+V / Enter. The game
    // polls these APIs while executing the command, so they must stay visible
    // to it during the whole automation window.
    if ((g_gameInputAutomationActive.load(std::memory_order_relaxed) ||
         g_automationKeyPassthrough.load(std::memory_order_relaxed)) &&
        IsAutomationKey(key)) {
        return true;
    }
    return false;
}

bool IsPanelDirectInputKeyAllowed(int key) {
    if (!g_panelVisible.load(std::memory_order_relaxed)) return true;
    if (g_gameInputAutomationActive.load(std::memory_order_relaxed) &&
        IsAutomationDirectInputKey(key)) return true;
    return false;
}

bool IsGameProcessForeground(HWND gameWindow) {
    if (!gameWindow) return false;
    DWORD gameProcess = 0;
    GetWindowThreadProcessId(gameWindow, &gameProcess);
    const HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    return gameProcess != 0 && foregroundProcess == gameProcess;
}

ImGuiKey ImGuiKeyFromVirtualKey(DWORD vk, DWORD scanCode, DWORD flags) {
    if (vk >= '0' && vk <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F24)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));

    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return (flags & LLKHF_EXTENDED) != 0
        ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock;
    case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_BROWSER_BACK: return ImGuiKey_AppBack;
    case VK_BROWSER_FORWARD: return ImGuiKey_AppForward;
    case VK_OEM_102: return ImGuiKey_Oem102;
    case VK_SHIFT: {
        const UINT resolved = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        return resolved == VK_RSHIFT ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
    }
    case VK_CONTROL: return (flags & LLKHF_EXTENDED) != 0
        ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
    case VK_MENU: return (flags & LLKHF_EXTENDED) != 0
        ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

void QueueImGuiKeyEvent(ImGuiKey key, bool down) {
    std::lock_guard lock(g_pendingTextMutex);
    if (key != ImGuiKey_None) g_pendingKeyEvents.push_back({key, down});
    g_pendingKeyEvents.push_back({ImGuiMod_Ctrl,
        IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL)});
    g_pendingKeyEvents.push_back({ImGuiMod_Shift,
        IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT)});
    g_pendingKeyEvents.push_back({ImGuiMod_Alt,
        IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU)});
    g_pendingKeyEvents.push_back({ImGuiMod_Super,
        IsKeyDown(VK_LWIN) || IsKeyDown(VK_RWIN)});
}

void UpdateToggleState(DWORD virtualKey, bool down) {
    if (!down) return;
    switch (virtualKey) {
    case VK_CAPITAL:
        g_capsLockOn.store(!g_capsLockOn.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        break;
    case VK_NUMLOCK:
        g_numLockOn.store(!g_numLockOn.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
        break;
    case VK_SCROLL:
        g_scrollLockOn.store(!g_scrollLockOn.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void RequestKeyboardHookRefresh() {
    const DWORD threadId = g_keyboardHookThreadId.load(std::memory_order_relaxed);
    if (threadId != 0) PostThreadMessageW(threadId, kRefreshKeyboardHookMessage, 0, 0);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM message, LPARAM data) {
    if (code < HC_ACTION)
        return CallNextHookEx(g_keyboardHook, code, message, data);

    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    if (!key) return CallNextHookEx(g_keyboardHook, code, message, data);

    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    const LPARAM keyLParam = (static_cast<LPARAM>(key->scanCode & 0xFF) << 16) |
        ((key->flags & LLKHF_EXTENDED) ? (1LL << 24) : 0) |
        (up ? ((1LL << 30) | (1LL << 31)) : 0);
    const int trackedKey = ResolveVirtualKey(key->vkCode, keyLParam);
    bool wasDown = false;
    if ((down || up) && trackedKey >= 0 && trackedKey < 256) {
        auto& flag = g_pressedKeys[static_cast<std::size_t>(trackedKey)];
        wasDown = flag.load(std::memory_order_relaxed);
        flag.store(down, std::memory_order_relaxed);
    }
    if (!wasDown) UpdateToggleState(key->vkCode, down);

    if (up && key->vkCode < g_blockedTextKeys.size() &&
        g_blockedTextKeys[key->vkCode]) {
        g_blockedTextKeys[key->vkCode] = false;
        // Не прерываем здесь: пусть код ниже передаст KeyUp в ImGui,
        // иначе клавиши вроде Backspace "залипают" (ImGui не получит
        // событие отпускания и будет стирать текст бесконечно).
    }
    if (!g_panelVisible.load(std::memory_order_relaxed)) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }

    const HWND gameWindow = g_gameWindow.load(std::memory_order_relaxed);
    if (!IsGameProcessForeground(gameWindow)) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }
    // Keys injected by the panel command sender must always reach the game.
    if ((key->flags & LLKHF_INJECTED) != 0 &&
        key->dwExtraInfo == kGameInputAutomationTag) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }
    if (static_cast<int>(key->vkCode) == g_panelToggleKey.load(std::memory_order_relaxed) ||
        (key->vkCode == VK_TAB && (key->flags & LLKHF_ALTDOWN) != 0) ||
        key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }

    const bool textInput = g_panelTextInputActive.load(std::memory_order_relaxed);
    if (textInput && (down || up)) {
        // While a text field is active the key is forwarded to the next hook
        // instead of being swallowed. AutoHotkey and similar tools only trigger
        // hotstrings/hotkeys when they still receive the physical keystrokes,
        // and their injected replacement text then arrives here as VK_PACKET
        // (or as ordinary WM_CHAR for the game, which WindowMessage() keeps
        // away from it). Printed characters are translated below, because the
        // window is ANSI and its WM_CHAR only carries a single ANSI byte.
        // Blocking here (the previous behaviour) silently disabled every AHK
        // hotstring because the AHK hook never saw the typed characters.
        if (down) {
            if (key->vkCode == VK_PACKET) {
                // AHK / SendInput Unicode text: scanCode carries the UTF-16
                // unit and ToUnicodeEx() cannot translate it.
                if (key->scanCode != 0) {
                    {
                        std::lock_guard lock(g_pendingTextMutex);
                        g_pendingTextCharacters.push_back(key->scanCode);
                        g_lastTranslatedCharacterTick = GetTickCount64();
                    }
                    LogInput("PACKET ch=%04X",
                        static_cast<unsigned int>(key->scanCode));
                }
            } else {
                if (!wasDown) {
                    const bool shiftKey = key->vkCode == VK_SHIFT ||
                        key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT;
                    const bool altKey = key->vkCode == VK_MENU ||
                        key->vkCode == VK_LMENU || key->vkCode == VK_RMENU;
                    const bool ctrlKey = key->vkCode == VK_CONTROL ||
                        key->vkCode == VK_LCONTROL || key->vkCode == VK_RCONTROL;
                    if (shiftKey || altKey || ctrlKey) {
                        const bool altHeld = (key->flags & LLKHF_ALTDOWN) != 0 ||
                            IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU);
                        const bool shiftHeld =
                            IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT);
                        const bool ctrlHeld =
                            IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL);
                        LogInput("MOD vk=%02X alt=%d shift=%d ctrl=%d",
                            static_cast<unsigned int>(key->vkCode),
                            static_cast<int>(altHeld), static_cast<int>(shiftHeld),
                            static_cast<int>(ctrlHeld));
                        // Alt+Shift is the system layout hotkey (Ctrl+Shift for
                        // some users) and it cannot work while the hook stops
                        // gameplay keys, so switch the layout here.
                        static ULONGLONG lastCycleTick = 0; // Hook thread only.
                        const ULONGLONG now = GetTickCount64();
                        const bool cycle = (shiftKey && (altHeld || ctrlHeld)) ||
                            (altKey && shiftHeld);
                        if (cycle && now - lastCycleTick >= 150) {
                            lastCycleTick = now;
                            CycleInputLayout();
                        }
                    }
                }
                // The hook is the only source of typed characters: the window
                // message path mangles them (see g_lastTranslatedCharacterTick).
                (void)QueueTranslatedTextCharacter(*key);
            }
        }
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }

    // The panel is open but no text field is active: keep every physical
    // gameplay key out of the game so m/i/o/p/k/j and other hotkeys cannot
    // open menus. Their window messages never appear because the key was
    // stopped here. Injected keys (AutoHotkey and similar tools) are
    // forwarded instead — the game's window layer then lets their replacement
    // text into the game chat. Blocking them here silently disabled every
    // AHK hotkey/hotstring while the panel was open.
    if ((key->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }
    // Modifier keys and lock toggles must reach the rest of the hook chain:
    // swallowing them broke the system input-layout hotkey (Alt+Shift).
    const bool modifierOrToggle =
        key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT ||
        key->vkCode == VK_RSHIFT || key->vkCode == VK_MENU ||
        key->vkCode == VK_LMENU || key->vkCode == VK_RMENU ||
        key->vkCode == VK_CONTROL || key->vkCode == VK_LCONTROL ||
        key->vkCode == VK_RCONTROL || key->vkCode == VK_LWIN ||
        key->vkCode == VK_RWIN || key->vkCode == VK_CAPITAL ||
        key->vkCode == VK_NUMLOCK || key->vkCode == VK_SCROLL;
    const bool altOrCtrlHeld = (key->flags & LLKHF_ALTDOWN) != 0 ||
        IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU) ||
        IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL);
    if (modifierOrToggle || altOrCtrlHeld) {
        return CallNextHookEx(g_keyboardHook, code, message, data);
    }
    return 1;
}

DWORD WINAPI KeyboardHookThread(LPVOID module) {
    g_capsLockOn.store((::GetAsyncKeyState(VK_CAPITAL) & 0x0001) != 0,
                       std::memory_order_relaxed);
    g_numLockOn.store((::GetAsyncKeyState(VK_NUMLOCK) & 0x0001) != 0,
                      std::memory_order_relaxed);
    g_scrollLockOn.store((::GetAsyncKeyState(VK_SCROLL) & 0x0001) != 0,
                         std::memory_order_relaxed);
    g_keyboardHookModule.store(reinterpret_cast<std::uintptr_t>(module),
                               std::memory_order_relaxed);
    g_keyboardHookThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const auto installHook = [module] {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
            reinterpret_cast<HINSTANCE>(module), 0);
    };
    installHook();
    if (!g_keyboardHook) return 1;

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kRefreshKeyboardHookMessage) {
            // Re-register so our hook is first in the low-level chain again;
            // other ASI modules may have installed theirs after us.
            if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = nullptr;
            installHook();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = nullptr;
    g_keyboardHookThreadId.store(0, std::memory_order_relaxed);
    return 0;
}

int ResolveVirtualKey(WPARAM key, LPARAM lParam) {    if (key == VK_SHIFT) {
        const auto scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
        const UINT resolved = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        return resolved == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    }
    if (key == VK_CONTROL)
        return (lParam & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU)
        return (lParam & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    return static_cast<int>(key);
}

bool IsKeyDown(int key) {
    return key >= 0 && key < static_cast<int>(g_pressedKeys.size()) &&
           g_pressedKeys[static_cast<std::size_t>(key)].load(std::memory_order_relaxed);
}

void TrackPhysicalKey(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
        message != WM_KEYUP && message != WM_SYSKEYUP) return;
    const int key = ResolveVirtualKey(wParam, lParam);
    if (key >= 0 && key < static_cast<int>(g_pressedKeys.size())) {
        g_pressedKeys[static_cast<std::size_t>(key)].store(
            message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
            std::memory_order_relaxed);
    }
}

HKL ResolveInputLayout() {
    HKL layout = nullptr;
    if (const std::uintptr_t overridden =
            g_layoutOverride.load(std::memory_order_relaxed)) {
        layout = reinterpret_cast<HKL>(overridden);
    } else {
        if (const HWND foreground = GetForegroundWindow()) {
            const DWORD threadId = GetWindowThreadProcessId(foreground, nullptr);
            if (threadId != 0) layout = GetKeyboardLayout(threadId);
        }
        if (!layout) {
            if (const HWND window = g_gameWindow.load(std::memory_order_relaxed)) {
                const DWORD threadId = GetWindowThreadProcessId(window, nullptr);
                if (threadId != 0) layout = GetKeyboardLayout(threadId);
            }
        }
        if (!layout) layout = GetKeyboardLayout(0);
    }
    if (layout) {
        g_effectiveInputLangId.store(
            static_cast<unsigned int>(LOWORD(reinterpret_cast<std::uintptr_t>(layout))),
            std::memory_order_relaxed);
    }
    return layout;
}

// Cycles the input layout from inside the plugin: while the panel is open the
// low-level hook stops gameplay keys, and other hooks (anticheat / launcher)
// may break the system Alt+Shift hotkey as well.
void CycleInputLayout() {
    HKL layouts[16]{};
    const int count = GetKeyboardLayoutList(
        static_cast<int>(std::size(layouts)), layouts);
    if (count <= 0) return;

    HKL systemLayout = nullptr;
    if (const HWND foreground = GetForegroundWindow()) {
        systemLayout = GetKeyboardLayout(
            GetWindowThreadProcessId(foreground, nullptr));
    }
    const std::uintptr_t current =
        reinterpret_cast<std::uintptr_t>(ResolveInputLayout());
    int index = -1;
    for (int i = 0; i < count; ++i) {
        if (reinterpret_cast<std::uintptr_t>(layouts[i]) == current) {
            index = i;
            break;
        }
    }
    const HKL next = layouts[(index + 1) % count];
    g_layoutOverride.store(reinterpret_cast<std::uintptr_t>(next),
        std::memory_order_relaxed);
    g_effectiveInputLangId.store(
        static_cast<unsigned int>(LOWORD(reinterpret_cast<std::uintptr_t>(next))),
        std::memory_order_relaxed);
    LogInput("LAYOUT next=%p sys=%p count=%d", reinterpret_cast<void*>(next),
        reinterpret_cast<void*>(systemLayout), count);

    if (const HWND window = g_gameWindow.load(std::memory_order_relaxed)) {
        PostMessageW(window, WM_INPUTLANGCHANGEREQUEST, 0,
            reinterpret_cast<LPARAM>(next));
    }
}

UINT KeyboardCodePageForLayout(HKL layout) {
    const LCID lcid = MAKELCID(HIWORD(layout), SORT_DEFAULT);
    DWORD codePage = 0;
    if (GetLocaleInfoA(lcid, LOCALE_RETURN_NUMBER | LOCALE_IDEFAULTANSICODEPAGE,
            reinterpret_cast<LPSTR>(&codePage),
            static_cast<int>(sizeof(codePage))) == 0 ||
        codePage == 0) {
        return CP_ACP;
    }
    return codePage;
}

bool HookTranslatedCharacterRecent() {
    std::lock_guard lock(g_pendingTextMutex);
    return g_lastTranslatedCharacterTick != 0 &&
        GetTickCount64() - g_lastTranslatedCharacterTick <= 500;
}

unsigned int DecodeWindowCharacter(HWND window, UINT message, WPARAM wParam) {
    if (message == WM_UNICHAR)
        return wParam == UNICODE_NOCHAR ? 0u
                                        : static_cast<unsigned int>(wParam);
    if (IsWindowUnicode(window) || wParam > 0xFF)
        return static_cast<unsigned int>(wParam);
    // ANSI-окно: один байт активной кодовой страницы раскладки.
    wchar_t wide[4]{};
    const int count = MultiByteToWideChar(KeyboardCodePageForLayout(ResolveInputLayout()),
        MB_PRECOMPOSED, reinterpret_cast<const char*>(&wParam), 1, wide,
        static_cast<int>(std::size(wide)));
    return count > 0 ? static_cast<unsigned int>(wide[0]) : 0u;
}

bool QueueTranslatedTextCharacter(const KBDLLHOOKSTRUCT& key) {
    // The low-level hook runs on its own thread and has no keyboard state of
    // its own, so build it from the async state and the keys seen by the hook.
    BYTE keyboardState[256]{};
    if (g_getAsyncKeyState) {
        for (int vk = 0; vk < 256; ++vk) {
            if ((g_getAsyncKeyState(vk) & 0x8000) != 0)
                keyboardState[vk] |= 0x80;
        }
    }
    for (std::size_t vk = 0; vk < g_pressedKeys.size(); ++vk) {
        if (g_pressedKeys[vk].load(std::memory_order_relaxed))
            keyboardState[vk] |= 0x80;
    }

    // Caps Lock / Num Lock toggles are tracked by the hook (GetKeyState() is
    // useless on this thread), so merge them into the state manually.
    const auto applyToggle = [&](int vk, bool on) {
        keyboardState[vk] = static_cast<BYTE>(
            (keyboardState[vk] & 0x80) | (on ? 0x01 : 0x00));
    };
    applyToggle(VK_CAPITAL, g_capsLockOn.load(std::memory_order_relaxed));
    applyToggle(VK_NUMLOCK, g_numLockOn.load(std::memory_order_relaxed));
    applyToggle(VK_SCROLL, g_scrollLockOn.load(std::memory_order_relaxed));

    if (key.vkCode < std::size(keyboardState))
        keyboardState[key.vkCode] |= 0x80;

    const bool ctrl = (keyboardState[VK_CONTROL] & 0x80) != 0 ||
        (keyboardState[VK_LCONTROL] & 0x80) != 0 ||
        (keyboardState[VK_RCONTROL] & 0x80) != 0;
    const bool alt = (keyboardState[VK_MENU] & 0x80) != 0 ||
        (keyboardState[VK_LMENU] & 0x80) != 0 ||
        (keyboardState[VK_RMENU] & 0x80) != 0 ||
        (key.flags & LLKHF_ALTDOWN) != 0;
    // AltGr reports Ctrl+Alt; any other combination is a hotkey, not text.
    if (ctrl != alt) return false;

    const HKL layout = ResolveInputLayout();
    wchar_t translated[4]{};
    int length = ToUnicodeEx(key.vkCode, key.scanCode, keyboardState,
        translated, static_cast<int>(std::size(translated)), 0, layout);
    if (length < 0) {
        // Dead key: translate once more to flush it and clear the kernel state,
        // otherwise the next keystroke gets combined with the pending accent.
        length = ToUnicodeEx(key.vkCode, key.scanCode, keyboardState,
            translated, static_cast<int>(std::size(translated)), 0, layout);
    }
    if (length <= 0) return false;

    unsigned int codepoints[std::size(translated)]{};
    int count = 0;
    for (int index = 0; index < length; ++index) {
        unsigned int codepoint = static_cast<unsigned short>(translated[index]);
        if (codepoint < 0x20) continue;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && index + 1 < length) {
            const unsigned int low = static_cast<unsigned short>(translated[index + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }
        codepoints[count++] = codepoint;
    }
    if (count == 0) return false;

    {
        std::lock_guard lock(g_pendingTextMutex);
        for (int index = 0; index < count; ++index)
            g_pendingTextCharacters.push_back(codepoints[index]);
        g_lastTranslatedCharacterTick = GetTickCount64();
    }
    for (int index = 0; index < count; ++index) {
        LogInput("HOOK vk=%02X scan=%02X len=%d ch=%04X hkl=%04X",
            static_cast<unsigned int>(key.vkCode),
            static_cast<unsigned int>(key.scanCode), length, codepoints[index],
            static_cast<unsigned int>(LOWORD(reinterpret_cast<std::uintptr_t>(layout))));
    }
    return true;
}

void DrainPendingKeyboardInput(ImGuiIO& io) {
    std::deque<PendingKeyEvent> keyEvents;
    std::deque<unsigned int> characters;
    {
        std::lock_guard lock(g_pendingTextMutex);
        keyEvents.swap(g_pendingKeyEvents);
        characters.swap(g_pendingTextCharacters);
    }
    for (const auto& event : keyEvents) io.AddKeyEvent(event.key, event.down);
    for (const unsigned int codepoint : characters) io.AddInputCharacter(codepoint);
    // Застрявший Ctrl заставляет ImGui отбрасывать все введённые символы
    // (ignore_char_inputs в InputText), из-за чего поле молча не набиралось.
    if (io.KeyCtrl && !io.KeyAlt && !IsKeyDown(VK_CONTROL) &&
        !IsKeyDown(VK_LCONTROL) && !IsKeyDown(VK_RCONTROL)) {
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.KeyCtrl = false;
    }
    if (!characters.empty() || !keyEvents.empty()) {
        LogInput("DRAIN chars=%zu keys=%zu", characters.size(), keyEvents.size());
    }
}

void ClearTranslatedTextCharacters() {
    std::lock_guard lock(g_pendingTextMutex);
    g_pendingTextCharacters.clear();
    g_pendingKeyEvents.clear();
}

bool IsTaggedAutomationMessage(UINT message) {
    const bool keyboardMessage = message == WM_CHAR || message == WM_SYSCHAR ||
        message == WM_UNICHAR ||
        message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    return keyboardMessage &&
        static_cast<std::uintptr_t>(GetMessageExtraInfo()) == kGameInputAutomationTag;
}

bool IsPanelKeyAllowed(int key) {
    return IsPanelApiKeyAllowed(key);
}

void SyncToggleKeyStates() {
    // Must be called from the game's window thread: GetKeyState() reflects the
    // toggle state of the thread that owns the message queue.
    const auto query = g_getKeyState ? g_getKeyState : ::GetKeyState;
    g_capsLockOn.store((query(VK_CAPITAL) & 0x0001) != 0, std::memory_order_relaxed);
    g_numLockOn.store((query(VK_NUMLOCK) & 0x0001) != 0, std::memory_order_relaxed);
    g_scrollLockOn.store((query(VK_SCROLL) & 0x0001) != 0, std::memory_order_relaxed);
    // Потерянный keyup модификатора не должен оставлять его зажатым.
    constexpr int kModifierKeys[] = {
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL,
        VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
    };
    for (const int key : kModifierKeys) {
        g_pressedKeys[static_cast<std::size_t>(key)].store(
            false, std::memory_order_relaxed);
    }
}

SHORT WINAPI GetAsyncKeyStateHook(int key) {
    if (t_passthroughInputQueries) {
        return g_getAsyncKeyState ? g_getAsyncKeyState(key) : 0;
    }
    if (!IsPanelKeyAllowed(key)) return 0;
    return g_getAsyncKeyState ? g_getAsyncKeyState(key) : 0;
}

SHORT WINAPI GetKeyStateHook(int key) {
    if (t_passthroughInputQueries) {
        return g_getKeyState ? g_getKeyState(key) : 0;
    }
    if (!IsPanelKeyAllowed(key)) return 0;
    return g_getKeyState ? g_getKeyState(key) : 0;
}

BOOL WINAPI GetKeyboardStateHook(PBYTE state) {
    if (!g_getKeyboardState) return FALSE;
    const BOOL result = g_getKeyboardState(state);
    if (result && state && !t_passthroughInputQueries &&
        g_panelVisible.load(std::memory_order_relaxed)) {
        for (int key = 0; key < 256; ++key) {
            if (!IsPanelApiKeyAllowed(key)) state[key] = 0;
        }
    }
    return result;
}

void SetPanelVisible(bool visible) {
    const bool wasVisible = g_panelVisible.load(std::memory_order_relaxed);
    if (visible == wasVisible) return;

    std::scoped_lock lock(g_cursorStateMutex);
    ClearTranslatedTextCharacters();
    if (visible) {
        g_panelTextInputActive.store(false, std::memory_order_relaxed);
        RECT desktop{};
        RECT clip{};
        GetWindowRect(GetDesktopWindow(), &desktop);
        g_restoreClipRect = GetClipCursor(&clip) != FALSE &&
            (clip.left != desktop.left || clip.top != desktop.top ||
             clip.right != desktop.right || clip.bottom != desktop.bottom);
        g_clipRectBeforePanel = clip;
        g_panelVisible.store(true, std::memory_order_relaxed);
        RequestKeyboardHookRefresh();

        const auto showCursor = g_showCursor ? g_showCursor : ::ShowCursor;
        int count = showCursor(TRUE);
        g_cursorCountBeforePanel = count - 1;
        for (int attempts = 0; count < 0 && attempts < 64; ++attempts) {
            count = showCursor(TRUE);
        }
        if (g_clipCursor) g_clipCursor(nullptr);
        else ::ClipCursor(nullptr);
        ::SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }

    g_panelTextInputActive.store(false, std::memory_order_relaxed);
    g_panelVisible.store(false, std::memory_order_relaxed);
    if (g_layoutOverride.load(std::memory_order_relaxed) != 0) {
        g_layoutOverride.store(0, std::memory_order_relaxed);
        LogInput("LAYOUT reset (panel hidden)");
    }
    const auto showCursor = g_showCursor ? g_showCursor : ::ShowCursor;
    int count = showCursor(FALSE);
    for (int attempts = 0; count > g_cursorCountBeforePanel && attempts < 64; ++attempts) {
        count = showCursor(FALSE);
    }
    for (int attempts = 0; count < g_cursorCountBeforePanel && attempts < 64; ++attempts) {
        count = showCursor(TRUE);
    }
    if (g_restoreClipRect) {
        if (g_clipCursor) g_clipCursor(&g_clipRectBeforePanel);
        else ::ClipCursor(&g_clipRectBeforePanel);
    }
    g_restoreClipRect = false;
}

InputDeviceHook FindInputDevice(IDirectInputDevice8A* device) {
    std::scoped_lock lock(g_inputDeviceMutex);
    const auto it = g_inputDevices.find(device);
    return it == g_inputDevices.end() ? InputDeviceHook{} : it->second;
}

HRESULT STDMETHODCALLTYPE DirectInputGetDeviceStateHook(
    IDirectInputDevice8A* device, DWORD size, LPVOID state) {
    const InputDeviceHook info = FindInputDevice(device);
    if (!info.getState) return DIERR_NOTINITIALIZED;
    const HRESULT result = info.getState(device, size, state);
    if (SUCCEEDED(result) && state && g_panelVisible.load(std::memory_order_relaxed)) {
        if (info.mouse) {
            std::memset(state, 0, size);
        } else if (info.keyboard) {
            auto* keys = static_cast<unsigned char*>(state);
            for (DWORD key = 0; key < std::min<DWORD>(size, 256); ++key) {
                if (!IsPanelDirectInputKeyAllowed(static_cast<int>(key)))
                    keys[key] = 0;
            }
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DirectInputGetDeviceDataHook(
    IDirectInputDevice8A* device, DWORD objectSize,
    LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const InputDeviceHook info = FindInputDevice(device);
    if (!info.getData) return DIERR_NOTINITIALIZED;
    const HRESULT result = info.getData(device, objectSize, data, count, flags);
    if (SUCCEEDED(result) && count && g_panelVisible.load(std::memory_order_relaxed)) {
        if (info.mouse) {
            *count = 0;
        } else if (info.keyboard) {
            if (data && objectSize >= sizeof(DIDEVICEOBJECTDATA)) {
                DWORD kept = 0;
                for (DWORD index = 0; index < *count; ++index) {
                    if (IsPanelDirectInputKeyAllowed(
                            static_cast<int>(data[index].dwOfs)))
                        data[kept++] = data[index];
                }
                *count = kept;
            } else {
                *count = 0;
            }
        }
    }
    return result;
}

void InstallInputDeviceHooks(IDirectInputDevice8A* device, bool mouse, bool keyboard) {
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    void* const stateTarget = table[9];
    void* const dataTarget = table[10];

    std::scoped_lock lock(g_inputDeviceMutex);

    InputDeviceHook& entry = g_inputDevices[device];
    entry.mouse = mouse;
    entry.keyboard = keyboard;

    // DirectInput shares the same GetDeviceState/GetDeviceData vtable entries
    // between keyboard and mouse devices. Hook each target address once and
    // reuse the same trampoline for every device, otherwise the second device
    // overwrites the original pointer with a wrong one.
    auto stateIt = g_directInputTargetHooks.find(stateTarget);
    if (stateIt == g_directInputTargetHooks.end()) {
        DirectInputGetDeviceStateFn original = nullptr;
        if (MH_CreateHook(stateTarget,
                reinterpret_cast<LPVOID>(&DirectInputGetDeviceStateHook),
                reinterpret_cast<void**>(&original)) == MH_OK) {
            MH_EnableHook(stateTarget);
            DirectInputTargetHook targets;
            targets.getState = original;
            stateIt = g_directInputTargetHooks.emplace(stateTarget, targets).first;
        }
    }
    if (stateIt != g_directInputTargetHooks.end()) {
        entry.getState = stateIt->second.getState;
    }

    auto dataIt = g_directInputTargetHooks.find(dataTarget);
    if (dataIt == g_directInputTargetHooks.end()) {
        DirectInputGetDeviceDataFn original = nullptr;
        if (MH_CreateHook(dataTarget,
                reinterpret_cast<LPVOID>(&DirectInputGetDeviceDataHook),
                reinterpret_cast<void**>(&original)) == MH_OK) {
            MH_EnableHook(dataTarget);
            DirectInputTargetHook targets;
            targets.getData = original;
            dataIt = g_directInputTargetHooks.emplace(dataTarget, targets).first;
        }
    }
    if (dataIt != g_directInputTargetHooks.end()) {
        entry.getData = dataIt->second.getData;
    }
}

HRESULT STDMETHODCALLTYPE DirectInputCreateDeviceHook(
    IDirectInput8A* input, REFGUID instance,
    LPDIRECTINPUTDEVICE8A* device, LPUNKNOWN outer) {
    if (!g_directInputCreateDevice) return DIERR_NOTINITIALIZED;
    const HRESULT result = g_directInputCreateDevice(input, instance, device, outer);
    if (SUCCEEDED(result) && device && *device) {
        if (IsEqualGUID(instance, GUID_SysMouse))
            InstallInputDeviceHooks(*device, true, false);
        else if (IsEqualGUID(instance, GUID_SysKeyboard))
            InstallInputDeviceHooks(*device, false, true);
    }
    return result;
}

HRESULT WINAPI DirectInput8CreateHook(HINSTANCE instance, DWORD version,
    REFIID interfaceId, LPVOID* output, LPUNKNOWN outer) {
    if (!g_directInput8Create) return DIERR_NOTINITIALIZED;
    const HRESULT result = g_directInput8Create(
        instance, version, interfaceId, output, outer);
    if (SUCCEEDED(result) && output && *output) {
        auto* directInput = static_cast<IDirectInput8A*>(*output);
        void** table = *reinterpret_cast<void***>(directInput);
        if (MH_CreateHook(table[3],
                reinterpret_cast<LPVOID>(&DirectInputCreateDeviceHook),
                reinterpret_cast<void**>(&g_directInputCreateDevice)) == MH_OK) {
            MH_EnableHook(table[3]);
        }
    }
    return result;
}

constexpr auto kHeartbeatInterval = std::chrono::seconds(5);
constexpr auto kReportRetention = std::chrono::minutes(15) + kHeartbeatInterval;

std::int64_t UnixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void WriteHeartbeat(const std::filesystem::path& path) {
    try {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file) file << UnixSecondsNow() << '\n';
    } catch (...) {
    }
}

bool SessionExpired(const std::filesystem::path& heartbeatPath,
                    const std::filesystem::path& reportsPath) {
    try {
        std::ifstream heartbeat(heartbeatPath, std::ios::binary);
        std::int64_t lastActive = 0;
        if (heartbeat >> lastActive) {
            const auto ageSeconds = UnixSecondsNow() - lastActive;
            return ageSeconds > std::chrono::duration_cast<std::chrono::seconds>(
                kReportRetention).count();
        }

        if (!std::filesystem::exists(reportsPath)) return false;
        const auto fileNow = std::filesystem::file_time_type::clock::now();
        const auto systemNow = std::chrono::system_clock::now();
        const auto modified = std::filesystem::last_write_time(reportsPath);
        const auto modifiedSystemTime = systemNow +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(modified - fileNow);
        return systemNow - modifiedSystemTime > kReportRetention;
    } catch (...) {
        return false;
    }
}

void StartHeartbeat(const std::filesystem::path& heartbeatPath) {
    WriteHeartbeat(heartbeatPath);
    std::thread([heartbeatPath] {
        for (;;) {
            std::this_thread::sleep_for(kHeartbeatInterval);
            WriteHeartbeat(heartbeatPath);
        }
    }).detach();
}

class PluginRuntime {
public:
    PluginRuntime() {
        const auto dataDirectory = SettingsStore::LocalDataDirectory();
        std::filesystem::create_directories(dataDirectory);
        settingsStore_ = std::make_unique<SettingsStore>(dataDirectory / L"settings.json");
        settings_ = settingsStore_->Load();
        const auto reportsPath = dataDirectory / L"reports.json";
        const auto heartbeatPath = dataDirectory / L"session.heartbeat";
        const bool expired = SessionExpired(heartbeatPath, reportsPath);
        reports_ = std::make_unique<ReportManager>(reportsPath);
        if (expired) reports_->ClearAll();
        StartHeartbeat(heartbeatPath);
        game_ = std::make_unique<ClipboardGameIntegration>();
        source_ = std::make_unique<ChatlogReader>(
            settings_.chatlogPath, dataDirectory / L"chatlog_cursor.json",
            [this](const ParsedEvent& event) { reports_->Apply(event); }, !expired);
        source_->SetAdminNick(settings_.adminNick);
        source_->Start();
    }

    ~PluginRuntime() {
        if (source_) source_->Stop();
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        if (window_ && originalWindowProc_) {
            SetWindowLongPtrW(window_, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(originalWindowProc_));
        }
    }

    void Render(IDirect3DDevice9* device) {
        if (!InitializeUi(device)) return;
        SaveSettingsIfDue();
        if (!g_panelVisible.load(std::memory_order_relaxed)) {
            panelWasVisible_ = false;
            textInputActivePrevious_ = false;
            return;
        }

        const bool textInputNow = g_panelTextInputActive.load(std::memory_order_relaxed);
        if (!panelWasVisible_ || (textInputNow && !textInputActivePrevious_)) {
            // Another ASI may have installed its low-level hook after ours.
            // Re-register when the panel or a text field becomes active.
            RequestKeyboardHookRefresh();
        }
        panelWasVisible_ = true;
        textInputActivePrevious_ = textInputNow;

        ImGui::GetStyle().FontSizeBase = static_cast<float>(settings_.fontSize);
        ImGui::GetStyle().Alpha = std::clamp(static_cast<float>(settings_.transparency) / 100.0f, 0.30f, 1.0f);
        g_panelToggleKey.store(settings_.toggleKey, std::memory_order_relaxed);
        ImGui_ImplDX9_NewFrame();
        {
            ScopedInputQueryPassthrough passthrough;
            ImGui_ImplWin32_NewFrame();
        }
        DrainPendingKeyboardInput(ImGui::GetIO());
        ImGui::NewFrame();
        ui_->Render();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    void BeforeReset() {
        if (ImGui::GetCurrentContext()) ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    void AfterReset(HRESULT result) {
        if (SUCCEEDED(result) && ImGui::GetCurrentContext()) {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
    }

    LRESULT WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        const auto forwardToGame = [&] {
            return originalWindowProc_
                ? CallWindowProcW(originalWindowProc_, window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
        };

        TrackPhysicalKey(message, wParam, lParam);
        if (message == WM_SETFOCUS || message == WM_ACTIVATE ||
            message == WM_ACTIVATEAPP) {
            SyncToggleKeyStates();
        }
        if (message == WM_KEYDOWN && static_cast<int>(wParam) == g_panelToggleKey.load(std::memory_order_relaxed)) {
            if (!g_panelToggleDown.exchange(true)) {
                const bool visible = !g_panelVisible.load();
                SyncToggleKeyStates();
                SetPanelVisible(visible);
                if (ui_) ui_->OnPanelVisibility(visible);
            }
            return 0;
        }
        if (message == WM_KEYUP && static_cast<int>(wParam) == g_panelToggleKey.load(std::memory_order_relaxed)) {
            g_panelToggleDown.store(false);
            return 0;
        }

        const bool panelVisible = g_panelVisible.load(std::memory_order_relaxed);

        // Let the ImGui backend refresh its ANSI keyboard code page after a
        // layout change, then let the game process the notification as well.
        if (message == WM_INPUTLANGCHANGE) {
            LogInput("LANGCHANGE hkl=%p", reinterpret_cast<void*>(lParam));
            if (g_layoutOverride.load(std::memory_order_relaxed) != 0 &&
                static_cast<std::uintptr_t>(lParam) ==
                    g_layoutOverride.load(std::memory_order_relaxed)) {
                // The system caught up with our override; from here on the
                // real layout is authoritative again.
                g_layoutOverride.store(0, std::memory_order_relaxed);
                LogInput("LANGCHANGE adopt");
            }
            if (ImGui::GetCurrentContext()) {
                ScopedInputQueryPassthrough passthrough;
                ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
            }
            return forwardToGame();
        }
        if (message == WM_INPUTLANGCHANGEREQUEST) return forwardToGame();

        const bool ansiImeChar = message == WM_IME_CHAR && !IsWindowUnicode(window);
        const bool keyboardMessage = message == WM_CHAR || message == WM_SYSCHAR ||
            message == WM_UNICHAR ||
            message == WM_KEYDOWN || message == WM_KEYUP ||
            message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
            ansiImeChar || message == WM_IME_COMPOSITION;
        const bool gameAutomationKey =
            g_gameInputAutomationActive.load(std::memory_order_relaxed) &&
            IsTaggedAutomationMessage(message);
        if (panelVisible && gameAutomationKey) {
            // Only the keys explicitly injected by the command sender reach GTA.
            // Physical typing remains captured even while a command is in flight.
            const bool wasPassing = g_automationKeyPassthrough.exchange(true,
                std::memory_order_relaxed);
            const LRESULT result = forwardToGame();
            g_automationKeyPassthrough.store(wasPassing, std::memory_order_relaxed);
            return result;
        }
        if (panelVisible && message == WM_SETCURSOR) {
            ::SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (!gameAutomationKey && panelVisible && ImGui::GetCurrentContext()) {
            // Пока ни одно текстовое поле панели не активно, клавиатурные
            // сообщения могут прийти здесь только от инжектированных клавиш
            // (AutoHotkey и т.п.) или от клавиши открытия панели: физические
            // клавиши останавливает низкоуровневый хук, и сообщений от них не
            // возникает. Пересылаем их игре, чтобы текст AHK попадал в чат,
            // пока панель открыта. Отдача их в ImGui вместо этого молча
            // теряла весь вставленный AHK текст.
            if (!g_panelTextInputActive.load(std::memory_order_relaxed) &&
                keyboardMessage) {
                return forwardToGame();
            }
            const bool windowCharacter = message == WM_CHAR || message == WM_SYSCHAR ||
                message == WM_UNICHAR;
            if (windowCharacter) {
                // Печатные символы набраны на клавиатуре — их уже перевёл
                // низкоуровневый хук по раскладке потока переднего плана.
                // Это ANSI-окно, поэтому оконный WM_CHAR несёт один ANSI-байт,
                // который бэкенд ImGui перекодирует через keyboard code page и
                // портит кириллицу. Дубликат отбрасываем.
                const bool duplicate = wParam >= 0x20 && wParam != UNICODE_NOCHAR &&
                    HookTranslatedCharacterRecent();
                const unsigned int codepoint = duplicate ? 0u
                    : DecodeWindowCharacter(window, message, wParam);
                LogInput("WIN msg=%04X wp=%04X unicode=%d dropped=%d decoded=%04X",
                    message, static_cast<unsigned int>(wParam),
                    static_cast<int>(IsWindowUnicode(window)),
                    static_cast<int>(duplicate), codepoint);
                if (codepoint != 0) ImGui::GetIO().AddInputCharacter(codepoint);
                return 1;
            }
            if (ansiImeChar) {
                // Текст IME не проходит через хук клавиатуры — берём как есть.
                const unsigned int codepoint =
                    DecodeWindowCharacter(window, WM_CHAR, wParam);
                if (codepoint != 0) ImGui::GetIO().AddInputCharacter(codepoint);
                return 1;
            }
            {
                ScopedInputQueryPassthrough passthrough;
                ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
            }
            const bool mouseMessage =
                (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                message == WM_MOUSEACTIVATE || message == WM_NCMOUSEMOVE ||
                message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONUP ||
                message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP;
            if (message == WM_INPUT) {
                // DefWindowProc releases the raw-input packet; it is not passed to GTA.
                DefWindowProcW(window, message, wParam, lParam);
                return 0;
            }
            if (mouseMessage || keyboardMessage) return 1;
        }
        return forwardToGame();
    }

private:
    void ScheduleSettingsSave() {
        settingsSaveDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    }

    void SaveSettingsIfDue() {
        if (settingsSaveDue_ == std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() < settingsSaveDue_) return;
        try {
            settingsStore_->Save(settings_);
        } catch (...) {
        }
        settingsSaveDue_ = {};
    }

    bool InitializeUi(IDirect3DDevice9* device) {
        if (ui_) return true;
        D3DDEVICE_CREATION_PARAMETERS creation{};
        if (FAILED(device->GetCreationParameters(&creation)) || !creation.hFocusWindow) {
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Keyboard navigation inside an overlay tends to steal focus from text
        // fields; the panel is fully mouse driven.
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigInputTrickleEventQueue = false;

        static const ImWchar glyphRanges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
            0x2010, 0x205E, // Dashes, quotes, bullets, ellipsis
            0x2116, 0x2116, // №
            0x20AC, 0x20AC, // €
            0
        };
        const char* fontCandidates[] = {
            "C:\\Windows\\Fonts\\seguisb.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf"
        };
        for (const char* fontPath : fontCandidates) {
            if (ImFont* cyrillicFont = io.Fonts->AddFontFromFileTTF(
                    fontPath, 17.0f, nullptr, glyphRanges)) {
                io.FontDefault = cyrillicFont;
                break;
            }
        }
        if (!ImGui_ImplWin32_Init(creation.hFocusWindow) || !ImGui_ImplDX9_Init(device)) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        window_ = creation.hFocusWindow;
        g_gameWindow.store(window_, std::memory_order_relaxed);
        OpenInputLog();
        {
            wchar_t classNameWide[128]{};
            GetClassNameW(window_, classNameWide,
                static_cast<int>(std::size(classNameWide)));
            char className[128]{};
            WideCharToMultiByte(CP_UTF8, 0, classNameWide, -1, className,
                static_cast<int>(sizeof(className)), nullptr, nullptr);
            DWORD ansiCodePage = 0;
            GetLocaleInfoA(LOCALE_SYSTEM_DEFAULT,
                LOCALE_RETURN_NUMBER | LOCALE_IDEFAULTANSICODEPAGE,
                reinterpret_cast<LPSTR>(&ansiCodePage),
                static_cast<int>(sizeof(ansiCodePage)));
            LogInput("INIT unicode=%d hkl=%p cp=%u class=%s",
                static_cast<int>(IsWindowUnicode(window_)),
                reinterpret_cast<void*>(GetKeyboardLayout(
                    GetWindowThreadProcessId(window_, nullptr))),
                ansiCodePage, className);
        }
        SetLastError(0);
        const LONG_PTR previous = SetWindowLongPtrW(window_, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&WindowProc));
        if (previous == 0 && GetLastError() != 0) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        originalWindowProc_ = reinterpret_cast<WNDPROC>(previous);
        activeRuntime_ = this;
        ui_ = std::make_unique<ReportPanelUi>(
            *reports_, *source_, *game_, settings_, window_,
            [this] { ScheduleSettingsSave(); },
            [this](const std::filesystem::path& path) { source_->SetPath(path); },
            [this](std::string nick) { source_->SetAdminNick(std::move(nick)); },
            [] { SetPanelVisible(false); });
        return true;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        PluginRuntime* runtime = activeRuntime_;
        return runtime ? runtime->WindowMessage(window, message, wParam, lParam)
                       : DefWindowProcW(window, message, wParam, lParam);
    }

    inline static PluginRuntime* activeRuntime_{nullptr};
    HWND window_{nullptr};
    WNDPROC originalWindowProc_{nullptr};
    std::unique_ptr<SettingsStore> settingsStore_;
    AppSettings settings_;
    std::unique_ptr<ReportManager> reports_;
    std::unique_ptr<ClipboardGameIntegration> game_;
    std::unique_ptr<ChatlogReader> source_;
    std::unique_ptr<ReportPanelUi> ui_;
    std::chrono::steady_clock::time_point settingsSaveDue_{};
    bool panelWasVisible_{false};
    bool textInputActivePrevious_{false};
};

std::unique_ptr<PluginRuntime> g_runtime;

IDirect3D9* WINAPI Direct3DCreate9Hook(UINT sdkVersion) {
    IDirect3D9* direct3D = g_direct3DCreate9(sdkVersion);
    if (!direct3D) return nullptr;

    void** table = *reinterpret_cast<void***>(direct3D);
    void* target = table[16]; // IDirect3D9::CreateDevice
    if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&CreateDeviceHook),
            reinterpret_cast<void**>(&g_createDevice)) == MH_OK) {
        MH_EnableHook(target);
    }
    return direct3D;
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirect3D9* direct3D, UINT adapter,
    D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
    const HRESULT result = g_createDevice(direct3D, adapter, deviceType,
        focusWindow, behaviorFlags, parameters, device);
    if (SUCCEEDED(result) && device && *device &&
        !g_deviceHooksInstalled.exchange(true)) {
        void** table = *reinterpret_cast<void***>(*device);
        if (MH_CreateHook(table[42], reinterpret_cast<LPVOID>(&EndSceneHook),
                reinterpret_cast<void**>(&g_endScene)) == MH_OK &&
            MH_CreateHook(table[16], reinterpret_cast<LPVOID>(&ResetHook),
                reinterpret_cast<void**>(&g_reset)) == MH_OK) {
            MH_EnableHook(table[42]);
            MH_EnableHook(table[16]);
        } else {
            g_deviceHooksInstalled.store(false);
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice9* device) {
    {
        std::lock_guard lock(g_runtimeMutex);
        if (g_runtime) g_runtime->Render(device);
    }
    return g_endScene(device);
}

HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device,
                                    D3DPRESENT_PARAMETERS* parameters) {
    {
        std::lock_guard lock(g_runtimeMutex);
        if (g_runtime) g_runtime->BeforeReset();
    }
    const HRESULT result = g_reset(device, parameters);
    {
        std::lock_guard lock(g_runtimeMutex);
        if (g_runtime) g_runtime->AfterReset(result);
    }
    return result;
}

void InstallCursorHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    const auto install = [](FARPROC target, LPVOID detour, LPVOID* original) {
        if (!target) return;
        auto* address = reinterpret_cast<void*>(target);
        if (MH_CreateHook(address, detour, original) == MH_OK) {
            MH_EnableHook(address);
        }
    };
    install(GetProcAddress(user32, "ShowCursor"),
        reinterpret_cast<LPVOID>(&ShowCursorHook),
        reinterpret_cast<LPVOID*>(&g_showCursor));
    install(GetProcAddress(user32, "ClipCursor"),
        reinterpret_cast<LPVOID>(&ClipCursorHook),
        reinterpret_cast<LPVOID*>(&g_clipCursor));
    install(GetProcAddress(user32, "SetCursorPos"),
        reinterpret_cast<LPVOID>(&SetCursorPosHook),
        reinterpret_cast<LPVOID*>(&g_setCursorPos));
    install(GetProcAddress(user32, "SetCursor"),
        reinterpret_cast<LPVOID>(&SetCursorHook),
        reinterpret_cast<LPVOID*>(&g_setCursor));
    install(GetProcAddress(user32, "GetAsyncKeyState"),
        reinterpret_cast<LPVOID>(&GetAsyncKeyStateHook),
        reinterpret_cast<LPVOID*>(&g_getAsyncKeyState));
    install(GetProcAddress(user32, "GetKeyState"),
        reinterpret_cast<LPVOID>(&GetKeyStateHook),
        reinterpret_cast<LPVOID*>(&g_getKeyState));
    install(GetProcAddress(user32, "GetKeyboardState"),
        reinterpret_cast<LPVOID>(&GetKeyboardStateHook),
        reinterpret_cast<LPVOID*>(&g_getKeyboardState));
}

void InstallDirectInputHooks() {
    HMODULE directInput = GetModuleHandleW(L"dinput8.dll");
    if (!directInput) directInput = LoadLibraryW(L"dinput8.dll");
    if (!directInput) return;
    auto* target = reinterpret_cast<void*>(
        GetProcAddress(directInput, "DirectInput8Create"));
    if (!target) return;
    if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&DirectInput8CreateHook),
            reinterpret_cast<void**>(&g_directInput8Create)) == MH_OK) {
        MH_EnableHook(target);
    }
}

DWORD WINAPI InitializePlugin(LPVOID module) {
    try {
        if (MH_Initialize() != MH_OK) return 1;
        if (HANDLE hookThread = CreateThread(nullptr, 0, KeyboardHookThread,
                module, 0, nullptr)) {
            CloseHandle(hookThread);
        }
        InstallCursorHooks();
        InstallDirectInputHooks();
        {
            std::lock_guard lock(g_runtimeMutex);
            g_runtime = std::make_unique<PluginRuntime>();
        }
        HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
        if (!d3d9) return 1;
        auto* function = reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"));
        if (!function) return 1;
        if (MH_CreateHook(function, reinterpret_cast<LPVOID>(&Direct3DCreate9Hook),
                reinterpret_cast<void**>(&g_direct3DCreate9)) != MH_OK) return 1;
        if (MH_EnableHook(function) != MH_OK) return 1;
    } catch (...) {
        return 1;
    }
    return 0;
}

} // namespace
} // namespace reportpanel

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0,
                reportpanel::InitializePlugin, instance, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
