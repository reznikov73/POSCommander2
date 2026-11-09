#include "archive.h"

#include "dialogs.h"
#include "util.h"

#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* kArchiveClass = L"POSArchive2";

const int kListId = 40;
const int kExtractAllId = 41;
const int kExtractSelId = 42;
const int kCloseId = 43;
const int kStatusId = 44;

struct ArchiveState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    std::wstring path;
    std::vector<ArchiveEntry> entries;
    std::wstring extractedTo;
};

// Путь к 7z.exe: обычно Program Files, иначе — из PATH.
std::wstring Find7z() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    const wchar_t* candidates[] = {
        L"C:\\Program Files\\7-Zip\\7z.exe",
        L"C:\\Program Files (x86)\\7-Zip\\7z.exe",
    };
    for (auto c : candidates) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
            cached = c;
            return cached;
        }
    }

    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"7z.exe", nullptr, MAX_PATH, found, nullptr)) cached = found;
    return cached;
}

// Запуск консольной команды с захватом вывода (UTF-8).
bool RunCapture(const std::wstring& commandLine, std::string& output) {
    output.clear();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, read);
    }

    WaitForSingleObject(pi.hProcess, 120000);
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) {
        need = MultiByteToWideChar(CP_OEMCP, 0, text.data(), (int)text.size(), nullptr, 0);
        if (need <= 0) return {};
        std::wstring out((size_t)need, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, text.data(), (int)text.size(), out.data(), need);
        return out;
    }
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

std::wstring TrimW(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    return s.substr(i);
}

// Разбор вывода `7z l -slt -ba`: блоки полей через пустую строку.
std::vector<ArchiveEntry> ParseListing(const std::string& raw) {
    std::vector<ArchiveEntry> entries;
    const std::wstring text = Utf8ToWide(raw);

    ArchiveEntry current;
    bool hasPath = false;
    size_t pos = 0;

    auto flush = [&]() {
        if (hasPath && !current.name.empty()) {
            // Первая запись — сам архив (у него нет атрибута каталога, но есть Type)
            if (!current.isDir) entries.push_back(current);
        }
        current = ArchiveEntry{};
        hasPath = false;
    };

    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        std::wstring line = (end == std::wstring::npos) ? text.substr(pos) : text.substr(pos, end - pos);
        pos = (end == std::wstring::npos) ? text.size() + 1 : end + 1;
        line = TrimW(line);

        if (line.empty()) {
            flush();
            continue;
        }
        const size_t eq = line.find(L" = ");
        if (eq == std::wstring::npos) continue;
        const std::wstring key = line.substr(0, eq);
        const std::wstring value = line.substr(eq + 3);

        if (key == L"Path") {
            current.name = value;
            hasPath = true;
        } else if (key == L"Size") {
            current.size = value;
        } else if (key == L"Modified") {
            current.modified = value;
        } else if (key == L"Attributes") {
            if (value.find(L'D') != std::wstring::npos) current.isDir = true;
            else if (value.find(L"Directory") != std::wstring::npos) current.isDir = true;
        } else if (key == L"Folder") {
            if (value == L"+") current.isDir = true;
        }
    }
    flush();

    // Первая запись описывает сам архив — убираем её, если совпало имя файла.
    if (!entries.empty()) {
        const size_t slash = entries.front().name.find_last_of(L"\\/");
        const std::wstring firstName = (slash == std::wstring::npos) ? entries.front().name : entries.front().name.substr(slash + 1);
        if (entries.size() > 1 && entries.front().size.empty() && !entries.front().isDir &&
            firstName.find(L'.') != std::wstring::npos) {
            entries.erase(entries.begin());
        }
    }
    return entries;
}

void FillList(ArchiveState* st) {
    ListView_DeleteAllItems(st->list);
    int index = 0;
    for (auto& e : st->entries) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = (LPWSTR)e.name.c_str();
        int row = ListView_InsertItem(st->list, &item);
        if (row >= 0) {
            ListView_SetItemText(st->list, row, 1, (LPWSTR)(e.isDir ? L"<папка>" : e.size.c_str()));
            ListView_SetItemText(st->list, row, 2, (LPWSTR)e.modified.c_str());
        }
        ++index;
    }

    wchar_t buf[512]{};
    swprintf(buf, 512, L"Файлов в архиве: %d   ·   %s", (int)st->entries.size(), st->path.c_str());
    SetWindowTextW(st->status, buf);
}

void LayoutArchive(ArchiveState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int btnH = 26;
    const int statusH = 22;
    const int pad = 8;

    MoveWindow(st->list, pad, pad, w - 2 * pad, h - pad * 2 - btnH - statusH, TRUE);
    MoveWindow(st->status, pad, h - btnH - statusH - pad, w - 2 * pad, statusH, TRUE);
    int x = pad;
    for (int id : {kExtractAllId, kExtractSelId, kCloseId}) {
        HWND b = GetDlgItem(st->hwnd, id);
        if (b) {
            MoveWindow(b, x, h - btnH - pad, 150, btnH, TRUE);
            x += 156;
        }
    }
}

std::wstring QuoteArg(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

// Распаковка: entries — список путей внутри архива (пусто = всё).
bool ExtractEntries(ArchiveState* st, const std::vector<std::wstring>& entries, const std::wstring& target,
                    std::wstring& error) {
    const std::wstring exe = Find7z();
    if (exe.empty()) {
        error = L"7z.exe не найден. Установите 7-Zip.";
        return false;
    }

    CreateDirectoryW(target.c_str(), nullptr);

    std::wstring cmd = QuoteArg(exe) + L" x -y -bso0 -bsp0 -sccUTF-8 " + QuoteArg(st->path) + L" -o" + QuoteArg(target);
    for (auto& e : entries) cmd += L" " + QuoteArg(e);

    std::string output;
    if (!RunCapture(cmd, output)) {
        error = L"Не удалось запустить 7z.exe";
        return false;
    }
    return true;
}

void DoExtract(ArchiveState* st, bool selectedOnly) {
    std::vector<std::wstring> names;
    if (selectedOnly) {
        int index = -1;
        while ((index = ListView_GetNextItem(st->list, index, LVNI_SELECTED)) >= 0) {
            if (index < (int)st->entries.size()) names.push_back(st->entries[index].name);
        }
        if (names.empty()) {
            MessageBoxW(st->hwnd, L"Ничего не выбрано.", L"Архив", MB_ICONINFORMATION);
            return;
        }
    }

    std::wstring target;
    {
        wchar_t buf[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, buf);
        target = buf;
    }
    if (!InputBox(st->hwnd, L"Распаковка", L"Куда распаковать:", target, true)) return;

    std::wstring error;
    if (!ExtractEntries(st, names, target, error)) {
        MessageBoxW(st->hwnd, error.c_str(), L"Архив", MB_ICONERROR);
        return;
    }
    st->extractedTo = target;
    MessageBoxW(st->hwnd, (L"Распаковано в:\n" + target).c_str(), L"Архив", MB_ICONINFORMATION);
}

LRESULT CALLBACK ArchiveProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<ArchiveState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<ArchiveState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->hwnd = hwnd;

            st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 380; col.iSubItem = 0; col.pszText = (LPWSTR)L"Имя";
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 110; col.iSubItem = 1; col.pszText = (LPWSTR)L"Размер";
            ListView_InsertColumn(st->list, 1, &col);
            col.cx = 170; col.iSubItem = 2; col.pszText = (LPWSTR)L"Изменён";
            ListView_InsertColumn(st->list, 2, &col);

            struct { const wchar_t* text; int id; } buttons[] = {
                {L"Извлечь всё…", kExtractAllId},
                {L"Извлечь выбранное", kExtractSelId},
                {L"Закрыть", kCloseId},
            };
            for (auto& b : buttons) {
                HWND h = CreateWindowExW(0, L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                         0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)b.id, g_inst, nullptr);
                if (h && st->font) SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
            }

            st->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kStatusId, g_inst, nullptr);
            if (st->font) {
                SendMessageW(st->list, WM_SETFONT, (WPARAM)st->font, TRUE);
                SendMessageW(st->status, WM_SETFONT, (WPARAM)st->font, TRUE);
            }

            FillList(st);
            return 0;
        }

        case WM_SIZE:
            if (st) LayoutArchive(st);
            return 0;

        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wParam);
            if (id == kExtractAllId) { DoExtract(st, false); return 0; }
            if (id == kExtractSelId) { DoExtract(st, true); return 0; }
            if (id == kCloseId) { DestroyWindow(hwnd); return 0; }
            break;
        }

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

bool ListArchive(const std::wstring& archive, std::vector<ArchiveEntry>& out) {
    out.clear();
    const std::wstring exe = Find7z();
    if (exe.empty()) return false;

    std::string raw;
    if (!RunCapture(QuoteArg(exe) + L" l -slt -ba -sccUTF-8 " + QuoteArg(archive), raw)) return false;
    out = ParseListing(raw);
    return true;
}

namespace {

// ---------- окно прогресса распаковки ----------
const wchar_t* kUnpackClass = L"POSUnpackProgress2";
const int kUnpTextId = 70;
const int kUnpPercentId = 71;
const int kUnpBarId = 72;
const int kUnpCancelId = 73;

struct UnpackProgress {
    HWND hwnd = nullptr;
    HWND text = nullptr;
    HWND percent = nullptr;
    HWND bar = nullptr;
    HFONT font = nullptr;
    HWND owner = nullptr;
    bool ownerDisabled = false;
    bool cancelled = false;
    int value = -1;
};

UnpackProgress g_unp;

void UnpLayout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 12, lineH = 20, barH = 18, btnW = 100, btnH = 26;
    MoveWindow(g_unp.text, pad, pad, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_unp.percent, pad, pad + lineH + 6, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_unp.bar, pad, pad + 2 * lineH + 14, rc.right - 2 * pad, barH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kUnpCancelId), rc.right - pad - btnW, rc.bottom - pad - btnH, btnW, btnH, TRUE);
}

LRESULT CALLBACK UnpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_unp.hwnd = hwnd;
            g_unp.text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                         0, 0, 10, 10, hwnd, (HMENU)kUnpTextId, nullptr, nullptr);
            g_unp.percent = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            0, 0, 10, 10, hwnd, (HMENU)kUnpPercentId, nullptr, nullptr);
            g_unp.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                        0, 0, 10, 10, hwnd, (HMENU)kUnpBarId, nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                          0, 0, 10, 10, hwnd, (HMENU)kUnpCancelId, nullptr, nullptr);
            g_unp.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            const HWND kids[] = {g_unp.text, g_unp.percent, g_unp.bar, cancel};
            for (HWND k : kids) SendMessageW(k, WM_SETFONT, (WPARAM)g_unp.font, TRUE);
            SendMessageW(g_unp.bar, PBM_SETRANGE32, 0, 100);
            UnpLayout(hwnd);
            return 0;
        }
        case WM_SIZE:
            UnpLayout(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kUnpCancelId) {
                g_unp.cancelled = true;
                EnableWindow(GetDlgItem(hwnd, kUnpCancelId), FALSE);
                SetWindowTextW(g_unp.percent, L"Отмена…");
                return 0;
            }
            break;
        case WM_CLOSE:              // крестик = отмена, окно закрывает код распаковки
            g_unp.cancelled = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void UnpShow(HWND owner, const std::wstring& archiveName) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = UnpProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kUnpackClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_unp.cancelled = false;
    g_unp.value = -1;
    g_unp.owner = owner;

    const int w = 460, h = 160;
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kUnpackClass, L"Распаковка",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h,
                                owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;
    SetWindowTextW(g_unp.text, archiveName.c_str());
    SetWindowTextW(g_unp.percent, L"0%");
    EnableWindow(owner, FALSE);
    g_unp.ownerDisabled = true;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    LogDebug(L"unpack: окно прогресса создано hwnd=" + std::to_wstring((unsigned long long)(uintptr_t)hwnd));
}

void UnpSetProgress(int percent) {
    if (!g_unp.hwnd || percent == g_unp.value) return;
    g_unp.value = percent;
    SendMessageW(g_unp.bar, PBM_SETPOS, (WPARAM)percent, 0);
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%d%%", percent);
    SetWindowTextW(g_unp.percent, buf);
}

// «… 45% 12 - file.txt» → 45; заодно запоминаем последнюю строку для отчёта.
void UnpParse(const std::string& chunk, std::wstring& status) {
    if (chunk.empty()) return;
    size_t start = 0;
    while (start < chunk.size()) {
        const size_t end = chunk.find_first_of("\r\n", start);
        const std::string piece = chunk.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = (end == std::string::npos) ? chunk.size() : end + 1;
        if (piece.empty()) continue;

        const size_t pct = piece.find('%');
        if (pct != std::string::npos) {
            size_t i = pct;
            while (i > 0 && (piece[i - 1] == ' ')) --i;
            size_t j = i;
            while (j > 0 && piece[j - 1] >= '0' && piece[j - 1] <= '9') --j;
            if (j < i) {
                const int value = atoi(piece.substr(j, i - j).c_str());
                UnpSetProgress(value < 0 ? 0 : (value > 100 ? 100 : value));
            }
        }
        if (!status.empty() || pct != std::string::npos) {
            // последняя содержательная строка — её покажем при ошибке
        }
    }
    // хвост строки после последнего \r\n
    const size_t nl = chunk.find_last_of("\r\n");
    if (nl != std::string::npos && nl + 1 < chunk.size()) status = L"";
}

void UnpHide() {
    LogDebug(L"unpack: окно прогресса закрывается, есть=" + std::to_wstring(g_unp.hwnd ? 1 : 0));
    if (!g_unp.hwnd) return;
    HWND hwnd = g_unp.hwnd;
    g_unp.hwnd = nullptr;
    DestroyWindow(hwnd);
    if (g_unp.ownerDisabled && g_unp.owner) {
        EnableWindow(g_unp.owner, TRUE);
        SetActiveWindow(g_unp.owner);
    }
    g_unp.ownerDisabled = false;
}

void UnpPump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (g_unp.hwnd && IsDialogMessageW(g_unp.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

bool ExtractArchiveWithProgress(HWND owner, const std::wstring& archive,
                                const std::wstring& target, const std::wstring& password,
                                std::wstring& error) {
    const std::wstring exe = Find7z();
    if (exe.empty()) {
        error = L"7z.exe не найден. Установите 7-Zip.";
        return false;
    }
    CreateDirectoryW(target.c_str(), nullptr);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        error = L"Не удалось создать канал для вывода 7z.";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    std::wstring cmd = QuoteArg(exe) + L" x -y -bsp1 -bso1 -sccUTF-8 " + QuoteArg(archive) +
                       L" -o" + QuoteArg(target);
    if (!password.empty()) cmd += L" " + QuoteArg(L"-p" + password);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        error = L"Не удалось запустить 7z.exe";
        return false;
    }
    CloseHandle(writePipe);

    std::wstring archiveName = archive;
    const size_t slash = archiveName.find_last_of(L'\\');
    if (slash != std::wstring::npos) archiveName = archiveName.substr(slash + 1);

    // Окно показываем не сразу: мелкий архив распаковывается за десятки мс и окно
    // только мигало бы. Как в TC — прогресс появляется, если работа затянулась.
    const ULONGLONG started = GetTickCount64();
    bool windowShown = false;

    std::string output;
    std::wstring status;
    char buffer[4096];
    bool cancelled = false;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            const DWORD want = available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
            if (ReadFile(readPipe, buffer, want, &got, nullptr) && got > 0) {
                output.append(buffer, got);
                if (output.size() > 8192) output.erase(0, output.size() - 8192);
                UnpParse(std::string(buffer, got), status);
            }
        }

        if (!windowShown && GetTickCount64() - started > 250) {
            UnpShow(owner, archiveName);
            windowShown = (g_unp.hwnd != nullptr);
        }

        UnpPump();

        if (g_unp.cancelled) {
            cancelled = true;
            TerminateProcess(pi.hProcess, 1);
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            // дочитать остаток канала
            DWORD got = 0;
            while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
                output.append(buffer, got);
            }
            break;
        }
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (windowShown) UnpHide();

    if (cancelled) {
        error = L"Распаковка отменена.";
        return false;
    }
    if (code != 0) {
        error = L"7z завершился с кодом " + std::to_wstring(code) + L".\\n\\n" + Utf8ToWide(output);
        return false;
    }
    return true;
}

bool ExtractFromArchive(const std::wstring& archive, const std::vector<std::wstring>& innerPaths,
                        const std::wstring& target, std::wstring& error) {
    const std::wstring exe = Find7z();
    if (exe.empty()) {
        error = L"7z.exe не найден. Установите 7-Zip.";
        return false;
    }

    CreateDirectoryW(target.c_str(), nullptr);

    std::wstring cmd = QuoteArg(exe) + L" x -y -bso0 -bsp0 -sccUTF-8 " + QuoteArg(archive) + L" -o" + QuoteArg(target);
    for (auto& e : innerPaths) cmd += L" " + QuoteArg(e);

    std::string output;
    if (!RunCapture(cmd, output)) {
        error = L"Не удалось запустить 7z.exe";
        return false;
    }
    return true;
}

bool CreateArchive(const std::wstring& archivePath, const std::vector<std::wstring>& items,
                   std::wstring& error) {
    const std::wstring exe = Find7z();
    if (exe.empty()) {
        error = L"7z.exe не найден. Установите 7-Zip.";
        return false;
    }
    if (items.empty()) {
        error = L"Нечего упаковывать.";
        return false;
    }

    // Формат по расширению итогового файла.
    std::wstring ext = Lower(archivePath.substr(archivePath.find_last_of(L'.') == std::wstring::npos
                                                    ? archivePath.size()
                                                    : archivePath.find_last_of(L'.')));
    std::wstring format = L"-t7z";
    if (ext == L".zip") format = L"-tzip";
    else if (ext == L".tar") format = L"-ttar";
    else if (ext == L".gz") format = L"-tgzip";

    std::wstring cmd = QuoteArg(exe) + L" a " + format + L" -y -bso0 -bsp0 -sccUTF-8 " + QuoteArg(archivePath);
    for (auto& item : items) cmd += L" " + QuoteArg(item);

    std::string output;
    if (!RunCapture(cmd, output)) {
        error = L"Не удалось запустить 7z.exe";
        return false;
    }
    if (GetFileAttributesW(archivePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Архив не создан.\n\n" + Utf8ToWide(output);
        return false;
    }
    return true;
}

std::wstring ArchiveTempDir() {
    wchar_t tmp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmp)) return L".";
    std::wstring dir = std::wstring(tmp) + L"posc2-arc";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool IsArchivePath(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = Lower(path.substr(dot));
    static const wchar_t* known[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".bz2", L".xz", L".cab", L".iso"};
    for (auto k : known) {
        if (ext == k) return true;
    }
    return false;
}

std::wstring ArchiveDialog(HWND owner, const std::wstring& archivePath, const std::wstring& defaultTarget) {
    const std::wstring exe = Find7z();
    if (exe.empty()) {
        MessageBoxW(owner, L"7z.exe не найден. Установите 7-Zip, чтобы открыть архив.", L"Архив", MB_ICONERROR);
        return {};
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ArchiveProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kArchiveClass;
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);
        registered = true;
    }

    ArchiveState state;
    state.path = archivePath;

    std::string raw;
    const std::wstring cmd = QuoteArg(exe) + L" l -slt -ba -sccUTF-8 " + QuoteArg(archivePath);
    if (!RunCapture(cmd, raw)) {
        MessageBoxW(owner, L"Не удалось прочитать архив.", L"Архив", MB_ICONERROR);
        return {};
    }
    state.entries = ParseListing(raw);

    RECT ownerRect{0, 0, 900, 600};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kArchiveClass,
                                (L"Архив — " + archivePath.substr(archivePath.find_last_of(L'\\') + 1)).c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, ownerRect.left + 70, ownerRect.top + 70, 820, 560,
                                nullptr, nullptr, g_inst, &state);
    if (!hwnd) return {};

    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(state.list);

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
    (void)defaultTarget;
    return state.extractedTo;
}
