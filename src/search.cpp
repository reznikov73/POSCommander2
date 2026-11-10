#include "search.h"

#include "ops.h"
#include "settings.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const wchar_t* kSearchClass = L"POSSearch2";

const int kMaskId = 10;
const int kDirId = 11;
const int kTextId = 12;
const int kCaseId = 13;
const int kSubdirsId = 14;
const int kHiddenId = 15;
const int kStopId = 20;
const int kBrowseId = 22;
const int kOpenDirId = 23;
const int kListId = 30;
const int kStatusId = 31;
const int kHintId = 32;
const int kDriveIdBase = 200;      // 200..225 — кнопки корней
const int kMenuOpenDir = 1;
const int kMenuCopyPath = 2;

const int kTimerId = 1;
const size_t kMaxResults = 50000;

struct Result {
    std::wstring path;
    std::wstring name;
    std::wstring dir;
    ULONGLONG size = 0;
    FILETIME mtime{};
};

struct SearchState {
    HWND hwnd = nullptr;
    HWND mask = nullptr;
    HWND dir = nullptr;
    HWND text = nullptr;
    HWND caseCheck = nullptr;
    HWND subdirs = nullptr;
    HWND hiddenCheck = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HWND hint = nullptr;
    HWND startBtn = nullptr;
    HWND stopBtn = nullptr;
    HWND browseBtn = nullptr;
    HWND openDirBtn = nullptr;
    HWND labelMask = nullptr;
    HWND labelDir = nullptr;
    HWND labelText = nullptr;
    HWND labelRoots = nullptr;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;

    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> scanned{0};

    std::mutex mutex;
    std::vector<Result> pending;   // найдено, но ещё не показано
    std::vector<Result> rows;      // всё, что уже в списке
    int sortCol = -1;
    bool sortAsc = true;

    std::wstring chosen;
};

void FillResults(SearchState* st);
void UpdateStatus(SearchState* st);
void LayoutSearch(SearchState* st);

// ---------- поиск ----------

bool FileContainsText(const std::wstring& path, const std::wstring& needle, bool caseSensitive) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG limit = 8LL * 1024 * 1024;      // больше 8 МБ не читаем
    DWORD toRead = (DWORD)(size.QuadPart > limit ? limit : size.QuadPart);
    std::string data(toRead, '\0');
    DWORD read = 0;
    BOOL ok = toRead == 0 ? TRUE : ReadFile(f, data.data(), toRead, &read, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    data.resize(read);
    if (data.empty()) return false;

    std::wstring wide;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        wide.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
    } else {
        UINT cp = CP_ACP;
        size_t offset = 0;
        if (data.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            cp = CP_UTF8;
            offset = 3;
        } else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0) > 0) {
            cp = CP_UTF8;
        }
        int need = MultiByteToWideChar(cp, 0, data.data() + offset, (int)(data.size() - offset), nullptr, 0);
        if (need <= 0) return false;
        wide.resize((size_t)need);
        MultiByteToWideChar(cp, 0, data.data() + offset, (int)(data.size() - offset), wide.data(), need);
    }

    if (!caseSensitive) {
        wide = Lower(wide);
        return wide.find(needle) != std::wstring::npos;
    }
    return wide.find(needle) != std::wstring::npos;
}

void WalkDirectory(SearchState* st, const std::wstring& dir, const std::vector<std::wstring>& masks,
                   const std::wstring& needle, bool caseSensitive, bool recurse, bool includeHidden,
                   int depth) {
    if (st->stop.load() || depth > 32) return;

    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    do {
        if (st->stop.load()) break;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;

        const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring name = data.cFileName;
        const std::wstring full = JoinPath(dir, name);

        if (isDir) {
            if (recurse && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(full);
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!includeHidden && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;

        bool nameMatches = false;
        for (auto& m : masks) {
            if (MatchMask(Lower(name), m)) {
                nameMatches = true;
                break;
            }
        }
        st->scanned.fetch_add(1);

        if (!nameMatches) continue;
        if (!needle.empty() && !FileContainsText(full, needle, caseSensitive)) continue;

        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->rows.size() + st->pending.size() < kMaxResults) {
                Result r;
                r.path = full;
                r.name = name;
                r.dir = dir;
                r.size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
                r.mtime = data.ftLastWriteTime;
                st->pending.push_back(std::move(r));
            }
        }
    } while (FindNextFileW(find, &data));

    FindClose(find);

    for (auto& sub : subdirs) {
        if (st->stop.load()) break;
        WalkDirectory(st, sub, masks, needle, caseSensitive, recurse, includeHidden, depth + 1);
    }
}

void SavePrefs(SearchState* st) {
    const std::wstring ini = SettingsFilePath();
    if (ini.empty()) return;

    wchar_t buf[512]{};
    GetWindowTextW(st->mask, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Mask", buf, ini.c_str());
    GetWindowTextW(st->dir, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Dir", buf, ini.c_str());
    GetWindowTextW(st->text, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Text", buf, ini.c_str());

    wchar_t flags[8]{};
    swprintf(flags, 8, L"%d%d%d",
             SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->hiddenCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Search", L"Flags", flags, ini.c_str());
}

void ReadPrefs(SearchState* st, const std::wstring& startDir) {
    const std::wstring ini = SettingsFilePath();
    wchar_t buf[512]{};

    GetPrivateProfileStringW(L"Search", L"Mask", L"*", buf, 512, ini.c_str());
    SetWindowTextW(st->mask, buf[0] ? buf : L"*");

    GetPrivateProfileStringW(L"Search", L"Dir", L"", buf, 512, ini.c_str());
    SetWindowTextW(st->dir, buf[0] ? buf : startDir.c_str());

    GetPrivateProfileStringW(L"Search", L"Text", L"", buf, 512, ini.c_str());
    SetWindowTextW(st->text, buf);

    GetPrivateProfileStringW(L"Search", L"Flags", L"100", buf, 8, ini.c_str());
    const std::wstring flags = buf;
    SendMessageW(st->subdirs, BM_SETCHECK, flags.size() > 0 && flags[0] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->caseCheck, BM_SETCHECK, flags.size() > 1 && flags[1] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->hiddenCheck, BM_SETCHECK, flags.size() > 2 && flags[2] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
}

void StartWorker(SearchState* st) {
    if (st->running.load()) return;

    wchar_t maskBuf[512]{};
    GetWindowTextW(st->mask, maskBuf, 512);
    std::wstring maskText = Trim(maskBuf);
    if (maskText.empty()) maskText = L"*";

    wchar_t dirBuf[MAX_PATH * 4]{};
    GetWindowTextW(st->dir, dirBuf, MAX_PATH * 4);
    std::wstring startDir = Trim(dirBuf);
    if (startDir.empty()) {
        MessageBoxW(st->hwnd, L"Укажите, где искать: выберите корень или папку.", L"Поиск файлов",
                    MB_ICONINFORMATION | MB_OK);
        SetFocus(st->dir);
        return;
    }
    if (GetFileAttributesW(startDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(st->hwnd, (L"Такой папки нет:\n" + startDir).c_str(), L"Поиск файлов",
                    MB_ICONWARNING | MB_OK);
        SetFocus(st->dir);
        return;
    }

    wchar_t textBuf[512]{};
    GetWindowTextW(st->text, textBuf, 512);
    std::wstring needle = Trim(textBuf);
    const bool caseSensitive = SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool recurse = SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hidden = SendMessageW(st->hiddenCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!caseSensitive && !needle.empty()) needle = Lower(needle);

    auto masks = SplitMasks(maskText);
    if (masks.empty()) masks.push_back(L"*");

    SavePrefs(st);

    {
        std::lock_guard<std::mutex> lock(st->mutex);
        st->pending.clear();
        st->rows.clear();
    }
    ListView_DeleteAllItems(st->list);
    st->sortCol = -1;
    st->sortAsc = true;

    st->stop.store(false);
    st->scanned.store(0);
    st->running.store(true);
    EnableWindow(st->startBtn, FALSE);
    EnableWindow(st->stopBtn, TRUE);
    UpdateStatus(st);

    if (st->worker.joinable()) st->worker.join();
    st->worker = std::thread([st, startDir, masks, needle, caseSensitive, recurse, hidden]() {
        WalkDirectory(st, startDir, masks, needle, caseSensitive, recurse, hidden, 0);
        st->running.store(false);
        PostMessageW(st->hwnd, WM_APP + 202, 0, 0);
    });
}

// ---------- интерфейс ----------

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void LayoutSearch(SearchState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int pad = 10;
    const int labelW = 88;
    const int editX = pad + labelW + 8;
    const int rowH = 24;
    const int right = w - pad;
    const int browseW = 128;

    int y = 12;
    MoveWindow(st->labelMask, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->mask, editX, y, right - editX, rowH, TRUE);
    MoveWindow(st->hint, editX, y + rowH + 2, right - editX, 16, TRUE);

    y += rowH + 20;
    MoveWindow(st->labelDir, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->dir, editX, y, right - editX - browseW - 8, rowH, TRUE);
    MoveWindow(st->browseBtn, right - browseW, y, browseW, rowH, TRUE);

    y += rowH + 8;
    MoveWindow(st->labelRoots, pad, y + 3, labelW, 18, TRUE);
    int x = editX;
    for (int i = 0; i < 26; ++i) {
        HWND hb = GetDlgItem(st->hwnd, kDriveIdBase + i);
        if (!hb) continue;
        MoveWindow(hb, x, y, 38, 22, TRUE);
        x += 42;
    }

    y += 30;
    MoveWindow(st->labelText, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->text, editX, y, right - editX, rowH, TRUE);

    y += rowH + 8;
    MoveWindow(st->subdirs, editX, y, 150, 20, TRUE);
    MoveWindow(st->caseCheck, editX + 160, y, 170, 20, TRUE);
    MoveWindow(st->hiddenCheck, editX + 340, y, 190, 20, TRUE);

    y += 28;
    const int startW = 160;
    const int stopW = 92;
    MoveWindow(st->startBtn, right - startW, y, startW, rowH, TRUE);
    MoveWindow(st->stopBtn, right - startW - 8 - stopW, y, stopW, rowH, TRUE);
    MoveWindow(st->status, pad, y + 4, right - startW - 8 - stopW - pad - 12, 18, TRUE);

    y += rowH + 8;
    const int bottomY = h - pad - rowH;
    const int listH = (bottomY - 8 - y) > 60 ? (bottomY - 8 - y) : 60;
    MoveWindow(st->list, pad, y, w - 2 * pad, listH, TRUE);

    MoveWindow(st->openDirBtn, pad, bottomY, 170, rowH, TRUE);
    MoveWindow(GetDlgItem(st->hwnd, IDCANCEL), right - 140, bottomY, 140, rowH, TRUE);
}

void UpdateStatus(SearchState* st) {
    size_t found = 0;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        found = st->rows.size();
    }
    wchar_t buf[256]{};
    swprintf(buf, 256, L"Найдено: %zu   ·   просмотрено файлов: %d%s", found, st->scanned.load(),
             st->running.load() ? L"   ·   идёт поиск" : L"");
    SetWindowTextW(st->status, buf);
}

void FillResults(SearchState* st) {
    std::vector<Result> batch;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (!st->pending.empty()) batch.swap(st->pending);
    }
    if (batch.empty()) {
        UpdateStatus(st);
        return;
    }

    size_t base = 0;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        base = st->rows.size();
        st->rows.insert(st->rows.end(), batch.begin(), batch.end());
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        const Result& r = batch[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)(base + i);
        item.iSubItem = 0;
        item.pszText = (LPWSTR)r.name.c_str();
        item.lParam = (LPARAM)(base + i);
        const int index = ListView_InsertItem(st->list, &item);
        if (index < 0) continue;
        ListView_SetItemText(st->list, index, 1, (LPWSTR)r.dir.c_str());
        std::wstring size = FmtSize(r.size);
        ListView_SetItemText(st->list, index, 2, (LPWSTR)size.c_str());
        std::wstring date = FmtDate(r.mtime);
        ListView_SetItemText(st->list, index, 3, (LPWSTR)date.c_str());
    }
    UpdateStatus(st);
}

int CALLBACK CompareResults(LPARAM a, LPARAM b, LPARAM ctx) {
    auto* st = reinterpret_cast<SearchState*>(ctx);
    const size_t ia = (size_t)a;
    const size_t ib = (size_t)b;
    if (ia >= st->rows.size() || ib >= st->rows.size()) return 0;
    const Result& x = st->rows[ia];
    const Result& y = st->rows[ib];

    int cmp = 0;
    switch (st->sortCol) {
        case 0: cmp = CompareNatural(x.name, y.name); break;
        case 1: cmp = CompareNatural(x.dir, y.dir); break;
        case 2: cmp = (x.size < y.size) ? -1 : (x.size > y.size ? 1 : 0); break;
        case 3: cmp = CompareFileTime(&x.mtime, &y.mtime); break;
        default: cmp = 0; break;
    }
    if (cmp == 0) cmp = CompareNatural(x.name, y.name);
    return st->sortAsc ? cmp : -cmp;
}

Result* SelectedResult(SearchState* st) {
    int sel = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (sel < 0 && ListView_GetItemCount(st->list) > 0) sel = 0;
    if (sel < 0) return nullptr;

    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(st->list, &item)) return nullptr;
    const size_t index = (size_t)item.lParam;
    if (index >= st->rows.size()) return nullptr;
    return &st->rows[index];
}

void ChooseAndClose(SearchState* st, bool directoryOnly) {
    Result* r = SelectedResult(st);
    if (!r) return;
    st->chosen = directoryOnly ? r->dir + L"\\" : r->path;
    LogDebug(L"search choose '" + st->chosen + L"'");
    DestroyWindow(st->hwnd);
}

void CopySelectedPath(SearchState* st) {
    Result* r = SelectedResult(st);
    if (!r) return;
    if (!OpenClipboard(st->hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (r->path.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, r->path.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

void BrowseForFolder(SearchState* st) {
    BROWSEINFOW bi{};
    bi.hwndOwner = st->hwnd;
    bi.lpszTitle = L"Где искать";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH * 4]{};
    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(st->dir, path);
    CoTaskMemFree(pidl);
}

LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<SearchState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<SearchState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->hwnd = hwnd;

            st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            st->fontSmall = CreateFontW(-Scale(12, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            st->labelMask = MakeChild(hwnd, L"STATIC", L"Искать:", SS_LEFT, 0);
            st->labelDir = MakeChild(hwnd, L"STATIC", L"Место:", SS_LEFT, 0);
            st->labelRoots = MakeChild(hwnd, L"STATIC", L"Корни:", SS_LEFT, 0);
            st->labelText = MakeChild(hwnd, L"STATIC", L"С текстом:", SS_LEFT, 0);

            st->mask = MakeChild(hwnd, L"EDIT", L"*", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kMaskId);
            st->dir = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kDirId);
            st->text = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kTextId);
            st->hint = MakeChild(hwnd, L"STATIC", L"Через ; можно задать несколько масок: *.txt;*.md",
                                 SS_LEFT, kHintId);

            st->browseBtn = MakeChild(hwnd, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, kBrowseId);
            st->caseCheck = MakeChild(hwnd, L"BUTTON", L"Учитывать регистр", BS_AUTOCHECKBOX | WS_TABSTOP, kCaseId);
            st->subdirs = MakeChild(hwnd, L"BUTTON", L"В подкаталогах", BS_AUTOCHECKBOX | WS_TABSTOP, kSubdirsId);
            st->hiddenCheck = MakeChild(hwnd, L"BUTTON", L"Скрытые и системные", BS_AUTOCHECKBOX | WS_TABSTOP,
                                        kHiddenId);
            st->startBtn = MakeChild(hwnd, L"BUTTON", L"Начать поиск", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            st->stopBtn = MakeChild(hwnd, L"BUTTON", L"Стоп", BS_PUSHBUTTON | WS_TABSTOP, kStopId);
            st->openDirBtn = MakeChild(hwnd, L"BUTTON", L"Открыть каталог", BS_PUSHBUTTON | WS_TABSTOP, kOpenDirId);
            MakeChild(hwnd, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 300; col.iSubItem = 0; col.pszText = (LPWSTR)L"Файл";
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 380; col.iSubItem = 1; col.pszText = (LPWSTR)L"Каталог";
            ListView_InsertColumn(st->list, 1, &col);
            col.cx = 90; col.iSubItem = 2; col.pszText = (LPWSTR)L"Размер";
            ListView_InsertColumn(st->list, 2, &col);
            col.cx = 130; col.iSubItem = 3; col.pszText = (LPWSTR)L"Дата";
            ListView_InsertColumn(st->list, 3, &col);

            st->status = MakeChild(hwnd, L"STATIC", L"Готов к поиску", SS_LEFT, kStatusId);

            int slot = 0;
            for (const auto& drive : GetDrives()) {
                if (slot >= 26) break;
                std::wstring label;
                label.push_back(drive.letter);
                label += L":";
                CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(kDriveIdBase + slot), g_inst, nullptr);
                ++slot;
            }

            for (HWND h : {st->labelMask, st->labelDir, st->labelRoots, st->labelText, st->mask, st->dir,
                           st->text, st->caseCheck, st->subdirs, st->hiddenCheck, st->list, st->status,
                           st->startBtn, st->stopBtn, st->browseBtn, st->openDirBtn,
                           GetDlgItem(hwnd, IDCANCEL)}) {
                if (h && st->font) SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
            }
            if (st->hint && st->fontSmall) SendMessageW(st->hint, WM_SETFONT, (WPARAM)st->fontSmall, TRUE);
            for (int i = 0; i < 26; ++i) {
                if (HWND h = GetDlgItem(hwnd, kDriveIdBase + i))
                    SendMessageW(h, WM_SETFONT, (WPARAM)st->fontSmall, TRUE);
            }

            EnableWindow(st->stopBtn, FALSE);
            SetTimer(hwnd, kTimerId, 120, nullptr);
            return 0;
        }

        case WM_SIZE:
            if (st) LayoutSearch(st);
            return 0;

        case WM_TIMER:
            if (st && wParam == kTimerId) FillResults(st);
            return 0;

        case WM_APP + 202:
            if (st) {
                FillResults(st);
                EnableWindow(st->startBtn, TRUE);
                EnableWindow(st->stopBtn, FALSE);
                UpdateStatus(st);
            }
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (!st) break;

            if (id == IDOK) {
                if (GetFocus() == st->list) ChooseAndClose(st, false);
                else StartWorker(st);
                return 0;
            }
            if (id == IDCANCEL) {
                st->stop.store(true);
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kStopId) {
                st->stop.store(true);
                EnableWindow(st->stopBtn, FALSE);
                SetWindowTextW(st->status, L"Останавливаю поиск…");
                return 0;
            }
            if (id == kBrowseId) {
                BrowseForFolder(st);
                return 0;
            }
            if (id == kOpenDirId) {
                ChooseAndClose(st, true);
                return 0;
            }
            if (id >= kDriveIdBase && id < kDriveIdBase + 26) {
                wchar_t root[8]{};
                GetWindowTextW(reinterpret_cast<HWND>(lParam), root, 8);
                if (root[0]) {
                    std::wstring path = root;
                    if (path.back() != L'\\') path += L'\\';
                    SetWindowTextW(st->dir, path.c_str());
                }
                return 0;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* info = reinterpret_cast<NMHDR*>(lParam);
            if (!st || !info || info->idFrom != kListId) break;

            if (info->code == NM_DBLCLK || info->code == NM_RETURN) {
                ChooseAndClose(st, false);
                return 0;
            }
            if (info->code == LVN_KEYDOWN) {
                auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                if (key->wVKey == VK_RETURN) {
                    ChooseAndClose(st, false);
                    return 0;
                }
            }
            if (info->code == LVN_COLUMNCLICK) {
                auto* click = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (st->sortCol == click->iSubItem) {
                    st->sortAsc = !st->sortAsc;
                } else {
                    st->sortCol = click->iSubItem;
                    st->sortAsc = true;
                }
                ListView_SortItems(st->list, CompareResults, (LPARAM)st);
                return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            if (!st || reinterpret_cast<HWND>(wParam) != st->list) break;
            if (!SelectedResult(st)) return 0;

            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuOpenDir, L"Открыть каталог");
            AppendMenuW(menu, MF_STRING, kMenuCopyPath, L"Скопировать путь");
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                GetWindowRect(st->list, &rc);
                pt.x = rc.left + 20;
                pt.y = rc.top + 20;
            }
            const int cmd =
                (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == kMenuOpenDir) ChooseAndClose(st, true);
            if (cmd == kMenuCopyPath) CopySelectedPath(st);
            return 0;
        }

        case WM_CLOSE:
            if (st) st->stop.store(true);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                st->stop.store(true);
                SavePrefs(st);

                RECT wr{};
                if (GetWindowRect(hwnd, &wr)) {
                    const std::wstring iniPath = SettingsFilePath();
                    wchar_t buf[32]{};
                    swprintf(buf, 32, L"%d", (int)wr.left);
                    WritePrivateProfileStringW(L"Search", L"X", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)wr.top);
                    WritePrivateProfileStringW(L"Search", L"Y", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.right - wr.left));
                    WritePrivateProfileStringW(L"Search", L"W", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.bottom - wr.top));
                    WritePrivateProfileStringW(L"Search", L"H", buf, iniPath.c_str());
                }
                if (st->worker.joinable()) st->worker.join();
                KillTimer(hwnd, kTimerId);
                if (st->font) DeleteObject(st->font);
                if (st->fontSmall) DeleteObject(st->fontSmall);
                st->font = nullptr;
                st->fontSmall = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::wstring SearchDialog(HWND owner, const std::wstring& startDir) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SearchProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSearchClass;
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);
        registered = true;
    }

    SearchState state;
    state.chosen.clear();

    RECT ownerRect{0, 0, 900, 600};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

    // Позиция и размер запоминаются между вызовами.
    const std::wstring iniPath = SettingsFilePath();
    const int savedX = (int)GetPrivateProfileIntW(L"Search", L"X", ownerRect.left + 60, iniPath.c_str());
    const int savedY = (int)GetPrivateProfileIntW(L"Search", L"Y", ownerRect.top + 40, iniPath.c_str());
    const int savedW = (int)GetPrivateProfileIntW(L"Search", L"W", 940, iniPath.c_str());
    const int savedH = (int)GetPrivateProfileIntW(L"Search", L"H", 640, iniPath.c_str());
    const RECT frame = ClampWindowRect(RECT{savedX, savedY, savedX + savedW, savedY + savedH});
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kSearchClass, L"Поиск файлов",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, frame.left, frame.top,
                                width, height, nullptr, nullptr, g_inst, &state);
    if (!hwnd) return {};

    ReadPrefs(&state, startDir);
    LayoutSearch(&state);

    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(state.mask);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (ownerWasEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        SetFocus(owner);
    }
    return state.chosen;
}
