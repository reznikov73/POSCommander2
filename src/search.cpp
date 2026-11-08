#include "search.h"

#include "ops.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>

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
const int kStartId = 20;
const int kCancelId = 21;
const int kListId = 30;
const int kStatusId = 31;
const int kBrowseId = 22;

const int kTimerId = 1;
const size_t kMaxResults = 50000;

struct SearchState {
    HWND hwnd = nullptr;
    HWND mask = nullptr;
    HWND dir = nullptr;
    HWND text = nullptr;
    HWND caseCheck = nullptr;
    HWND subdirs = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;

    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> scanned{0};

    std::mutex mutex;
    std::vector<std::wstring> pending;   // найдено, но ещё не показано
    size_t shown = 0;

    std::wstring chosen;
};

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
                   const std::wstring& needle, bool caseSensitive, bool recurse, int depth) {
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
        std::wstring name = data.cFileName;
        std::wstring full = JoinPath(dir, name);

        if (isDir) {
            if (recurse && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(full);
            continue;
        }

        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

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
            if (st->pending.size() + st->shown < kMaxResults) st->pending.push_back(full);
        }
    } while (FindNextFileW(find, &data));

    FindClose(find);

    for (auto& sub : subdirs) {
        if (st->stop.load()) break;
        WalkDirectory(st, sub, masks, needle, caseSensitive, recurse, depth + 1);
    }
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
    if (startDir.empty()) return;

    wchar_t textBuf[512]{};
    GetWindowTextW(st->text, textBuf, 512);
    std::wstring needle = Trim(textBuf);
    const bool caseSensitive = SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool recurse = SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!caseSensitive && !needle.empty()) needle = Lower(needle);

    auto masks = SplitMasks(maskText);
    if (masks.empty()) masks.push_back(L"*");

    {
        std::lock_guard<std::mutex> lock(st->mutex);
        st->pending.clear();
        st->shown = 0;
    }
    ListView_DeleteAllItems(st->list);
    st->stop.store(false);
    st->scanned.store(0);
    st->running.store(true);
    SetWindowTextW(st->status, L"Идёт поиск…");
    EnableWindow(st->mask, FALSE);
    EnableWindow(st->dir, FALSE);
    EnableWindow(st->text, FALSE);

    if (st->worker.joinable()) st->worker.join();
    st->worker = std::thread([st, startDir, masks, needle, caseSensitive, recurse]() {
        WalkDirectory(st, startDir, masks, needle, caseSensitive, recurse, 0);
        st->running.store(false);
        PostMessageW(st->hwnd, WM_APP + 202, 0, 0);
    });
}

// ---------- интерфейс ----------

void LayoutSearch(SearchState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int pad = 8;
    const int rowH = 24;
    const int btnW = 120;

    int y = pad;
    MoveWindow(st->mask, 90, y, w - 90 - pad - btnW - pad, rowH, TRUE);
    MoveWindow(GetDlgItem(st->hwnd, kStartId), w - pad - btnW, y, btnW, rowH, TRUE);
    y += rowH + 6;

    MoveWindow(st->dir, 90, y, w - 90 - pad - btnW - pad, rowH, TRUE);
    MoveWindow(GetDlgItem(st->hwnd, kBrowseId), w - pad - btnW, y, btnW, rowH, TRUE);
    y += rowH + 6;

    MoveWindow(st->text, 90, y, w - 90 - pad, rowH, TRUE);
    y += rowH + 6;

    MoveWindow(st->caseCheck, 90, y, 220, 20, TRUE);
    MoveWindow(st->subdirs, 320, y, 260, 20, TRUE);
    y += 26;

    MoveWindow(st->status, pad, y, w - 2 * pad, 20, TRUE);
    y += 24;

    const int listH = (rc.bottom - y - pad - rowH - 6 > 60) ? (rc.bottom - y - pad - rowH - 6) : 60;
    MoveWindow(st->list, pad, y, w - 2 * pad, listH, TRUE);
    y += listH + 6;

    MoveWindow(GetDlgItem(st->hwnd, kCancelId), w - pad - btnW, y, btnW, rowH, TRUE);
}

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void FillResults(SearchState* st) {
    std::vector<std::wstring> batch;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (!st->pending.empty()) {
            batch.swap(st->pending);
        }
    }
    for (auto& path : batch) {
        std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
        std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = (int)st->shown;
        item.pszText = (LPWSTR)name.c_str();
        int index = ListView_InsertItem(st->list, &item);
        if (index >= 0) ListView_SetItemText(st->list, index, 1, (LPWSTR)dir.c_str());
        st->shown++;
    }
    if (!batch.empty()) ListView_EnsureVisible(st->list, (int)st->shown - 1, FALSE);

    wchar_t buf[256]{};
    swprintf(buf, 256, L"Найдено: %zu   ·   просмотрено файлов: %d%s", st->shown, st->scanned.load(),
             st->running.load() ? L"   ·   идёт поиск" : L"");
    SetWindowTextW(st->status, buf);
}

void ChooseAndClose(SearchState* st) {
    int sel = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (sel < 0) sel = 0;
    if (ListView_GetItemCount(st->list) == 0) return;

    wchar_t name[512]{};
    wchar_t dir[MAX_PATH * 4]{};
    ListView_GetItemText(st->list, sel, 0, name, 512);
    ListView_GetItemText(st->list, sel, 1, dir, MAX_PATH * 4);
    st->chosen = std::wstring(dir) + L"\\" + name;
    LogDebug(L"search choose row=" + std::to_wstring(sel) + L" path='" + st->chosen + L"'");
    DestroyWindow(st->hwnd);
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

            st->mask = MakeChild(hwnd, L"EDIT", L"*", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kMaskId);
            st->dir = MakeChild(hwnd, L"EDIT", st ? L"" : L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kDirId);
            st->text = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kTextId);
            st->caseCheck = MakeChild(hwnd, L"BUTTON", L"Учитывать регистр", BS_AUTOCHECKBOX | WS_TABSTOP, kCaseId);
            st->subdirs = MakeChild(hwnd, L"BUTTON", L"В подкаталогах", BS_AUTOCHECKBOX | WS_TABSTOP, kSubdirsId);
            SendMessageW(st->subdirs, BM_SETCHECK, BST_CHECKED, 0);
            MakeChild(hwnd, L"BUTTON", L"Начать поиск", BS_PUSHBUTTON | WS_TABSTOP, kStartId);
            MakeChild(hwnd, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, kCancelId);
            MakeChild(hwnd, L"BUTTON", L"Диски…", BS_PUSHBUTTON | WS_TABSTOP, kBrowseId);

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 260; col.iSubItem = 0; col.pszText = (LPWSTR)L"Файл";
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 520; col.iSubItem = 1; col.pszText = (LPWSTR)L"Каталог";
            ListView_InsertColumn(st->list, 1, &col);

            st->status = MakeChild(hwnd, L"STATIC", L"Готов к поиску", SS_LEFT, kStatusId);

            for (HWND h : {st->mask, st->dir, st->text, st->caseCheck, st->subdirs, st->list, st->status,
                           GetDlgItem(hwnd, kStartId), GetDlgItem(hwnd, kCancelId), GetDlgItem(hwnd, kBrowseId)}) {
                if (h && st->font) SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
            }

            SetTimer(hwnd, kTimerId, 120, nullptr);
            SetFocus(st->mask);
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
                EnableWindow(st->mask, TRUE);
                EnableWindow(st->dir, TRUE);
                EnableWindow(st->text, TRUE);
            }
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!st) break;
            if (id == kStartId) {
                StartWorker(st);
                return 0;
            }
            if (id == kCancelId) {
                st->stop.store(true);
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kBrowseId) {
                // Быстрый выбор корня: первый доступный диск.
                auto drives = GetDrives();
                if (!drives.empty()) {
                    std::wstring root;
                    root.push_back(drives[0].letter);
                    root += L":\\";
                    SetWindowTextW(st->dir, root.c_str());
                }
                return 0;
            }
            if (id == kListId && HIWORD(wParam) == LBN_DBLCLK) {
                ChooseAndClose(st);
                return 0;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* info = reinterpret_cast<NMHDR*>(lParam);
            if (st && info && info->idFrom == kListId) {
                if (info->code == NM_DBLCLK || info->code == NM_RETURN) {
                    ChooseAndClose(st);
                    return 0;
                }
                if (info->code == LVN_KEYDOWN) {
                    auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                    if (key->wVKey == VK_RETURN) {
                        ChooseAndClose(st);
                        return 0;
                    }
                }
            }
            break;
        }

        case WM_CLOSE:
            if (st) st->stop.store(true);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                st->stop.store(true);
                if (st->worker.joinable()) st->worker.join();
                KillTimer(hwnd, kTimerId);
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
    int width = 900;
    int height = 620;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kSearchClass, L"Поиск файлов",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, ownerRect.left + 60, ownerRect.top + 60,
                                width, height, nullptr, nullptr, g_inst, &state);
    if (!hwnd) return {};

    // Заголовки полей и начальные значения — после создания, чтобы не усложнять шаблон.
    SetWindowTextW(state.mask, L"*");
    SetWindowTextW(state.dir, startDir.c_str());

    HWND labelMask = CreateWindowExW(0, L"STATIC", L"Искать файлы:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     8, 10, 78, 20, hwnd, nullptr, g_inst, nullptr);
    HWND labelDir = CreateWindowExW(0, L"STATIC", L"Место поиска:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    8, 40, 78, 20, hwnd, nullptr, g_inst, nullptr);
    HWND labelText = CreateWindowExW(0, L"STATIC", L"С текстом:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     8, 70, 78, 20, hwnd, nullptr, g_inst, nullptr);
    for (HWND h : {labelMask, labelDir, labelText}) {
        if (state.font) SendMessageW(h, WM_SETFONT, (WPARAM)state.font, TRUE);
    }

    LayoutSearch(&state);

    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(state.mask);

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
    return state.chosen;
}
