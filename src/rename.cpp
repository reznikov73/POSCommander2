// Пакетное переименование с живым предпросмотром и проверкой конфликтов.
#include "rename.h"

#include "ops.h"
#include "undo.h"
#include "util.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

const wchar_t* kRenameClass = L"POSRename2";
const int kTplId = 500;
const int kListId = 501;
const int kHintId = 502;
const int kStatusId = 503;

struct Row {
    std::wstring oldName;
    std::wstring newName;
    std::wstring status;
    bool ok = false;        // можно переименовывать
    bool changed = false;
};

struct State {
    HWND hwnd = nullptr;
    HWND tpl = nullptr;
    HWND list = nullptr;
    HWND hint = nullptr;
    HWND status = nullptr;
    HWND applyBtn = nullptr;
    std::wstring dir;
    std::vector<std::wstring> names;
    std::vector<Row> rows;
    int errors = 0;
    int ready = 0;
    int applied = 0;
    bool done = false;
};

State g;

void ReplaceAll(std::wstring& text, const std::wstring& what, const std::wstring& with) {
    size_t pos = 0;
    while ((pos = text.find(what, pos)) != std::wstring::npos) {
        text.replace(pos, what.size(), with);
        pos += with.size();
    }
}

std::wstring ExpandTemplate(const std::wstring& tpl, const std::wstring& oldName, size_t index,
                            size_t total) {
    std::wstring base = oldName;
    std::wstring ext;
    const size_t dot = oldName.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        base = oldName.substr(0, dot);
        ext = oldName.substr(dot);
    }

    std::wstring number = std::to_wstring(index + 1);
    std::wstring padded = number;
    int digits = (int)std::to_wstring(total > 0 ? total : 1).size();
    if (digits < 2) digits = 2;        // {nn} всегда минимум два знака: 01, 02, …
    while ((int)padded.size() < digits) padded.insert(padded.begin(), L'0');

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t date[32]{};
    swprintf(date, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    std::wstring out = tpl;
    ReplaceAll(out, L"{name}", base);
    ReplaceAll(out, L"{ext}", ext);
    ReplaceAll(out, L"{nn}", padded);
    ReplaceAll(out, L"{n}", number);
    ReplaceAll(out, L"{date}", date);

    // Расширение добавляем сами, если шаблон его не содержит.
    if (tpl.find(L"{ext}") == std::wstring::npos && !ext.empty()) out += ext;
    return out;
}

bool HasInvalidChars(const std::wstring& name) {
    static const wchar_t* bad = L"\\/:*?\"<>|";
    if (name.empty()) return true;
    if (name.back() == L'.' || name.back() == L' ') return true;
    return name.find_first_of(bad) != std::wstring::npos;
}

void RebuildPreview() {
    wchar_t tplBuf[512]{};
    GetWindowTextW(g.tpl, tplBuf, 512);
    const std::wstring tpl = Trim(tplBuf);

    g.rows.clear();
    g.errors = 0;
    g.ready = 0;

    std::map<std::wstring, int> seen;      // новые имена в нижнем регистре → сколько раз
    std::map<std::wstring, int> sources;   // исходные имена, чтобы отличить «своих» от чужих
    for (const auto& n : g.names) sources[Lower(n)] = 1;

    for (size_t i = 0; i < g.names.size(); ++i) {
        Row row;
        row.oldName = g.names[i];
        row.newName = tpl.empty() ? row.oldName : ExpandTemplate(tpl, row.oldName, i, g.names.size());
        row.changed = row.newName != row.oldName;

        if (!row.changed) {
            row.status = L"без изменений";
        } else if (HasInvalidChars(row.newName)) {
            row.status = L"недопустимое имя";
            ++g.errors;
        } else if (++seen[Lower(row.newName)] > 1) {
            row.status = L"дубликат в списке";
            ++g.errors;
        } else {
            const std::wstring full = JoinPath(g.dir, row.newName);
            if (PathExists(full) && sources.find(Lower(row.newName)) == sources.end()) {
                row.status = L"уже существует";
                ++g.errors;
            } else {
                row.status = L"ок";
                row.ok = true;
                ++g.ready;
            }
        }
        g.rows.push_back(std::move(row));
    }

    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g.list);
    for (int i = 0; i < (int)g.rows.size(); ++i) {
        const Row& row = g.rows[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = (LPWSTR)row.oldName.c_str();
        const int index = ListView_InsertItem(g.list, &item);
        if (index < 0) continue;
        ListView_SetItemText(g.list, index, 1, (LPWSTR)row.newName.c_str());
        ListView_SetItemText(g.list, index, 2, (LPWSTR)row.status.c_str());
    }
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, TRUE);

    wchar_t buf[256]{};
    swprintf(buf, 256, L"Файлов: %d · к переименованию: %d · ошибок: %d", (int)g.rows.size(), g.ready,
             g.errors);
    SetWindowTextW(g.status, buf);
    EnableWindow(g.applyBtn, g.ready > 0 && g.errors == 0);
}

void ApplyAll() {
    int done = 0;
    std::wstring firstError;
    std::vector<std::pair<std::wstring, std::wstring>> applied;

    for (const Row& row : g.rows) {
        if (!row.ok || !row.changed) continue;
        DWORD err = 0;
        const std::wstring from = JoinPath(g.dir, row.oldName);
        const std::wstring to = JoinPath(g.dir, row.newName);
        if (RenamePath(from, to, err)) {
            ++done;
            applied.emplace_back(from, to);
        } else if (firstError.empty()) {
            firstError = row.oldName + L": " + ErrText(err);
        }
    }

    wchar_t report[512]{};
    swprintf(report, 512, L"Переименовано: %d", done);
    std::wstring text = report;
    if (!firstError.empty()) text += L"\n\nПервая ошибка:\n" + firstError;
    MessageBoxW(g.hwnd, text.c_str(), L"Пакетное переименование", MB_ICONINFORMATION | MB_OK);

    UndoPushRenames(applied);
    g.applied = done;
    g.done = true;
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 10;
    const int rowH = 24;
    const int labelW = 76;
    const int editX = pad + labelW + 8;
    const int right = rc.right - pad;
    const int btnH = 26;
    const int bottom = rc.bottom - pad - btnH;

    MoveWindow(GetDlgItem(hwnd, 600), pad, pad + 4, labelW, 18, TRUE);
    MoveWindow(g.tpl, editX, pad, right - editX, rowH, TRUE);
    MoveWindow(g.hint, editX, pad + rowH + 2, right - editX, 16, TRUE);

    const int listTop = pad + rowH + 22;
    const int statusY = bottom - 24;
    MoveWindow(g.list, pad, listTop, rc.right - 2 * pad, statusY - listTop - 4, TRUE);
    MoveWindow(g.status, pad, statusY, rc.right - 2 * pad - 260, 20, TRUE);

    MoveWindow(g.applyBtn, right - 250, bottom, 150, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), right - 92, bottom, 92, btnH, TRUE);
}

LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;

            const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HWND label = CreateWindowExW(0, L"STATIC", L"Шаблон:", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                         hwnd, (HMENU)600, nullptr, nullptr);
            g.tpl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"{name}_{nn}",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                    (HMENU)kTplId, nullptr, nullptr);
            g.hint = CreateWindowExW(0, L"STATIC",
                                     L"{name} — имя · {ext} — расширение · {n} — номер · {nn} — номер с нулями · "
                                     L"{date} — дата",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, (HMENU)kHintId, nullptr,
                                     nullptr);

            g.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 10, 10, hwnd,
                                     (HMENU)kListId, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            struct Col { const wchar_t* text; int width; };
            const Col cols[] = {{L"Было", 280}, {L"Станет", 280}, {L"Статус", 190}};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = (LPWSTR)cols[i].text;
                c.cx = cols[i].width;
                c.iSubItem = i;
                ListView_InsertColumn(g.list, i, &c);
            }

            g.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                       (HMENU)kStatusId, nullptr, nullptr);
            g.applyBtn = CreateWindowExW(0, L"BUTTON", L"Переименовать",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd,
                                         (HMENU)IDOK, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10,
                            10, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

            for (HWND h : {label, g.tpl, g.hint, g.list, g.status, g.applyBtn, GetDlgItem(hwnd, IDCANCEL)}) {
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
            }

            Layout(hwnd);
            RebuildPreview();
            SetFocus(g.tpl);
            SendMessageW(g.tpl, EM_SETSEL, 0, -1);
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kTplId && HIWORD(wp) == EN_CHANGE) {
                RebuildPreview();
                return 0;
            }
            if (id == IDOK) {
                if (g.ready > 0 && g.errors == 0) ApplyAll();
                return 0;
            }
            if (id == IDCANCEL) {
                g.done = true;
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            g.done = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int BatchRenameDialog(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RenameProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kRenameClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g = State{};
    g.dir = dir;
    g.names = names;

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = 820, h = 520;
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kRenameClass, L"Пакетное переименование",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 0;

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);

    return g.applied;
}
