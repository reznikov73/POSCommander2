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
