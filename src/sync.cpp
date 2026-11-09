// Синхронизация каталогов двух панелей: сравнение по имени/размеру/дате
// и копирование выбранных расхождений в нужную сторону.
#include "sync.h"

#include "util.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

const wchar_t* kSyncClass = L"POSSync2";
const int kListId = 300;
const int kStatusId = 301;
const int kCopyRightId = 302;      // →
const int kCopyLeftId = 303;       // ←
const int kWrapId = 304;     // «Обновить»
const int kOnlyDiffId = 305;       // только различия
const int kCloseId = 306;

struct FileInfo {
    bool exists = false;
    bool isDir = false;
    unsigned long long size = 0;
    FILETIME time{};
};

struct RelEntry {
    std::wstring rel;              // путь относительно корня, через '\'
    FileInfo left;
    FileInfo right;
    bool differs = false;
    std::wstring status;           // «нет справа», «размер», …
    bool checked = false;
};

struct SyncState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HWND onlyDiff = nullptr;
    HFONT font = nullptr;
    std::wstring leftRoot;
    std::wstring rightRoot;
    std::vector<RelEntry> items;
    bool done = false;
};

SyncState g_sync;

std::wstring LowerKey(const std::wstring& text) {
    std::wstring out = text;
    for (auto& c : out) c = (wchar_t)towlower(c);
    return out;
}

void ScanTreeLevel(const std::wstring& root, const std::wstring& rel, int depth,
                   std::map<std::wstring, std::pair<std::wstring, FileInfo>>& out) {
    if (depth > 32) return;                     // защита от циклов и очень глубоких деревьев

    std::wstring mask = root;
    if (!mask.empty() && mask.back() != L'\\') mask.push_back(L'\\');
    mask += rel.empty() ? L"*" : rel + L"\\*";

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(mask.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // симлинки не обходим

        const std::wstring child = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
        FileInfo info;
        info.exists = true;
        info.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        info.time = fd.ftLastWriteTime;
        out[LowerKey(child)] = {child, info};

        if (info.isDir) ScanTreeLevel(root, child, depth + 1, out);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

void ScanTree(const std::wstring& root, std::map<std::wstring, std::pair<std::wstring, FileInfo>>& out) {
    if (!IsDirectory(root)) return;
    ScanTreeLevel(root, std::wstring(), 0, out);
}

std::wstring JoinRoot(const std::wstring& root, const std::wstring& rel) {
    std::wstring out = root;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    return out + rel;
}

void MakeAllDirs(const std::wstring& path) {
    // создаём каждый уровень пути по очереди, уже существующие пропускаем
    std::wstring current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        const bool last = (i + 1 == path.size());
        if (path[i] == L'\\' || last) {
            if (current.size() > 3 && current.back() != L':') CreateDirectoryW(current.c_str(), nullptr);
        }
    }
}

void MakeParentDirs(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 3) return;
    MakeAllDirs(path.substr(0, slash));
}

bool CopyOne(const RelEntry& item, bool toRight, std::wstring& error) {
    const std::wstring src = JoinRoot(toRight ? g_sync.leftRoot : g_sync.rightRoot, item.rel);
    const std::wstring dst = JoinRoot(toRight ? g_sync.rightRoot : g_sync.leftRoot, item.rel);
    const bool isDir = item.left.isDir || item.right.isDir;

    if (isDir) {
        MakeAllDirs(dst);
        if (!IsDirectory(dst)) {
            error = L"Не удалось создать каталог:\n" + dst;
            return false;
        }
        return true;
    }
    MakeParentDirs(dst);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        error = L"Не удалось скопировать:\n" + src + L"\n\n" + ErrText(GetLastError());
        return false;
    }
    return true;
}

void FillList() {
    if (!g_sync.list) return;
    SendMessageW(g_sync.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_sync.list);

    const bool onlyDiff = g_sync.onlyDiff && SendMessageW(g_sync.onlyDiff, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int row = 0;
    for (auto& e : g_sync.items) {
        if (onlyDiff && !e.differs) continue;

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = (LPWSTR)e.rel.c_str();
        ListView_InsertItem(g_sync.list, &item);

        std::wstring leftText = L"—";
        if (e.left.exists) leftText = e.left.isDir ? L"<папка>" : FmtSize(e.left.size);
        std::wstring rightText = L"—";
        if (e.right.exists) rightText = e.right.isDir ? L"<папка>" : FmtSize(e.right.size);

        ListView_SetItemText(g_sync.list, row, 1, (LPWSTR)leftText.c_str());
        ListView_SetItemText(g_sync.list, row, 2, (LPWSTR)rightText.c_str());
        ListView_SetItemText(g_sync.list, row, 3, (LPWSTR)e.status.c_str());

        ListView_SetCheckState(g_sync.list, row, e.differs ? TRUE : FALSE);
        row++;
    }
    SendMessageW(g_sync.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_sync.list, nullptr, TRUE);

    wchar_t buf[128]{};
    size_t diffCount = 0;
    for (auto& e : g_sync.items) if (e.differs) diffCount++;
    swprintf(buf, 128, L"Расхождений: %zu из %zu", diffCount, g_sync.items.size());
    SetWindowTextW(g_sync.status, buf);
}

void BuildItems() {
    g_sync.items.clear();
    std::map<std::wstring, std::pair<std::wstring, FileInfo>> left, right;
    ScanTree(g_sync.leftRoot, left);
    ScanTree(g_sync.rightRoot, right);

    std::vector<std::wstring> keys;
    keys.reserve(left.size() + right.size());
    for (auto& kv : left) keys.push_back(kv.first);
    for (auto& kv : right) {
        if (left.find(kv.first) == left.end()) keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());

    for (auto& key : keys) {
        RelEntry e;
        auto li = left.find(key);
        auto ri = right.find(key);
        if (li != left.end()) {
            e.rel = li->second.first;          // настоящее имя с сохранением регистра
            e.left = li->second.second;
        }
        if (ri != right.end()) {
            if (e.rel.empty()) e.rel = ri->second.first;
            e.right = ri->second.second;
        }

        if (e.left.exists && !e.right.exists) {
            e.differs = true;
            e.status = L"нет справа";
        } else if (!e.left.exists && e.right.exists) {
            e.differs = true;
            e.status = L"нет слева";
        } else if (e.left.isDir != e.right.isDir) {
            e.differs = true;
            e.status = L"файл/папка";
        } else if (!e.left.isDir && e.left.size != e.right.size) {
            e.differs = true;
            e.status = L"размер";
        } else if (!e.left.isDir && e.left.time.dwLowDateTime != e.right.time.dwLowDateTime &&
                   e.left.time.dwHighDateTime != e.right.time.dwHighDateTime) {
            e.differs = true;
            e.status = L"дата";
        } else {
            e.status = L"совпадает";
        }
        g_sync.items.push_back(std::move(e));
    }
}

void CopyChecked(bool toRight) {
    if (!g_sync.list) return;
    // сначала каталоги, чтобы файлы легли в готовые папки
    std::vector<size_t> order;
    for (size_t i = 0; i < g_sync.items.size(); ++i) {
        if (!ListView_GetCheckState(g_sync.list, (int)i)) continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        return g_sync.items[a].rel.size() < g_sync.items[b].rel.size();
    });

    int done = 0;
    for (size_t idx : order) {
        RelEntry& e = g_sync.items[idx];
        const bool sourceExists = toRight ? e.left.exists : e.right.exists;
        if (!sourceExists) continue;
        std::wstring error;
        if (!CopyOne(e, toRight, error)) {
            MessageBoxW(g_sync.hwnd, error.c_str(), L"Синхронизация", MB_ICONERROR);
            break;
        }
        done++;
        wchar_t buf[128]{};
        swprintf(buf, 128, L"Скопировано: %d из %zu", done, order.size());
        SetWindowTextW(g_sync.status, buf);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    BuildItems();
    FillList();
}

void SyncLayout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 8;
    const int btnH = 26, statusH = 20;
    const int bottom = rc.bottom - pad - btnH;
    MoveWindow(g_sync.list, pad, pad, rc.right - 2 * pad, bottom - pad * 2, TRUE);
    MoveWindow(GetDlgItem(hwnd, kCopyRightId), pad, bottom, 150, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kCopyLeftId), pad + 156, bottom, 150, btnH, TRUE);
    MoveWindow(g_sync.onlyDiff, pad + 316, bottom + 4, 150, 20, TRUE);
    MoveWindow(GetDlgItem(hwnd, kCloseId), rc.right - pad - 90, bottom, 90, btnH, TRUE);
    MoveWindow(g_sync.status, pad, rc.bottom - pad - statusH + 2, rc.right - 2 * pad - 100, statusH, TRUE);
}

LRESULT CALLBACK SyncProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_sync.hwnd = hwnd;
            g_sync.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                          WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 10, 10,
                                          hwnd, (HMENU)kListId, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(g_sync.list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
            struct Col { const wchar_t* text; int width; };
            const Col cols[] = {{L"Имя", 300}, {L"Левая панель", 110}, {L"Правая панель", 110}, {L"Состояние", 110}};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = (LPWSTR)cols[i].text;
                c.cx = cols[i].width;
                c.iSubItem = i;
                ListView_InsertColumn(g_sync.list, i, &c);
            }

            g_sync.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            0, 0, 10, 10, hwnd, (HMENU)kStatusId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Скопировать вправо →",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE,
                            0, 0, 10, 10, hwnd, (HMENU)kCopyRightId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"← Скопировать влево",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE,
                            0, 0, 10, 10, hwnd, (HMENU)kCopyLeftId, nullptr, nullptr);
            g_sync.onlyDiff = CreateWindowExW(0, L"BUTTON", L"Только различия",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                              0, 0, 10, 10, hwnd, (HMENU)kOnlyDiffId, nullptr, nullptr);
            SendMessageW(g_sync.onlyDiff, BM_SETCHECK, BST_CHECKED, 0);
            CreateWindowExW(0, L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            0, 0, 10, 10, hwnd, (HMENU)kCloseId, nullptr, nullptr);

            g_sync.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            for (HWND k : {g_sync.list, g_sync.status, g_sync.onlyDiff, GetDlgItem(hwnd, kCopyRightId),
                           GetDlgItem(hwnd, kCopyLeftId), GetDlgItem(hwnd, kCloseId)}) {
                SendMessageW(k, WM_SETFONT, (WPARAM)g_sync.font, TRUE);
            }
            SyncLayout(hwnd);
            BuildItems();
            FillList();
            return 0;
        }
        case WM_SIZE:
            SyncLayout(hwnd);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kCopyRightId) { CopyChecked(true); return 0; }
            if (id == kCopyLeftId) { CopyChecked(false); return 0; }
            if (id == kOnlyDiffId) { FillList(); return 0; }
            if (id == kCloseId) { g_sync.done = true; return 0; }
            break;
        }
        case WM_CLOSE:
            g_sync.done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void SyncDirectories(HWND owner, const std::wstring& leftRoot, const std::wstring& rightRoot) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SyncProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSyncClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_sync = SyncState{};
    g_sync.leftRoot = leftRoot;
    g_sync.rightRoot = rightRoot;
    g_sync.done = false;

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = 760, h = 480;
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kSyncClass,
                                (L"Синхронизация каталогов: " + leftRoot + L"  ↔  " + rightRoot).c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h,
                                owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!g_sync.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}
