// Поиск дубликатов: группируем по размеру, внутри группы сверяем CRC32.
// Лишние копии можно отметить автоматически и удалить в корзину.
#include "dupes.h"

#include "util.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

const wchar_t* kDupesClass = L"POSDupes2";
const int kListId = 400;
const int kStatusId = 401;
const int kMarkId = 402;
const int kDeleteId = 403;
const int kCloseId = 404;
const UINT kScanDoneMsg = WM_APP + 412;

struct Item {
    std::wstring path;
    std::wstring name;
    std::wstring dir;
    ULONGLONG size = 0;
    FILETIME time{};
    int group = 0;
};

struct DupState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    std::wstring root;
    std::vector<Item> items;
    size_t totalFiles = 0;
    int groups = 0;
    bool done = false;
    bool cancel = false;
};

DupState g_dup;

unsigned long Crc32File(const std::wstring& path, bool& ok) {
    static unsigned long table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320ul ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        ok = false;
        return 0;
    }

    std::vector<unsigned char> buf(64 * 1024);
    unsigned long crc = 0xFFFFFFFFul;
    DWORD read = 0;
    ok = true;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
        for (DWORD i = 0; i < read; ++i) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    CloseHandle(f);
    return crc ^ 0xFFFFFFFFul;
}

struct SizeEntry {
    ULONGLONG size = 0;
    std::vector<std::wstring> paths;
};

// Первый проход: собираем файлы и группируем по размеру.
void CollectFiles(const std::wstring& dir, int depth, std::map<ULONGLONG, std::vector<std::wstring>>& bySize,
                  size_t& counter) {
    if (g_dup.cancel || depth > 32) return;

    std::wstring mask = dir;
    if (!mask.empty() && mask.back() != L'\\') mask.push_back(L'\\');
    mask += L"*";

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(mask.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    do {
        if (g_dup.cancel) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        const std::wstring full = JoinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdirs.push_back(full);
            continue;
        }

        const ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size == 0) continue;                 // пустые файлы дубликатами не считаем
        bySize[size].push_back(full);
        ++counter;
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    for (const auto& sub : subdirs) CollectFiles(sub, depth + 1, bySize, counter);
}

void UpdateStatus(const wchar_t* suffix) {
    if (!g_dup.status) return;
    wchar_t buf[256]{};
    swprintf(buf, 256, L"Групп: %d · файлов в группах: %zu · просмотрено: %zu%s", g_dup.groups,
             g_dup.items.size(), g_dup.totalFiles, suffix ? suffix : L"");
    SetWindowTextW(g_dup.status, buf);
}

void FillList() {
    if (!g_dup.list) return;
    SendMessageW(g_dup.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_dup.list);

    int row = 0;
    for (const auto& it : g_dup.items) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = (LPWSTR)it.name.c_str();
        item.lParam = (LPARAM)row;
        const int index = ListView_InsertItem(g_dup.list, &item);
        if (index < 0) continue;

        wchar_t group[32]{};
        swprintf(group, 32, L"#%d", it.group);
        ListView_SetItemText(g_dup.list, index, 1, group);
        ListView_SetItemText(g_dup.list, index, 2, (LPWSTR)it.dir.c_str());

        std::wstring size = FmtSize(it.size);
        ListView_SetItemText(g_dup.list, index, 3, (LPWSTR)size.c_str());
        std::wstring date = FmtDate(it.time);
        ListView_SetItemText(g_dup.list, index, 4, (LPWSTR)date.c_str());

        ListView_SetCheckState(g_dup.list, index, FALSE);
        ++row;
    }

    SendMessageW(g_dup.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_dup.list, nullptr, TRUE);
    UpdateStatus(nullptr);
}

// Внутри каждой группы оставляем самый старый файл, остальные отмечаем.
void MarkExtra() {
    std::map<int, std::vector<int>> byGroup;
    for (int i = 0; i < (int)g_dup.items.size(); ++i) byGroup[g_dup.items[i].group].push_back(i);

    for (auto& kv : byGroup) {
        auto& rows = kv.second;
        if (rows.size() < 2) continue;
        size_t keep = rows.front();
        for (size_t idx : rows) {
            if (CompareFileTime(&g_dup.items[idx].time, &g_dup.items[keep].time) < 0) keep = idx;
        }
        for (size_t idx : rows) ListView_SetCheckState(g_dup.list, (int)idx, idx == keep ? FALSE : TRUE);
    }
}

void DeleteChecked() {
    std::vector<std::wstring> paths;
    for (int i = 0; i < (int)g_dup.items.size(); ++i) {
        if (ListView_GetCheckState(g_dup.list, i)) paths.push_back(g_dup.items[i].path);
    }
    if (paths.empty()) {
        MessageBoxW(g_dup.hwnd, L"Ничего не отмечено. Нажмите «Отметить лишние» или поставьте галочки вручную.",
                    L"Дубликаты", MB_ICONINFORMATION | MB_OK);
        return;
    }

    wchar_t question[256]{};
    swprintf(question, 256, L"Удалить в корзину %zu файл(ов)?", paths.size());
    if (MessageBoxW(g_dup.hwnd, question, L"Дубликаты", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    std::wstring list;
    for (const auto& p : paths) {
        list += p;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = g_dup.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
        MessageBoxW(g_dup.hwnd, L"Не удалось удалить часть файлов.", L"Дубликаты", MB_ICONERROR | MB_OK);
        return;
    }

    // Убираем удалённые из списка и перенумеровываем группы.
    std::vector<Item> rest;
    for (int i = 0; i < (int)g_dup.items.size(); ++i) {
        if (ListView_GetCheckState(g_dup.list, i)) continue;
        rest.push_back(g_dup.items[i]);
    }
    g_dup.items.swap(rest);

    std::map<int, int> renumber;
    int next = 1;
    for (auto& it : g_dup.items) {
        auto found = renumber.find(it.group);
        if (found == renumber.end()) {
            renumber[it.group] = next;
            it.group = next;
            ++next;
        } else {
            it.group = found->second;
        }
    }
    g_dup.groups = next - 1;
    FillList();
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 8;
    const int btnH = 26;
    const int statusH = 20;
    const int bottom = rc.bottom - pad - btnH;

    const int statusY = bottom - statusH - 4;
    MoveWindow(g_dup.list, pad, pad, rc.right - 2 * pad, statusY - pad - 4, TRUE);
    MoveWindow(g_dup.status, pad, statusY, rc.right - 2 * pad, statusH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kMarkId), pad, bottom, 200, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kDeleteId), pad + 208, bottom, 260, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kCloseId), rc.right - pad - 100, bottom, 100, btnH, TRUE);
}

void ScanAndFill(HWND hwnd) {
    std::map<ULONGLONG, std::vector<std::wstring>> bySize;
    size_t counter = 0;

    SetWindowTextW(g_dup.status, L"Сканирую каталог…");
    UpdateWindow(g_dup.status);
    CollectFiles(g_dup.root, 0, bySize, counter);
    g_dup.totalFiles = counter;
    if (g_dup.cancel || !IsWindow(hwnd)) return;

    g_dup.items.clear();
    g_dup.groups = 0;
    int groupNo = 0;

    for (auto& kv : bySize) {
        if (kv.second.size() < 2) continue;

        std::map<unsigned long, std::vector<std::wstring>> byHash;
        for (const auto& path : kv.second) {
            bool ok = false;
            const unsigned long crc = Crc32File(path, ok);
            if (!ok) continue;
            byHash[crc].push_back(path);
        }

        for (auto& hk : byHash) {
            if (hk.second.size() < 2) continue;
            ++groupNo;
            for (const auto& path : hk.second) {
                Item it;
                it.path = path;
                const size_t slash = path.find_last_of(L'\\');
                it.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
                it.dir = slash == std::wstring::npos ? L"" : path.substr(0, slash);
                it.size = kv.first;
                it.group = groupNo;

                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) it.time = fad.ftLastWriteTime;
                g_dup.items.push_back(std::move(it));
            }
        }

        if (g_dup.cancel) break;
        wchar_t buf[128]{};
        swprintf(buf, 128, L"Сверяю содержимое… групп: %d", groupNo);
        SetWindowTextW(g_dup.status, buf);
        UpdateWindow(g_dup.status);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hwnd)) return;
    }

    g_dup.groups = groupNo;
    std::sort(g_dup.items.begin(), g_dup.items.end(), [](const Item& a, const Item& b) {
        if (a.group != b.group) return a.group < b.group;
        return CompareNatural(a.name, b.name) < 0;
    });
    FillList();
}

LRESULT CALLBACK DupesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_dup.hwnd = hwnd;
            g_dup.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 10, 10,
                                         hwnd, (HMENU)kListId, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(g_dup.list,
                                              LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

            struct Col { const wchar_t* text; int width; };
            const Col cols[] = {{L"Файл", 250}, {L"Группа", 70}, {L"Каталог", 320}, {L"Размер", 90}, {L"Дата", 120}};
            for (int i = 0; i < 5; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = (LPWSTR)cols[i].text;
                c.cx = cols[i].width;
                c.iSubItem = i;
                ListView_InsertColumn(g_dup.list, i, &c);
            }

            g_dup.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                           (HMENU)kStatusId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Отметить лишние", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            0, 0, 10, 10, hwnd, (HMENU)kMarkId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Удалить отмеченные в корзину",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd, (HMENU)kDeleteId,
                            nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10,
                            10, hwnd, (HMENU)kCloseId, nullptr, nullptr);

            const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            for (HWND k : {g_dup.list, g_dup.status, GetDlgItem(hwnd, kMarkId), GetDlgItem(hwnd, kDeleteId),
                           GetDlgItem(hwnd, kCloseId)}) {
                SendMessageW(k, WM_SETFONT, (WPARAM)font, TRUE);
            }

            SetWindowTextW(hwnd, (L"Дубликаты: " + g_dup.root).c_str());
            Layout(hwnd);
            ScanAndFill(hwnd);
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kMarkId) { MarkExtra(); return 0; }
            if (id == kDeleteId) { DeleteChecked(); return 0; }
            if (id == kCloseId) { g_dup.done = true; return 0; }
            break;
        }

        case WM_CLOSE:
            g_dup.cancel = true;
            g_dup.done = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void FindDuplicates(HWND owner, const std::wstring& root) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DupesProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDupesClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_dup = DupState{};
    g_dup.root = root;

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = 900, h = 560;
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kDupesClass, L"Дубликаты",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!g_dup.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}
