// Справочник горячих клавиш: список с разделами и живым поиском по клавишам и описанию.
#include "help.h"

#include "util.h"

#include <commctrl.h>

#include <string>
#include <vector>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (int)(sizeof(a) / sizeof((a)[0]))
#endif

namespace {

struct HelpRow {
    const wchar_t* keys;
    const wchar_t* action;
};

struct HelpSection {
    const wchar_t* title;
    const HelpRow* rows;
    int count;
};

const HelpRow kFiles[] = {
    {L"F3", L"просмотр файла (Esc закрывает)"},
    {L"F4", L"правка во внешнем редакторе"},
    {L"F5", L"копирование (диалог: путь, маска, очередь)"},
    {L"F6", L"перемещение"},
    {L"F7", L"создать каталог"},
    {L"F8 / Del", L"удалить в корзину"},
    {L"Shift+Del", L"удалить мимо корзины (безвозвратно)"},
    {L"Shift+F5", L"копия рядом под новым именем"},
    {L"Shift+F7", L"создать каталог в неактивной панели"},
    {L"Shift+F4", L"новый текстовый файл и блокнот"},
    {L"F2 / Shift+F6", L"переименование прямо в строке"},
    {L"Alt+Enter", L"свойства объекта"},
    {L"Alt+Shift+Enter", L"посчитать размеры каталогов"},
    {L"Ctrl+L", L"пересчитать размеры отмеченного"},
    {L"F5 (вторая панель в архиве)", L"добавить файлы в архив"},
    {L"F8 (панель внутри архива)", L"удалить записи из архива"},
    {L"Alt+F5", L"упаковать в архив"},
};

const HelpRow kMark[] = {
    {L"Insert / Space", L"отметить и перейти на строку ниже"},
    {L"Shift+стрелки", L"отметить диапазон от курсора"},
    {L"Shift+клик / Ctrl+клик", L"диапазон / переключить одну запись"},
    {L"Num + / Num - / Num *", L"отметить / снять / инвертировать по маске"},
    {L"Ctrl+A", L"выделить всё"},
    {L"Space на папке", L"посчитать её размер и показать внизу"},
};

const HelpRow kNav[] = {
    {L"Enter", L"войти в каталог, открыть файл"},
    {L"Backspace", L"на уровень вверх"},
    {L"Ctrl+PgUp / Ctrl+PgDn", L"наверх / войти в каталог или архив"},
    {L"Ctrl+\\", L"в корень диска"},
    {L"Alt+← / Alt+→", L"назад / вперёд по истории"},
    {L"Alt+↓", L"история каталогов (в поле пути)"},
    {L"Tab", L"переключить панель"},
    {L"Ctrl+U", L"поменять панели местами"},
    {L"Alt+F1 / Alt+F2", L"меню дисков левой / правой панели"},
    {L"Ctrl+R", L"обновить панель"},
    {L"Буквы", L"быстрый поиск по имени"},
    {L"Esc", L"сброс быстрого поиска и фильтра"},
};

const HelpRow kTabs[] = {
    {L"Ctrl+T / Ctrl+W", L"новая вкладка / закрыть"},
    {L"Ctrl+Tab / Ctrl+Shift+Tab", L"следующая / предыдущая вкладка"},
    {L"Ctrl+Shift+A", L"список вкладок"},
    {L"Ctrl+Shift+W", L"закрыть все вкладки"},
};

const HelpRow kView[] = {
    {L"Ctrl+F3…F6", L"сортировка: имя / тип / дата / размер"},
    {L"Ctrl+F7", L"без сортировки"},
    {L"Ctrl+F1 / Ctrl+F2", L"краткий вид / подробный вид"},
    {L"Ctrl+F8", L"дерево каталогов"},
    {L"Ctrl+H", L"скрытые файлы"},
    {L"Ctrl+B", L"ветка: плоский список поддерева (повторно — выйти)"},
};

const HelpRow kFind[] = {
    {L"Alt+F7", L"поиск файлов (маска, текст, архивы)"},
    {L"Ctrl+S", L"быстрый фильтр по подстроке (Enter — оставить)"},
    {L"Ctrl+Shift+S", L"сбросить фильтр"},
    {L"Ctrl+Q", L"быстрый просмотр на другой панели"},
};

const HelpRow kCommand[] = {
    {L"Ctrl+Enter / Ctrl+Shift+Enter", L"имя файла / полный путь в командную строку"},
    {L"Ctrl+J / Ctrl+Shift+J", L"то же (алиас)"},
    {L"Ctrl+E / Ctrl+↓", L"предыдущая команда / в строку с последней"},
    {L"Alt+F8", L"выпадающий список истории команд"},
    {L"↑ / ↓ в строке", L"история команд"},
    {L"Enter в строке", L"выполнить команду"},
    {L"Esc в строке", L"очистить и вернуться в панель"},
};

const HelpRow kOther[] = {
    {L"Ctrl+D", L"избранное: меню, «Добавить текущий каталог» внутри"},
    {L"Ctrl+M / Ctrl+Z", L"пакетное переименование / отмена переименований"},
    {L"Ctrl+Shift+C", L"копировать путь"},
    {L"F9 / F10", L"меню"},
    {L"F1", L"этот справочник"},
    {L"Alt+F4", L"выход"},
};

const HelpRow kFtp[] = {
    {L"Ctrl+F", L"FTP: подключиться"},
    {L"Ctrl+N", L"FTP: новое соединение (можно вставить ftp://…)"},
    {L"Ctrl+Shift+F", L"FTP: отключиться"},
};

const HelpRow kLister[] = {
    {L"Esc / F10", L"закрыть просмотр"},
    {L"Ctrl+F / F7", L"поиск по тексту"},
    {L"F3 / Shift+F3", L"следующее / предыдущее вхождение"},
    {L"1 / 2 / 3 / 4", L"кодировки: ANSI / OEM / UTF-8 / UTF-16"},
    {L"H / T", L"hex-дамп / текст"},
    {L"M / C / G", L"Markdown / подсветка / оглавление"},
    {L"F2", L"перечитать файл"},
    {L"N / P, Alt+← / Alt+→", L"следующий / предыдущий файл панели"},
    {L"W", L"перенос строк"},
    {L"Num + / Num -", L"масштаб шрифта"},
    {L"Alt+Enter", L"на весь экран"},
};

const HelpSection kSections[] = {
    {L"Файлы и операции", kFiles, ARRAYSIZE(kFiles)},
    {L"Отметка файлов", kMark, ARRAYSIZE(kMark)},
    {L"Навигация", kNav, ARRAYSIZE(kNav)},
    {L"Вкладки", kTabs, ARRAYSIZE(kTabs)},
    {L"Вид и сортировка", kView, ARRAYSIZE(kView)},
    {L"Поиск и фильтр", kFind, ARRAYSIZE(kFind)},
    {L"Командная строка", kCommand, ARRAYSIZE(kCommand)},
    {L"Избранное и прочее", kOther, ARRAYSIZE(kOther)},
    {L"FTP", kFtp, ARRAYSIZE(kFtp)},
    {L"Просмотрщик (F3)", kLister, ARRAYSIZE(kLister)},
};

const wchar_t* kHelpClass = L"POSHelp2";
bool g_helpOpen = false;   // окно справочника открыто (одно на приложение)

const int kFilterId = 10;
const int kListId = 11;
const int kHintId = 12;

struct HelpState {
    HWND hwnd = nullptr;
    HWND filter = nullptr;
    HWND list = nullptr;
    HWND hint = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
};

// Esc/F1 закрывают окно даже когда фокус в поле поиска или в списке.
LRESULT CALLBACK HelpKeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE || wParam == VK_F1) {
            PostMessageW(GetParent(hwnd), WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_DOWN && id == 1) {          // из поля поиска — в список
            HWND list = GetDlgItem(GetParent(hwnd), kListId);
            if (list) SetFocus(list);
            return 0;
        }
        if (wParam == VK_UP && id == 2) {            // из списка — обратно в поиск
            HWND filter = GetDlgItem(GetParent(hwnd), kFilterId);
            if (filter) SetFocus(filter);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
void AddRow(HelpState* st, const wchar_t* keys, const wchar_t* action, bool section) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(st->list);
    item.pszText = const_cast<wchar_t*>(keys);
    const int index = ListView_InsertItem(st->list, &item);
    if (action && action[0]) ListView_SetItemText(st->list, index, 1, const_cast<wchar_t*>(action));
}

// Перезаполнение списка с учётом строки поиска (по клавишам и по описанию).
void FillHelpList(HelpState* st) {
    ListView_DeleteAllItems(st->list);
    wchar_t buf[128]{};
    GetWindowTextW(st->filter, buf, 128);
    const std::wstring needle = Lower(Trim(buf));

    for (const auto& section : kSections) {
        bool headerShown = false;
        for (int i = 0; i < section.count; ++i) {
            const HelpRow& row = section.rows[i];
            if (!needle.empty()) {
                const std::wstring hay = Lower(std::wstring(row.keys) + L" " + row.action);
                if (hay.find(needle) == std::wstring::npos) continue;
            }
            if (!headerShown) {
                AddRow(st, section.title, L"", true);
                headerShown = true;
            }
            AddRow(st, row.keys, row.action, false);
        }
    }
}

void LayoutHelp(HelpState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int pad = 10;
    const int filterH = 24;
    const int hintH = 18;
    MoveWindow(st->filter, pad, pad, rc.right - pad * 2, filterH, TRUE);
    MoveWindow(st->hint, pad, pad + filterH + 4, rc.right - pad * 2, hintH, TRUE);
    const int listY = pad + filterH + 4 + hintH + 6;
    MoveWindow(st->list, pad, listY, rc.right - pad * 2, rc.bottom - listY - pad, TRUE);
}

LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<HelpState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<HelpState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->hwnd = hwnd;

            st->font = CreateFontW(-Scale(14, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            st->fontBold = CreateFontW(-Scale(14, WindowDpi(hwnd)), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            st->filter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 10, 10, hwnd,
                                         (HMENU)(INT_PTR)kFilterId, nullptr, nullptr);
            st->hint = CreateWindowExW(0, L"STATIC", L"Начните печатать — список сузится (по клавишам и описанию)",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kHintId,
                                       nullptr, nullptr);
            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                           LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(L"Клавиши");
            col.cx = Scale(240, WindowDpi(hwnd));
            ListView_InsertColumn(st->list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"Действие");
            col.cx = Scale(430, WindowDpi(hwnd));
            ListView_InsertColumn(st->list, 1, &col);

            SetWindowSubclass(st->filter, HelpKeyProc, 1, 0);
            SetWindowSubclass(st->list, HelpKeyProc, 2, 0);

            const HWND kids[] = {st->filter, st->hint, st->list};
            for (HWND k : kids) SendMessageW(k, WM_SETFONT, (WPARAM)st->font, TRUE);

            FillHelpList(st);
            LayoutHelp(st);
            SetFocus(st->filter);
            return 0;
        }
        case WM_SIZE:
            if (st) LayoutHelp(st);
            return 0;
        case WM_COMMAND:
            if (st && LOWORD(wParam) == kFilterId && HIWORD(wParam) == EN_CHANGE) {
                FillHelpList(st);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_F1) {
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
                if (st->fontBold) DeleteObject(st->fontBold);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void ShowHotkeysHelp(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HelpProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kHelpClass;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);
        registered = true;
    }

    // Немодальное окно: одно на приложение. Повторный F1 показывет уже открытое.
    if (g_helpOpen) {
        HWND existing = FindWindowW(kHelpClass, nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
            SetFocus(GetDlgItem(existing, kFilterId));
            return;
        }
        g_helpOpen = false;
    }

    RECT ownerRect{0, 0, 900, 640};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);
    const int width = 720;
    const int height = 560;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    auto* state = new HelpState();
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kHelpClass, L"Горячие клавиши — PosFiles",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, width, height, owner, nullptr,
                                GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        delete state;
        return;
    }
    g_helpOpen = true;
    SetForegroundWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, kFilterId));
}
