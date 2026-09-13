#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "resource.h"

namespace {

constexpr wchar_t kAppName[] = L"dwrean DeskPins";
constexpr wchar_t kMainClass[] = L"DwreanDeskPinsMainWindow";
constexpr wchar_t kSelectorClass[] = L"DwreanDeskPinsSelectorWindow";
constexpr wchar_t kRunValueName[] = L"dwrean-deskpins";
constexpr wchar_t kMutexName[] = L"Local\\dwrean-deskpins-v2-single-instance";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SELECTION_COMPLETE = WM_APP + 2;

constexpr UINT kTrayIconId = 1;
constexpr int kHotkeyToggleActive = 1;

constexpr UINT IDM_SELECT_WINDOW = 100;
constexpr UINT IDM_UNPIN_ALL = 101;
constexpr UINT IDM_START_WITH_WINDOWS = 102;
constexpr UINT IDM_ABOUT = 103;
constexpr UINT IDM_EXIT = 104;
constexpr UINT IDM_PINNED_BASE = 1000;
constexpr UINT IDM_PINNED_MAX = 1099;

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_selectorWindow = nullptr;
NOTIFYICONDATAW g_trayIcon{};
std::vector<HWND> g_pinnedWindows;
POINT g_selectionPoint{};
bool g_selecting = false;

std::wstring executablePath() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        return {};
    }

    while (length == path.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
    }

    path.resize(length);
    return path;
}

bool isStartWithWindowsEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0,
                      KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG result = RegQueryValueExW(key,
                                         kRunValueName,
                                         nullptr,
                                         &type,
                                         reinterpret_cast<LPBYTE>(value),
                                         &bytes);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }

    const std::wstring path = executablePath();
    return !path.empty() && std::wstring(value).find(path) != std::wstring::npos;
}

bool setStartWithWindows(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0,
                        nullptr,
                        0,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring path = executablePath();
        if (path.empty()) {
            RegCloseKey(key);
            return false;
        }

        const std::wstring command = L"\"" + path + L"\"";
        result = RegSetValueExW(key,
                                kRunValueName,
                                0,
                                REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool isWindowTopMost(HWND window) {
    return window && IsWindow(window) &&
           (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool isOurWindow(HWND window) {
    return window == g_mainWindow || window == g_selectorWindow;
}

HWND normalizeTargetWindow(HWND window) {
    if (!window) {
        return nullptr;
    }

    HWND root = GetAncestor(window, GA_ROOT);
    if (root) {
        window = root;
    }

    if (!window || !IsWindow(window) || isOurWindow(window) ||
        window == GetDesktopWindow() || window == GetShellWindow()) {
        return nullptr;
    }

    return window;
}

void prunePinnedWindows() {
    g_pinnedWindows.erase(
        std::remove_if(g_pinnedWindows.begin(), g_pinnedWindows.end(), [](HWND window) {
            return !IsWindow(window) || !isWindowTopMost(window);
        }),
        g_pinnedWindows.end());
}

std::wstring windowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length > 0) {
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        if (copied > 0) {
            title.resize(static_cast<size_t>(copied));
            return title;
        }
    }

    wchar_t className[128]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) > 0) {
        return className;
    }

    return L"Untitled window";
}

std::wstring shortenTitle(const std::wstring& title) {
    constexpr size_t kMaxLength = 55;
    if (title.size() <= kMaxLength) {
        return title;
    }
    return title.substr(0, kMaxLength - 3) + L"...";
}

void updateTrayTooltip() {
    if (!g_trayIcon.hWnd) {
        return;
    }

    prunePinnedWindows();

    wchar_t tooltip[128]{};
    swprintf_s(tooltip,
               L"dwrean DeskPins - %zu pinned",
               g_pinnedWindows.size());
    wcsncpy_s(g_trayIcon.szTip, tooltip, _TRUNCATE);
    g_trayIcon.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
}

void setPinned(HWND window, bool pinned) {
    window = normalizeTargetWindow(window);
    if (!window) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const HWND insertAfter = pinned ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (!SetWindowPos(window,
                      insertAfter,
                      0,
                      0,
                      0,
                      0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    auto existing = std::find(g_pinnedWindows.begin(), g_pinnedWindows.end(), window);
    if (pinned) {
        if (existing == g_pinnedWindows.end()) {
            g_pinnedWindows.push_back(window);
        }
    } else if (existing != g_pinnedWindows.end()) {
        g_pinnedWindows.erase(existing);
    }

    updateTrayTooltip();
}

void togglePinned(HWND window) {
    window = normalizeTargetWindow(window);
    if (!window) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    setPinned(window, !isWindowTopMost(window));
}

void unpinAll() {
    const auto pinned = g_pinnedWindows;
    for (HWND window : pinned) {
        if (IsWindow(window)) {
            SetWindowPos(window,
                         HWND_NOTOPMOST,
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    g_pinnedWindows.clear();
    updateTrayTooltip();
}

void cancelSelection() {
    if (!g_selecting) {
        return;
    }

    g_selecting = false;
    ReleaseCapture();
    ShowWindow(g_selectorWindow, SW_HIDE);
}

void beginSelection() {
    if (g_selecting || !g_selectorWindow) {
        return;
    }

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_selecting = true;
    SetWindowPos(g_selectorWindow,
                 HWND_TOPMOST,
                 x,
                 y,
                 width,
                 height,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(g_selectorWindow);
    SetFocus(g_selectorWindow);
    SetCapture(g_selectorWindow);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
}

void finishSelection() {
    HWND target = WindowFromPoint(g_selectionPoint);
    target = normalizeTargetWindow(target);
    if (target) {
        togglePinned(target);
    } else {
        MessageBeep(MB_ICONWARNING);
    }
}

void showAbout() {
    MessageBoxW(nullptr,
                L"dwrean DeskPins 2.0 Beta\n\n"
                L"A lightweight modern fork of DeskPins for Windows 10 and Windows 11.\n\n"
                L"Left-click the tray icon to choose a window.\n"
                L"Ctrl+Alt+P toggles the currently active window.\n\n"
                L"Original DeskPins by Elias Fotinis.\n"
                L"Modernized by dwrean.net.",
                kAppName,
                MB_OK | MB_ICONINFORMATION);
}

void handleMenuCommand(UINT command) {
    if (command >= IDM_PINNED_BASE && command <= IDM_PINNED_MAX) {
        prunePinnedWindows();
        const size_t index = static_cast<size_t>(command - IDM_PINNED_BASE);
        if (index < g_pinnedWindows.size()) {
            setPinned(g_pinnedWindows[index], false);
        }
        return;
    }

    switch (command) {
        case IDM_SELECT_WINDOW:
            beginSelection();
            break;

        case IDM_UNPIN_ALL:
            unpinAll();
            break;

        case IDM_START_WITH_WINDOWS: {
            const bool enabled = isStartWithWindowsEnabled();
            if (!setStartWithWindows(!enabled)) {
                MessageBoxW(nullptr,
                            L"Windows startup could not be updated.",
                            kAppName,
                            MB_OK | MB_ICONERROR);
            }
            break;
        }

        case IDM_ABOUT:
            showAbout();
            break;

        case IDM_EXIT:
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
            break;

        default:
            break;
    }
}

void showTrayMenu() {
    prunePinnedWindows();

    HMENU menu = CreatePopupMenu();
    HMENU pinnedMenu = CreatePopupMenu();
    if (!menu || !pinnedMenu) {
        if (pinnedMenu) DestroyMenu(pinnedMenu);
        if (menu) DestroyMenu(menu);
        return;
    }

    AppendMenuW(menu, MF_STRING, IDM_SELECT_WINDOW, L"Pin / unpin a window...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (g_pinnedWindows.empty()) {
        AppendMenuW(pinnedMenu, MF_STRING | MF_GRAYED, 0, L"No pinned windows");
    } else {
        const size_t limit = std::min<size_t>(g_pinnedWindows.size(), 100);
        for (size_t i = 0; i < limit; ++i) {
            const std::wstring title = shortenTitle(windowTitle(g_pinnedWindows[i]));
            AppendMenuW(pinnedMenu,
                        MF_STRING,
                        IDM_PINNED_BASE + static_cast<UINT>(i),
                        title.c_str());
        }
    }

    AppendMenuW(menu,
                MF_POPUP,
                reinterpret_cast<UINT_PTR>(pinnedMenu),
                L"Pinned windows");
    AppendMenuW(menu,
                MF_STRING | (g_pinnedWindows.empty() ? MF_GRAYED : MF_ENABLED),
                IDM_UNPIN_ALL,
                L"Unpin all");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu,
                MF_STRING | (isStartWithWindowsEnabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_START_WITH_WINDOWS,
                L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_mainWindow);

    const UINT command = TrackPopupMenu(menu,
                                        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                        point.x,
                                        point.y,
                                        0,
                                        g_mainWindow,
                                        nullptr);

    PostMessageW(g_mainWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command != 0) {
        handleMenuCommand(command);
    }
}

bool addTrayIcon() {
    g_trayIcon = {};
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_mainWindow;
    g_trayIcon.uID = kTrayIconId;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_DWREAN_DESKPINS));
    if (!g_trayIcon.hIcon) {
        g_trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcsncpy_s(g_trayIcon.szTip, kAppName, _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &g_trayIcon)) {
        return false;
    }

    g_trayIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_trayIcon);
    updateTrayTooltip();
    return true;
}

void removeTrayIcon() {
    if (g_trayIcon.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
        g_trayIcon = {};
    }
}

LRESULT CALLBACK selectorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_LBUTTONDOWN:
            GetCursorPos(&g_selectionPoint);
            cancelSelection();
            PostMessageW(g_mainWindow, WM_SELECTION_COMPLETE, 0, 0);
            return 0;

        case WM_RBUTTONDOWN:
            cancelSelection();
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                cancelSelection();
                return 0;
            }
            break;

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_ERASEBKGND:
            return 1;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK mainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TRAYICON: {
            const UINT trayMessage = LOWORD(lParam);
            if (trayMessage == WM_LBUTTONUP || trayMessage == NIN_SELECT ||
                trayMessage == NIN_KEYSELECT) {
                beginSelection();
            } else if (trayMessage == WM_RBUTTONUP || trayMessage == WM_CONTEXTMENU) {
                showTrayMenu();
            }
            return 0;
        }

        case WM_SELECTION_COMPLETE:
            finishSelection();
            return 0;

        case WM_HOTKEY:
            if (wParam == kHotkeyToggleActive) {
                togglePinned(GetForegroundWindow());
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            cancelSelection();
            UnregisterHotKey(window, kHotkeyToggleActive);
            unpinAll();
            removeTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool registerWindowClasses() {
    HICON appIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_DWREAN_DESKPINS));
    if (!appIcon) {
        appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = mainWindowProc;
    mainClass.hInstance = g_instance;
    mainClass.hIcon = appIcon;
    mainClass.hIconSm = appIcon;
    mainClass.lpszClassName = kMainClass;

    if (!RegisterClassExW(&mainClass)) {
        return false;
    }

    WNDCLASSEXW selectorClass{};
    selectorClass.cbSize = sizeof(selectorClass);
    selectorClass.lpfnWndProc = selectorWindowProc;
    selectorClass.hInstance = g_instance;
    selectorClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    selectorClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    selectorClass.lpszClassName = kSelectorClass;

    return RegisterClassExW(&selectorClass) != 0;
}

bool createWindows() {
    g_mainWindow = CreateWindowExW(0,
                                   kMainClass,
                                   kAppName,
                                   WS_OVERLAPPED,
                                   0,
                                   0,
                                   0,
                                   0,
                                   nullptr,
                                   nullptr,
                                   g_instance,
                                   nullptr);
    if (!g_mainWindow) {
        return false;
    }

    g_selectorWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                       kSelectorClass,
                                       L"",
                                       WS_POPUP,
                                       0,
                                       0,
                                       0,
                                       0,
                                       nullptr,
                                       nullptr,
                                       g_instance,
                                       nullptr);
    if (!g_selectorWindow) {
        return false;
    }

    SetLayeredWindowAttributes(g_selectorWindow, 0, 1, LWA_ALPHA);
    ShowWindow(g_selectorWindow, SW_HIDE);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    SetProcessDPIAware();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) {
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    if (!registerWindowClasses() || !createWindows()) {
        MessageBoxW(nullptr,
                    L"dwrean DeskPins could not initialize.",
                    kAppName,
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    if (!RegisterHotKey(g_mainWindow,
                        kHotkeyToggleActive,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                        'P')) {
        MessageBoxW(nullptr,
                    L"The Ctrl+Alt+P shortcut is already being used by another application.\n"
                    L"dwrean DeskPins will continue to work from the tray icon.",
                    kAppName,
                    MB_OK | MB_ICONWARNING);
    }

    if (!addTrayIcon()) {
        MessageBoxW(nullptr,
                    L"dwrean DeskPins could not create its tray icon.",
                    kAppName,
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g_mainWindow);
        CloseHandle(mutex);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
