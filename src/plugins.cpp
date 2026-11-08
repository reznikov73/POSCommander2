#include "plugins.h"

#include "util.h"

#include <commctrl.h>

#include <string>
#include <vector>

namespace {

const wchar_t* kPluginsClass = L"POSPlugins2";

const int kListId = 60;
const int kStatusId = 61;
const int kCloseId = 62;

// Биты возможностей упаковщика (wcx) из TC SDK.
std::wstring CapsText(int caps) {
    if (caps == 0) return L"—";
    struct Bit {
        int value;
        const wchar_t* name;
    };
    static const Bit bits[] = {
        {1, L"создание"},        {2, L"изменение"},     {4, L"многофайловый"},
        {8, L"удаление"},        {16, L"настройки"},    {32, L"сборка в память"},
        {64, L"по содержимому"}, {128, L"поиск текста"}, {256, L"скрытый"},
        {512, L"шифрование"},    {1024, L"многотомный"}, {2048, L"только чтение"},
    };
    std::wstring out;
    for (auto& b : bits) {
        if (caps & b.value) {
            if (!out.empty()) out += L", ";
            out += b.name;
        }
    }
    return out.empty() ? L"—" : out;
}

std::vector<std::wstring> DefaultFolders() {
    std::vector<std::wstring> folders;
    const wchar_t* candidates[] = {
        L"E:\\Distr\\TCPU68\\Plugins\\wcx",
        L"C:\\totalcmd\\Plugins\\wcx",
        L"C:\\Program Files\\totalcmd\\Plugins\\wcx",
    };
    for (auto c : candidates) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) folders.push_back(c);
    }
    return folders;
}

}  // namespace

std::vector<PluginInfo> ScanWcxPlugins(const std::vector<std::wstring>& folders) {
    std::vector<PluginInfo> found;

    auto scanFolder = [&found](const std::wstring& folder) {
        std::wstring pattern = JoinPath(folder, L"*.wcx");
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;

        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            PluginInfo info;
            info.path = JoinPath(folder, data.cFileName);
            info.name = data.cFileName;

            HMODULE dll = LoadLibraryW(info.path.c_str());
            if (!dll) {
                info.error = L"не загружается (разрядность или зависимости)";
                found.push_back(info);
                continue;
            }

            info.loaded = true;
            using CapsFn = int(__stdcall*)();
            auto caps = reinterpret_cast<CapsFn>(GetProcAddress(dll, "GetPackerCaps"));
            if (caps) info.caps = caps();
            info.capsText = CapsText(info.caps);

            FreeLibrary(dll);
            found.push_back(info);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    };

    // Плагины TC обычно лежат в собственных подпапках: Plugins\wcx\<имя>\<имя>.wcx
    for (auto& folder : folders) {
        scanFolder(folder);

        std::wstring mask = JoinPath(folder, L"*");
        WIN32_FIND_DATAW dir{};
        HANDLE find = FindFirstFileW(mask.c_str(), &dir);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(dir.cFileName, L".") == 0 || wcscmp(dir.cFileName, L"..") == 0) continue;
            scanFolder(JoinPath(folder, dir.cFileName));
        } while (FindNextFileW(find, &dir));
        FindClose(find);
    }
    return found;
}

namespace {

struct PluginsState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    std::vector<PluginInfo> items;
};

void FillPlugins(PluginsState* st) {
    int row = 0;
    for (auto& p : st->items) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = (LPWSTR)p.name.c_str();
        int index = ListView_InsertItem(st->list, &item);
        if (index >= 0) {
            ListView_SetItemText(st->list, index, 1, (LPWSTR)(p.loaded ? L"загружен" : L"ошибка"));
            ListView_SetItemText(st->list, index, 2, (LPWSTR)p.capsText.c_str());
            ListView_SetItemText(st->list, index, 3, (LPWSTR)p.path.c_str());
            if (!p.error.empty()) ListView_SetItemText(st->list, index, 4, (LPWSTR)p.error.c_str());
        }
        ++row;
    }

    wchar_t buf[256]{};
    swprintf(buf, 256, L"Найдено плагинов: %d   ·   загружено: %d", (int)st->items.size(),
             (int)std::count_if(st->items.begin(), st->items.end(), [](const PluginInfo& p) { return p.loaded; }));
    SetWindowTextW(st->status, buf);
}

void LayoutPlugins(PluginsState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int pad = 8, btnH = 26, statusH = 22;

    MoveWindow(st->list, pad, pad, w - 2 * pad, h - pad * 2 - btnH - statusH, TRUE);
    MoveWindow(st->status, pad, h - btnH - statusH - pad, w - 2 * pad, statusH, TRUE);
    HWND close = GetDlgItem(st->hwnd, kCloseId);
    if (close) MoveWindow(close, pad, h - btnH - pad, 140, btnH, TRUE);
}

LRESULT CALLBACK PluginsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PluginsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<PluginsState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->hwnd = hwnd;

            st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 170; col.iSubItem = 0; col.pszText = (LPWSTR)L"Плагин";
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 90; col.iSubItem = 1; col.pszText = (LPWSTR)L"Состояние";
            ListView_InsertColumn(st->list, 1, &col);
            col.cx = 260; col.iSubItem = 2; col.pszText = (LPWSTR)L"Возможности";
            ListView_InsertColumn(st->list, 2, &col);
            col.cx = 320; col.iSubItem = 3; col.pszText = (LPWSTR)L"Путь";
            ListView_InsertColumn(st->list, 3, &col);
            col.cx = 220; col.iSubItem = 4; col.pszText = (LPWSTR)L"Примечание";
            ListView_InsertColumn(st->list, 4, &col);

            HWND close = CreateWindowExW(0, L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kCloseId, g_inst, nullptr);
            st->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kStatusId, g_inst, nullptr);
            for (HWND h : {st->list, st->status, close}) {
                if (h && st->font) SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
            }

            FillPlugins(st);
            return 0;
        }

        case WM_SIZE:
            if (st) LayoutPlugins(st);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == kCloseId) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                if (st->font) DeleteObject(st->font);
                st->font = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void PluginsDialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PluginsProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPluginsClass;
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);
        registered = true;
    }

    PluginsState state;
    state.items = ScanWcxPlugins(DefaultFolders());

    RECT ownerRect{0, 0, 900, 600};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kPluginsClass, L"Плагины Total Commander (wcx)",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, ownerRect.left + 80, ownerRect.top + 80, 900, 520,
                                nullptr, nullptr, g_inst, &state);
    if (!hwnd) return;

    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ownerWasEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        SetFocus(owner);
    }
}
