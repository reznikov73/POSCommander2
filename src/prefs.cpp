// Окно настроек: несколько практичных параметров, которые раньше правились только в INI.
#include "prefs.h"

#include "settings.h"
#include "util.h"

#include <commctrl.h>

#include <string>

namespace {

const wchar_t* kPrefsClass = L"POSPrefs2";
const int kFontId = 800;
const int kHiddenId = 801;
const int kSyntaxId = 802;
const int kResetId = 803;

struct PrefsState {
    HWND hwnd = nullptr;
    HWND fontBox = nullptr;
    HWND hidden = nullptr;
    HWND syntax = nullptr;
    HWND hint = nullptr;
    bool done = false;
    bool ok = false;
};

PrefsState g_prefs;

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 14;
    const int labelW = 190;
    const int rowH = 26;

    MoveWindow(GetDlgItem(hwnd, 810), pad, pad + 4, labelW, 20, TRUE);
    MoveWindow(g_prefs.fontBox, pad + labelW + 8, pad, 90, 200, TRUE);

    MoveWindow(g_prefs.hidden, pad, pad + rowH + 16, rc.right - 2 * pad, 20, TRUE);
    MoveWindow(g_prefs.syntax, pad, pad + rowH + 44, rc.right - 2 * pad, 20, TRUE);
    MoveWindow(g_prefs.hint, pad, pad + rowH + 78, rc.right - 2 * pad, 34, TRUE);

    MoveWindow(GetDlgItem(hwnd, kResetId), pad, rc.bottom - pad - 28, 260, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDOK), rc.right - pad - 110, rc.bottom - pad - 28, 110, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), rc.right - pad - 230, rc.bottom - pad - 28, 110, 28, TRUE);
}

LRESULT CALLBACK PrefsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_prefs.hwnd = hwnd;
            const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            HWND label = CreateWindowExW(0, L"STATIC", L"Шрифт интерфейса, пт:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         0, 0, 10, 10, hwnd, (HMENU)810, nullptr, nullptr);
            g_prefs.fontBox = CreateWindowExW(0, L"COMBOBOX", L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                              0, 0, 10, 10, hwnd, (HMENU)kFontId, nullptr, nullptr);
            for (int pt = 8; pt <= 14; ++pt) {
                wchar_t buf[16]{};
                swprintf(buf, 16, L"%d", pt);
                SendMessageW(g_prefs.fontBox, CB_ADDSTRING, 0, (LPARAM)buf);
            }

            g_prefs.hidden = CreateWindowExW(0, L"BUTTON", L"Показывать скрытые и системные файлы",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, hwnd,
                                            (HMENU)kHiddenId, nullptr, nullptr);
            g_prefs.syntax = CreateWindowExW(0, L"BUTTON", L"Подсветка синтаксиса в просмотрщике",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, hwnd,
                                             (HMENU)kSyntaxId, nullptr, nullptr);
            g_prefs.hint = CreateWindowExW(0, L"STATIC",
                                           L"Подсветку можно временно выключить в просмотрщике клавишей C.",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, (HMENU)811, nullptr,
                                           nullptr);

            CreateWindowExW(0, L"BUTTON", L"Сбросить размеры окон", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            0, 0, 10, 10, hwnd, (HMENU)kResetId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Сохранить", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                            10, 10, hwnd, (HMENU)IDOK, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                            hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

            for (HWND h : {label, g_prefs.fontBox, g_prefs.hidden, g_prefs.syntax, g_prefs.hint,
                           GetDlgItem(hwnd, kResetId), GetDlgItem(hwnd, IDOK), GetDlgItem(hwnd, IDCANCEL)}) {
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
            }

            // Текущие значения
            const Settings& st = GetSettings();
            const int index = st.uiFontPt - 8;
            SendMessageW(g_prefs.fontBox, CB_SETCURSEL, index >= 0 && index <= 6 ? index : 1, 0);
            SendMessageW(g_prefs.hidden, BM_SETCHECK, st.showHidden ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_prefs.syntax, BM_SETCHECK, st.syntaxHighlight ? BST_CHECKED : BST_UNCHECKED, 0);

            Layout(hwnd);
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kResetId) {
                const std::wstring ini = SettingsFilePath();
                for (const wchar_t* key : {L"X", L"Y", L"W", L"H"}) {
                    WritePrivateProfileStringW(L"Search", key, nullptr, ini.c_str());
                    WritePrivateProfileStringW(L"Lister", key, nullptr, ini.c_str());
                }
                MessageBoxW(hwnd, L"Размеры окон поиска и просмотра сброшены.", L"Настройки",
                            MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            if (id == IDOK) {
                g_prefs.ok = true;
                g_prefs.done = true;
                return 0;
            }
            if (id == IDCANCEL) {
                g_prefs.done = true;
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            g_prefs.done = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool ShowPreferences(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PrefsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPrefsClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_prefs = PrefsState{};

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = 560, h = 300;
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kPrefsClass, L"Настройки",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return false;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!g_prefs.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    int fontPt = 9;
    bool hidden = true;
    bool syntax = true;
    if (g_prefs.ok) {
        const int sel = (int)SendMessageW(g_prefs.fontBox, CB_GETCURSEL, 0, 0);
        fontPt = 8 + (sel >= 0 && sel <= 6 ? sel : 1);
        hidden = SendMessageW(g_prefs.hidden, BM_GETCHECK, 0, 0) == BST_CHECKED;
        syntax = SendMessageW(g_prefs.syntax, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    DestroyWindow(hwnd);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);

    if (!g_prefs.ok) return false;

    Settings& st = GetSettings();
    st.uiFontPt = fontPt;
    st.showHidden = hidden;
    st.syntaxHighlight = syntax;
    SaveSettings();
    return true;
}
