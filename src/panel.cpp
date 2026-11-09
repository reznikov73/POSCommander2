// Реализация панели.
#include "panel.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include "archive.h"
#include "ftp.h"
#include "settings.h"
#include "dialogs.h"

#include <algorithm>
#include <memory>
#include <thread>

namespace {
const wchar_t* kPanelClass = L"POSPanel2";
const int kPathEditId = 1;     // поле пути
const int kPathApplyId = 3;    // ✓ перейти
const int kPathFavId = 4;      // ★ избранное
const int kPathHistId = 5;     // ▼ история
const int kMaskEditId = 6;     // поле маски файлов
const int kMaskHistId = 7;     // ▼ история масок
const int kStatusInfoId = 8;   // сводка «Отмечено: …» справа статуса
const UINT_PTR kPathClickTimer = 7;   // отложенный переход по клику на компонент пути
const UINT_PTR kHoverShowTimer = 8;   // показ списка содержимого каталога
const UINT_PTR kHoverHideTimer = 9;   // скрытие списка
const UINT_PTR kHoverCascadeTimer = 11;  // открытие подуровня при наведении на папку
const wchar_t* kHoverClass = L"POSPathHover2";
const int kListId = 2;
const int kStatusId = 3;
const int kDriveBase = 100;   // 100..125 — кнопки дисков
const int kDriveRootId = 130; // «\» — в корень диска/архива
const int kDriveUpId = 131;   // «..» — на уровень вверх
const int kTvId = 5;          // дерево каталогов
const int kTabBase = 200;     // 200..231 — вкладки панели
const int kVolumeId = 4;      // строка "свободно из" под дисками

const COLORREF kMarkedText = RGB(200, 0, 0);
const COLORREF kHiddenText = RGB(128, 128, 128);
const COLORREF kActiveHeaderBg = RGB(204, 232, 255);
const COLORREF kInactiveHeaderBg = RGB(240, 240, 240);

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool CmpEntry(const Entry& a, const Entry& b, int column, bool reverse) {
    // ".." всегда сверху, каталоги всегда раньше файлов.
    if (a.isUp != b.isUp) return a.isUp;
    if (a.isDirFs() != b.isDirFs()) return a.isDirFs();

    int r = 0;
    switch (column) {
        case 1:
            r = CompareNatural(Lower(a.ext()), Lower(b.ext()));
            break;
        case 2:
            r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
            break;
        case 3:
            r = CompareFileTime(&a.ft, &b.ft);
            break;
        default:
            r = CompareNatural(a.name, b.name);
            break;
    }
    if (r == 0) r = CompareNatural(a.name, b.name);
    return reverse ? r > 0 : r < 0;
}
std::wstring LeafOf(const std::wstring& path) {
    if (path.size() <= 3) return path;                 // корень диска: "E:\"
    size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos + 1 >= path.size()) return path;
    return path.substr(pos + 1);
}

// Сокращение пути слева, как в TC («…\src\main.cpp»): пока текст не влезает,
// отрезаем ведущие компоненты и ставим многоточие. Последний компонент не режем.
std::wstring CompactPathLeft(const std::wstring& text, int maxWidth, HFONT font) {
    if (text.empty() || maxWidth <= 0) return text;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SIZE sz{};
    std::wstring t = text;
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    while (sz.cx > maxWidth) {
        const size_t lead = t.find_first_not_of(L'\\');      // UNC: ведущие '\' пропускаем
        if (lead == std::wstring::npos) break;
        const size_t cut = t.find(L'\\', lead);
        if (cut == std::wstring::npos) break;                // остался последний компонент
        t = L"…" + t.substr(cut + 1);
        if (!GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz)) break;
        if (t.size() <= 2) break;                             // «…x» — короче некуда
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return t;
}

}  // namespace

bool Panel::Create(HWND parent, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProcStatic;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPanelClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    hwnd_ = CreateWindowExW(0, kPanelClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100,
                            parent, (HMENU)(INT_PTR)id, g_inst, this);
    if (!hwnd_) return false;

    dpi_ = WindowDpi(hwnd_);
    brActive_ = CreateSolidBrush(kActiveHeaderBg);
    brInactive_ = CreateSolidBrush(kInactiveHeaderBg);

    pathEdit_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                                0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kPathEditId, g_inst, nullptr);
    if (pathEdit_) SetWindowSubclass(pathEdit_, PathEditProc, 1, (DWORD_PTR)this);

    const DWORD pathBtn = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
    pathApply_ = CreateWindowExW(0, L"BUTTON", L"✓", pathBtn, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kPathApplyId, g_inst, nullptr);
    pathFav_ = CreateWindowExW(0, L"BUTTON", L"★", pathBtn, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kPathFavId, g_inst, nullptr);
    pathHist_ = CreateWindowExW(0, L"BUTTON", L"▼", pathBtn, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kPathHistId, g_inst, nullptr);

    // Поле маски файлов «*.*» — справа от поля пути, с историей масок (этап 5 план-UI).
    maskEdit_ = CreateWindowExW(0, L"EDIT", L"*.*",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                                0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kMaskEditId, g_inst, nullptr);
    if (maskEdit_) SetWindowSubclass(maskEdit_, MaskEditProc, 2, (DWORD_PTR)this);
    maskHist_ = CreateWindowExW(0, L"BUTTON", L"", pathBtn, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kMaskHistId, g_inst, nullptr);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS,   // без LVS_NOSORTHEADER: клик по заголовку сортирует
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kListId, g_inst, nullptr);

    SetWindowSubclass(list_, ListProc, 3, (DWORD_PTR)this);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetImageList(list_, ShellSmallImageList(), LVSIL_SMALL);

    const Settings& st = GetSettings();
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    col.cx = st.colWidth[0];
    col.iSubItem = 0;
    col.pszText = (LPWSTR)L"Имя";
    ListView_InsertColumn(list_, 0, &col);
    col.cx = st.colWidth[1]; col.iSubItem = 1; col.pszText = (LPWSTR)L"Тип";
    ListView_InsertColumn(list_, 1, &col);
    col.cx = st.colWidth[2]; col.iSubItem = 2; col.pszText = (LPWSTR)L"Размер";
    col.fmt = LVCFMT_RIGHT;                       // размер по правому краю, как в TC
    ListView_InsertColumn(list_, 2, &col);
    col.cx = st.colWidth[3]; col.iSubItem = 3; col.pszText = (LPWSTR)L"Дата";
    col.fmt = LVCFMT_LEFT;
    ListView_InsertColumn(list_, 3, &col);

    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kStatusId, g_inst, nullptr);

    volume_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_RIGHT,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kVolumeId, g_inst, nullptr);

    // Справа в статусной строке — сводка «Отмечено: …»; слева путь под курсором (этап 5).
    statusInfo_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_RIGHT,
                                  0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kStatusInfoId, g_inst, nullptr);

    CreateDriveBar();

    // «\» и «..» в ряду дисков: корень и уровень вверх, как в TC.
    const DWORD navBtn = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
    rootBtn_ = CreateWindowExW(0, L"BUTTON", L"\\", navBtn, 0, 0, 10, 10, hwnd_,
                               (HMENU)(INT_PTR)kDriveRootId, g_inst, nullptr);
    upBtn_ = CreateWindowExW(0, L"BUTTON", L"..", navBtn, 0, 0, 10, 10, hwnd_,
                             (HMENU)(INT_PTR)kDriveUpId, g_inst, nullptr);

    // Подсказка с полным путём: при наведении на запись списка и на поле пути (как в TC).
    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, g_inst,
                           nullptr);
    if (tip_) {
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, Scale(900, dpi_));

        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.lpszText = LPSTR_TEXTCALLBACKW;

        ti.uId = (UINT_PTR)list_;          // только список: над полем пути показываем список каталога
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    return true;
}

// Ряд кнопок дисков над заголовком панели, как в Total Commander.
void Panel::CreateDriveBar() {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = (wchar_t)(L'A' + i);
        wchar_t root[4] = {letter, L':', L'\\', 0};

        // Значок тома: у сетевых, CD и съёмных он свой — оболочка отдаёт нужный.
        SHFILEINFOW sfi{};
        HICON icon = nullptr;
        if (SHGetFileInfoW(root, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) icon = sfi.hIcon;

        wchar_t text[4] = {letter, L':', 0, 0};
        HWND btn = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(kDriveBase + i), g_inst, nullptr);
        if (btn) {
            if (font_) SendMessageW(btn, WM_SETFONT, (WPARAM)font_, TRUE);
            driveButtons_.push_back(btn);
            driveLetters_.push_back(letter);
            driveIds_.push_back(kDriveBase + i);
            driveIcons_.push_back(icon);
        } else if (icon) {
            DestroyIcon(icon);
        }
    }
    UpdateDriveBar();
}

// Пересобрать ряд дисков (например, после подключения сетевого диска).
void Panel::RebuildDriveBar() {
    for (auto h : driveButtons_) DestroyWindow(h);
    driveButtons_.clear();
    driveLetters_.clear();
    driveIds_.clear();
    for (auto icon : driveIcons_) {
        if (icon) DestroyIcon(icon);
    }
    driveIcons_.clear();
    CreateDriveBar();
    Layout();
}

// Текущий диск подсвечивается при отрисовке.
void Panel::UpdateDriveBar() {
    for (auto h : driveButtons_) InvalidateRect(h, nullptr, TRUE);
}

// Кнопка диска: значок тома + буква; активный — в стиле hover-кнопок тулбара
// (этап 1): заливка #CCE8FF, рамка #8CC3EA (макет .db.on).
void Panel::DrawDriveButton(DRAWITEMSTRUCT* dis) {
    // ID кнопки — kDriveBase + буква (0..25), поэтому ищем позицию в списке, а не берём как индекс.
    int index = -1;
    for (size_t i = 0; i < driveIds_.size(); ++i) {
        if (driveIds_[i] == (int)dis->CtlID) {
            index = (int)i;
            break;
        }
    }
    if (index < 0) return;

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;

    const wchar_t current = path_.empty() ? 0 : path_[0];
    const bool isCurrent = !ftp_ && towupper(current) == towupper(driveLetters_[index]);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH fill = CreateSolidBrush(isCurrent ? RGB(204, 232, 255)
                                             : (pressed ? RGB(178, 215, 247) : GetSysColor(COLOR_BTNFACE)));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);

    if (isCurrent || pressed) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 195, 234));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    // Значок тома: если оболочка в момент создания панели не отдала его (бывает
    // при старте без рабочего стола), пробуем ещё раз здесь и запоминаем.
    if (index < (int)driveIcons_.size() && !driveIcons_[index]) {
        wchar_t rootPath[4] = {driveLetters_[index], L':', L'\\', 0};
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(rootPath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
            driveIcons_[index] = sfi.hIcon;
        }
    }

    const int iconLeft = rc.left + Scale(3, dpi_);
    const int iconTop = (rc.top + rc.bottom) / 2 - Scale(8, dpi_);
    int textLeft = iconLeft;
    if (index < (int)driveIcons_.size() && driveIcons_[index]) {
        DrawIconEx(dc, iconLeft, iconTop, driveIcons_[index], 16, 16, 0, nullptr, DI_NORMAL);
        textLeft = rc.left + Scale(22, dpi_);
    } else {
        // Совсем без значка не оставляем: рисуем простой силуэт диска.
        const int w = Scale(14, dpi_), h = Scale(11, dpi_);
        HBRUSH body = CreateSolidBrush(RGB(200, 205, 212));
        HPEN edge = CreatePen(PS_SOLID, 1, RGB(90, 96, 106));
        HGDIOBJ oldBrush = SelectObject(dc, body);
        HGDIOBJ oldPen = SelectObject(dc, edge);
        Rectangle(dc, iconLeft, iconTop + Scale(3, dpi_), iconLeft + w, iconTop + Scale(3, dpi_) + h);
        SelectObject(dc, GetStockObject(WHITE_BRUSH));
        Rectangle(dc, iconLeft + Scale(3, dpi_), iconTop + Scale(6, dpi_), iconLeft + w - Scale(3, dpi_),
                  iconTop + Scale(9, dpi_));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(edge);
        DeleteObject(body);
        textLeft = rc.left + Scale(22, dpi_);
    }

    wchar_t text[4] = {driveLetters_[index], L':', 0, 0};
    RECT tr = rc;
    tr.left = textLeft;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// Кнопки «\» и «..» в ряду дисков: тот же стиль, текст по центру без значка.
void Panel::DrawNavButton(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH fill = CreateSolidBrush(pressed ? RGB(178, 215, 247) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    if (pressed) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 195, 234));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    const wchar_t* text = dis->CtlID == kDriveRootId ? L"\\" : L"..";
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// ---------- вкладки ----------

void Panel::RebuildTabBar() {
    for (auto h : tabButtons_) DestroyWindow(h);   // старые кнопки вкладок убираем
    tabButtons_.clear();
    tabNames_.resize(tabs_.size());
    tabLocked_.resize(tabs_.size(), false);

    for (size_t i = 0; i < tabs_.size(); ++i) {
        HWND btn = CreateWindowExW(0, L"BUTTON", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(kTabBase + (int)i), g_inst, nullptr);
        if (btn) {
            if (font_) SendMessageW(btn, WM_SETFONT, (WPARAM)font_, TRUE);
            SetWindowSubclass(btn, TabProc, 2, (DWORD_PTR)this);
            tabButtons_.push_back(btn);
        }
    }
    Layout();
}

std::wstring Panel::TabTitle(int index) const {
    if (index < 0 || index >= (int)tabs_.size()) return L"";
    if (index < (int)tabNames_.size() && !tabNames_[index].empty()) return tabNames_[index];
    if (IsFtpUrl(tabs_[index])) return FtpLeafName(tabs_[index]);
    return LeafOf(tabs_[index]);
}

void Panel::SetTabMeta(const std::vector<std::wstring>& names, const std::vector<int>& locks) {
    tabNames_.assign(tabs_.size(), L"");
    tabLocked_.assign(tabs_.size(), false);
    for (size_t i = 0; i < names.size() && i < tabNames_.size(); ++i) tabNames_[i] = names[i];
    for (size_t i = 0; i < locks.size() && i < tabLocked_.size(); ++i) tabLocked_[i] = locks[i] != 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::UpdateTabText() {
    if (tab_ < 0 || tab_ >= (int)tabButtons_.size() || tab_ >= (int)tabs_.size()) return;
    InvalidateRect(tabButtons_[tab_], nullptr, TRUE);
}

int Panel::TabIndexFromHwnd(HWND wnd) const {
    for (size_t i = 0; i < tabButtons_.size(); ++i) {
        if (tabButtons_[i] == wnd) return (int)i;
    }
    return -1;
}

// Область крестика «×» в правой части вкладки.
RECT Panel::TabCloseRect(int index) const {
    RECT rc{};
    if (index < 0 || index >= (int)tabButtons_.size()) return rc;
    GetClientRect(tabButtons_[index], &rc);
    const int w = Scale(16, dpi_);
    rc.left = rc.right - w;
    return rc;
}

void Panel::CloseTabAt(int index) {
    if (index < 0 || index >= (int)tabs_.size() || tabs_.size() <= 1) return;
    tabs_.erase(tabs_.begin() + index);
    if (index < (int)tabNames_.size()) tabNames_.erase(tabNames_.begin() + index);
    if (index < (int)tabLocked_.size()) tabLocked_.erase(tabLocked_.begin() + index);
    if (tab_ > index) --tab_;
    if (tab_ >= (int)tabs_.size()) tab_ = (int)tabs_.size() - 1;
    if (index == tab_ || tabs_.size() == 1) Navigate(tabs_[tab_]);
    RebuildTabBar();
}

void Panel::CloseAllTabs() {
    if (tabs_.empty()) return;
    const std::wstring keep = tabs_[tab_];
    tabs_.assign(1, keep);
    tabNames_.assign(1, L"");
    tabLocked_.assign(1, false);
    tab_ = 0;
    RebuildTabBar();
}

void Panel::AddTabFor(const std::wstring& dir, const std::wstring& name) {
    tabs_.push_back(dir);
    tabNames_.push_back(name);
    tabLocked_.push_back(false);
    tab_ = (int)tabs_.size() - 1;
    RebuildTabBar();
    Navigate(dir);
}

void Panel::DuplicateTab(int index) {
    if (index < 0 || index >= (int)tabs_.size()) return;
    const std::wstring dir = tabs_[index];
    const std::wstring name = index < (int)tabNames_.size() ? tabNames_[index] : L"";
    tabs_.push_back(dir);
    tabNames_.push_back(name);
    tabLocked_.push_back(false);
    tab_ = (int)tabs_.size() - 1;
    RebuildTabBar();
    Navigate(dir);
}

void Panel::ToggleTabLock(int index) {
    if (index < 0 || index >= (int)tabs_.size()) return;
    if (index >= (int)tabLocked_.size()) tabLocked_.resize(tabs_.size(), false);
    tabLocked_[index] = !tabLocked_[index];
    if (tabLocked_[index]) tabs_[index] = path_;   // запоминаем каталог на момент блокировки
    RebuildTabBar();
    LogDebug(L"tab " + std::to_wstring(index) + L" lock=" + (tabLocked_[index] ? L"1" : L"0"));
}

// Меню вкладки: как в TC (копия, блокировка, переименование, закрытие).
void Panel::ShowTabMenu(int index, POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Открыть копию этой вкладки");
    AppendMenuW(menu, MF_STRING, 5, L"Открыть копию на другой панели");
    AppendMenuW(menu, MF_STRING, 3, L"Переименовать вкладку…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (TabLocked(index) ? MF_CHECKED : 0), 2,
                TabLocked(index) ? L"Разблокировать вкладку" : L"Заблокировать вкладку");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (tabs_.size() <= 1 ? MF_GRAYED : 0), 6, L"Закрыть вкладку");
    AppendMenuW(menu, MF_STRING | (tabs_.size() <= 1 ? MF_GRAYED : 0), 7, L"Закрыть все вкладки");

    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case 1:
            DuplicateTab(index);
            break;
        case 2:
            ToggleTabLock(index);
            break;
        case 3: {
            std::wstring name = TabTitle(index);
            if (InputBox(GetParent(hwnd_), L"Вкладка", L"Название вкладки:", name, false)) {
                if (index >= (int)tabNames_.size()) tabNames_.resize(tabs_.size());
                tabNames_[index] = Trim(name);
                RebuildTabBar();
            }
            break;
        }
        case 5:
            PostMessageW(GetParent(hwnd_), WM_APP + 302, (WPARAM)side_, (LPARAM)index);
            break;
        case 6:
            CloseTabAt(index);
            break;
        case 7:
            CloseAllTabs();
            break;
        default:
            break;
    }
}

LRESULT CALLBACK Panel::TabProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    if (!self) return DefSubclassProc(hwnd, msg, wParam, lParam);
    const int index = self->TabIndexFromHwnd(hwnd);

    switch (msg) {
        case WM_MBUTTONUP:                       // средняя кнопка закрывает вкладку
            if (index >= 0) self->CloseTabAt(index);
            return 0;

        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &pt);
            if (index >= 0) self->ShowTabMenu(index, pt);
            return 0;
        }

        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lParam);
            RECT close = self->TabCloseRect(index);
            if (self->tabs_.size() > 1 && x >= close.left) {   // клик по «×»
                self->CloseTabAt(index);
                return 0;
            }
            break;
        }

        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Рисование вкладки: активная — светлая с рамкой, неактивная — серая.
void Panel::DrawTabButton(DRAWITEMSTRUCT* dis) {
    const int index = (int)dis->CtlID - kTabBase;
    if (index < 0 || index >= (int)tabs_.size()) return;

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool activeTab = (index == tab_);
    const bool locked = TabLocked(index);

    HBRUSH fill = CreateSolidBrush(activeTab ? RGB(255, 255, 255) : RGB(226, 223, 218));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);

    HPEN pen = CreatePen(PS_SOLID, 1, activeTab ? RGB(90, 90, 90) : RGB(166, 162, 156));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
    LineTo(dc, rc.left, rc.top + 1);
    LineTo(dc, rc.right - 1, rc.top + 1);
    LineTo(dc, rc.right - 1, rc.bottom - 1);
    if (activeTab) {
        MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
        LineTo(dc, rc.right - 1, rc.bottom - 1);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, activeTab ? RGB(0, 0, 0) : RGB(70, 70, 70));
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));

    int textLeft = rc.left + Scale(6, dpi_);

    // Замок у заблокированной вкладки.
    if (locked) {
        const int lockW = Scale(9, dpi_);
        const int top = rc.top + Scale(8, dpi_);
        HBRUSH body = CreateSolidBrush(RGB(120, 90, 20));
        RECT bodyRc{textLeft, top + Scale(4, dpi_), textLeft + lockW, top + Scale(9, dpi_)};
        FillRect(dc, &bodyRc, body);
        DeleteObject(body);
        HPEN arcPen = CreatePen(PS_SOLID, 1, RGB(120, 90, 20));
        HGDIOBJ op = SelectObject(dc, arcPen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Arc(dc, textLeft + Scale(1, dpi_), top, textLeft + lockW - Scale(1, dpi_), top + Scale(7, dpi_),
            textLeft + lockW - Scale(1, dpi_), top + Scale(3, dpi_), textLeft + Scale(1, dpi_), top + Scale(3, dpi_));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(arcPen);
        textLeft += lockW + Scale(4, dpi_);
    }

    RECT textRc = rc;
    textRc.left = textLeft;
    textRc.right = rc.right - Scale(18, dpi_);
    DrawTextW(dc, TabTitle(index).c_str(), -1, &textRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Крестик закрытия.
    if (tabs_.size() > 1) {
        const int pad = Scale(5, dpi_);
        const int cx = rc.right - Scale(11, dpi_);
        const int cy = (rc.top + rc.bottom) / 2;
        HPEN xPen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
        HGDIOBJ op = SelectObject(dc, xPen);
        MoveToEx(dc, cx - pad, cy - pad, nullptr);
        LineTo(dc, cx + pad, cy + pad);
        MoveToEx(dc, cx + pad, cy - pad, nullptr);
        LineTo(dc, cx - pad, cy + pad);
        SelectObject(dc, op);
        DeleteObject(xPen);
    }

    SelectObject(dc, oldFont);
}

void Panel::RestoreState(const std::vector<std::wstring>& tabs, int active) {
    if (tabs.empty()) return;
    tabs_ = tabs;
    tab_ = (active >= 0 && active < (int)tabs_.size()) ? active : 0;
    if (!Navigate(tabs_[tab_])) {
        // сохранённый каталог мог исчезнуть: пробуем первый существующий
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (Navigate(tabs_[i])) {
                tab_ = (int)i;
                break;
            }
        }
    }
    RebuildTabBar();
}

void Panel::CaptureState(std::vector<std::wstring>& tabs, int& active) const {
    tabs = tabs_;
    active = tab_;
    if (tabs.empty()) {
        tabs.push_back(path_);
        active = 0;
    } else if (active >= 0 && active < (int)tabs.size() && !TabLocked(active)) {
        tabs[active] = path_;
    }
}

void Panel::AddTab() {
    if (tabs_.empty()) {
        tabs_.push_back(path_);
        tab_ = 0;
    } else {
        tabs_.push_back(path_);
        tab_ = (int)tabs_.size() - 1;
    }
    RebuildTabBar();
}

void Panel::CloseTab() {
    if (tabs_.size() <= 1) return;
    tabs_.erase(tabs_.begin() + tab_);
    if (tab_ >= (int)tabs_.size()) tab_ = (int)tabs_.size() - 1;
    RebuildTabBar();
    Navigate(tabs_[tab_]);
}

void Panel::ActivateTab(int index) {
    if (index < 0 || index >= (int)tabs_.size()) return;
    tab_ = index;
    Navigate(tabs_[tab_]);
}

void Panel::NextTab() {
    if (tabs_.size() < 2) return;
    ActivateTab((tab_ + 1) % (int)tabs_.size());
}

void Panel::SetFont(HFONT font, UINT dpi) {
    font_ = font;
    dpi_ = dpi;
    if (pathEdit_) SendMessageW(pathEdit_, WM_SETFONT, (WPARAM)font, TRUE);
    if (pathApply_) SendMessageW(pathApply_, WM_SETFONT, (WPARAM)font, TRUE);
    if (pathFav_) SendMessageW(pathFav_, WM_SETFONT, (WPARAM)font, TRUE);
    if (pathHist_) SendMessageW(pathHist_, WM_SETFONT, (WPARAM)font, TRUE);
    if (maskEdit_) SendMessageW(maskEdit_, WM_SETFONT, (WPARAM)font, TRUE);
    if (maskHist_) SendMessageW(maskHist_, WM_SETFONT, (WPARAM)font, TRUE);
    if (list_) SendMessageW(list_, WM_SETFONT, (WPARAM)font, TRUE);
    if (status_) SendMessageW(status_, WM_SETFONT, (WPARAM)font, TRUE);
    if (statusInfo_) SendMessageW(statusInfo_, WM_SETFONT, (WPARAM)font, TRUE);
    if (volume_) SendMessageW(volume_, WM_SETFONT, (WPARAM)font, TRUE);
    if (rootBtn_) SendMessageW(rootBtn_, WM_SETFONT, (WPARAM)font, TRUE);
    if (upBtn_) SendMessageW(upBtn_, WM_SETFONT, (WPARAM)font, TRUE);
    for (auto h : driveButtons_) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    for (auto h : tabButtons_) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    Layout();
}

void Panel::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    int driveH = Scale(24, dpi_);      // ряд дисков + свободное место, как в TC — одной строкой
    int tabH = Scale(24, dpi_);
    int headerH = Scale(24, dpi_);
    int statusH = Scale(22, dpi_);
    int listH = height - driveH - tabH - headerH - statusH;
    if (listH < 0) listH = 0;

    HDWP dwp = BeginDeferWindowPos(8 + 2 + (int)driveButtons_.size() + (int)tabButtons_.size());
    int drivesRight = 0;
    if (dwp) {
        // Ширина кнопки диска — по фактическому тексту «C:» (макет: поля 7px, значок 16, зазор 4).
        HDC dc = GetDC(hwnd_);
        HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        SIZE sz{};
        GetTextExtentPoint32W(dc, L"C:", 2, &sz);
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);

        int x = Scale(4, dpi_);
        const int bw = sz.cx + Scale(34, dpi_);
        for (auto h : driveButtons_) {
            dwp = DeferWindowPos(dwp, h, nullptr, x, 1, bw, driveH - 2, SWP_NOZORDER);
            x += bw + Scale(2, dpi_);
        }
        // «\» — корень, «..» — уровень вверх; переехали из заголовка, как в TC.
        const int nbw = Scale(28, dpi_);
        if (rootBtn_) {
            dwp = DeferWindowPos(dwp, rootBtn_, nullptr, x, 1, nbw, driveH - 2, SWP_NOZORDER);
            x += nbw + Scale(2, dpi_);
        }
        if (upBtn_) {
            dwp = DeferWindowPos(dwp, upBtn_, nullptr, x, 1, nbw, driveH - 2, SWP_NOZORDER);
            x += nbw + Scale(2, dpi_);
        }
        drivesRight = x;
    }
    // «[рабочий] 1 253,4 ГБ из 3 726,0 ГБ свободно» — справа от кнопок, у правого края.
    if (dwp)
        dwp = DeferWindowPos(dwp, volume_, nullptr, drivesRight, 1, width - drivesRight, driveH - 2, SWP_NOZORDER);
    if (dwp) {
        HDC dc = GetDC(hwnd_);
        HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        int x = 0;
        for (size_t i = 0; i < tabButtons_.size(); ++i) {
            int tw = Scale(90, dpi_);
            if (i < tabs_.size()) {
                const std::wstring title = TabTitle((int)i);
                HFONT btnFont = (HFONT)SendMessageW(tabButtons_[i], WM_GETFONT, 0, 0);
                if (btnFont) SelectObject(dc, btnFont);
                SIZE sz{};
                GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &sz);
                if (btnFont) SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
                tw = sz.cx + Scale(40, dpi_);                  // поля и крестик
                if (TabLocked((int)i)) tw += Scale(13, dpi_);
                if (tw < Scale(70, dpi_)) tw = Scale(70, dpi_);
                if (tw > Scale(230, dpi_)) tw = Scale(230, dpi_);
            }
            dwp = DeferWindowPos(dwp, tabButtons_[i], nullptr, x, driveH + 1, tw, tabH - 2, SWP_NOZORDER);
            x += tw + Scale(2, dpi_);
        }
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);
    }
    int y = driveH + tabH;
    int listX = 0;
    int listW = width;
    if (treeVisible_ && tree_) {
        const int treeW = Scale(210, dpi_);
        if (treeW < width - Scale(150, dpi_)) {
            if (dwp) dwp = DeferWindowPos(dwp, tree_, nullptr, 0, y + headerH, treeW, listH, SWP_NOZORDER);
            listX = treeW + Scale(4, dpi_);
            listW = width - listX;
        }
    }

    const int pbtn = Scale(22, dpi_);
    const int pgap = Scale(2, dpi_);
    const int btnCount = pathEditing_ ? 3 : 2;      // ✓ видна только при правке
    const int maskW = Scale(58, dpi_);              // поле маски — как в макете (.pmask 58px)
    const int editW = width - maskW - (pbtn + pgap) * (btnCount + 1);   // +1 — ▼ истории масок
    if (pathApply_) ShowWindow(pathApply_, pathEditing_ ? SW_SHOW : SW_HIDE);

    int bx = editW + maskW + pbtn + pgap;
    if (dwp) dwp = DeferWindowPos(dwp, pathEdit_, nullptr, 0, y + 1, editW, headerH - 2, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, maskEdit_, nullptr, editW, y + 1, maskW, headerH - 2, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, maskHist_, nullptr, editW + maskW, y + 1, pbtn, headerH - 2, SWP_NOZORDER);
    if (pathEditing_ && dwp) {
        dwp = DeferWindowPos(dwp, pathApply_, nullptr, bx, y + 1, pbtn, headerH - 2, SWP_NOZORDER);
        bx += pbtn + pgap;
    }
    if (dwp) dwp = DeferWindowPos(dwp, pathFav_, nullptr, bx, y + 1, pbtn, headerH - 2, SWP_NOZORDER);
    bx += pbtn + pgap;
    if (dwp) dwp = DeferWindowPos(dwp, pathHist_, nullptr, bx, y + 1, pbtn, headerH - 2, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, list_, nullptr, listX, y + headerH, listW, listH, SWP_NOZORDER);
    if (dwp) {
        // Статус: слева — путь под курсором, справа — сводка «Отмечено: …».
        const int infoW = std::min(Scale(260, dpi_), width / 2);
        dwp = DeferWindowPos(dwp, status_, nullptr, 0, y + headerH + listH, width - infoW, statusH, SWP_NOZORDER);
        dwp = DeferWindowPos(dwp, statusInfo_, nullptr, width - infoW, y + headerH + listH, infoW, statusH, SWP_NOZORDER);
    }
    if (dwp) EndDeferWindowPos(dwp);

    UpdateColumns(width);
    UpdateHeader();   // ширина заголовка известна только после раскладки
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void Panel::UpdateColumns(int width) {
    if (!list_) return;
    const Settings& st = GetSettings();
    int extW = Scale(st.colWidth[1], dpi_);
    int sizeW = Scale(st.colWidth[2], dpi_);
    int dateW = Scale(st.colWidth[3], dpi_);
    int nameW = Scale(st.colWidth[0], dpi_);

    // Если суммарно уже́ ширины панели — имя добирает остаток.
    const int rest = width - extW - sizeW - dateW - Scale(24, dpi_);
    if (nameW < rest) nameW = rest;
    if (nameW < Scale(90, dpi_)) nameW = Scale(90, dpi_);

    ListView_SetColumnWidth(list_, 0, nameW);
    ListView_SetColumnWidth(list_, 1, extW);
    ListView_SetColumnWidth(list_, 2, sizeW);
    ListView_SetColumnWidth(list_, 3, dateW);
    UpdateSortArrows();
}

// Стрелки сортировки в заголовке колонки: «Имя ▲» / «Тип ▼», как в TC.
void Panel::UpdateSortArrows() {
    if (!list_) return;
    static const wchar_t* kBase[4] = {L"Имя", L"Тип", L"Размер", L"Дата"};
    for (int i = 0; i < 4; ++i) {
        std::wstring text = kBase[i];
        if (i == sortColumn_) text += sortReverse_ ? L" \u25BC" : L" \u25B2";
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = (LPWSTR)text.c_str();
        ListView_SetColumn(list_, i, &col);
    }
}

// Пользователь потянул границу колонки — запоминаем ширины.
void Panel::SaveColumnWidths() {
    if (!list_) return;
    Settings& st = GetSettings();
    for (int i = 0; i < 4; ++i) st.colWidth[i] = ListView_GetColumnWidth(list_, i);
    SaveSettings();
}

void Panel::SetActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    UpdateHeader();
    LogDebug(L"активация: панель " + std::to_wstring(side_) +
             (active ? L" активна, курсор " : L" неактивна, курсор ") +
             std::to_wstring(CursorIndex()));   // временная диагностика этапа 4
    // Курсор: на активной панели — синяя заливка, на неактивной — пунктирная рамка.
    // Смена LVIS_SELECTED не всегда перерисовывает строку в списке без фокуса —
    // перерисовываем явно, чтобы заливка/рамка появлялись сразу.
    const int index = CursorIndex();
    if (index >= 0) {
        ListView_SetItemState(list_, index, active ? LVIS_SELECTED : 0, LVIS_SELECTED);
        ListView_RedrawItems(list_, index, index);
    }
}

// Строка под панелью дисков: сколько свободно на текущем томе.
std::wstring Panel::DisplayPath() const {
    if (!inArchive_) return path_;
    std::wstring out = archiveFile_;
    if (!archiveInner_.empty()) out += L"\\" + archiveInner_;
    return out;
}

void Panel::UpdateVolume() {
    if (!volume_) return;
    std::wstring text;
    if (ftp_) {
        FtpSite site;
        std::wstring remote;
        text = L"FTP";
        if (FtpParseUrl(path_, site, remote)) text += L" · " + site.host;
    } else if (inArchive_) {
        text = L"архив";
    } else if (path_.rfind(L"\\\\", 0) == 0) {
        // Сетевой путь (\\сервер\ресурс): объём есть не всегда.
        const std::wstring summary = VolumeSummary(path_);
        text = summary.empty() ? L"сеть" : (L"сеть · " + summary);
    } else {
        text = VolumeSummary(path_);
    }
    SetWindowTextW(volume_, text.c_str());
}

void Panel::UpdateHeader() {
    if (!pathEdit_) return;
    HidePathHover();
    // Пока поле в режиме правки, не затираем введённое.
    if (pathEditing_) return;
    const std::wstring path = DisplayPath();
    if (path.empty()) return;
    SetWindowTextW(pathEdit_, path.c_str());
}

// Индекс символа под точкой (x,y) внутри поля пути.
int Panel::PathCharIndexAt(int x, int y) const {
    if (!pathEdit_) return 0;
    const LRESULT r = SendMessageW(pathEdit_, EM_CHARFROMPOS, 0, MAKELPARAM(x, y));
    int index = (int)(short)LOWORD(r);
    const int len = GetWindowTextLengthW(pathEdit_);
    if (index < 0) index = 0;
    if (index > len) index = len;
    return index;
}

// Клик правее текста — «свободное место» поля: там включается правка.
bool Panel::PathClickInEmptyArea(int x) const {
    if (!pathEdit_) return true;
    const std::wstring text = DisplayPath();
    HDC dc = GetDC(pathEdit_);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(pathEdit_, WM_GETFONT, 0, 0));
    SIZE sz{};
    if (!text.empty()) GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(pathEdit_, dc);

    const int margin = Scale(4, dpi_) + GetSystemMetrics(SM_CXBORDER);
    return x >= margin + sz.cx;
}

// Переход по клику на компонент пути в нередактируемом состоянии.
// ---------- выпадающий список каталога: каскад папок, как в TC ----------

// Путь, которому соответствует компонент с индексом charIndex.
std::wstring Panel::PathPartTarget(int charIndex) const {
    const std::wstring text = DisplayPath();
    if (text.empty()) return L"";

    int end = -1;
    for (int i = 0; i < (int)text.size(); ++i) {
        if (text[i] != L'\\') continue;
        end = i;
        if (charIndex < end) break;
        end = -1;
    }
    if (end < 0) end = (int)text.size();
    if (end <= 0) return L"";

    std::wstring target = text.substr(0, end);
    if (target.size() == 2 && target[1] == L':') target += L'\\';
    return target;
}

// Начало компонента с индексом charIndex (для позиции всплывающего списка).
int Panel::PathPartStart(int charIndex) const {
    const std::wstring text = DisplayPath();
    if (text.empty()) return 0;

    int start = 0;
    int end = -1;
    for (int i = 0; i < (int)text.size(); ++i) {
        if (text[i] != L'\\') continue;
        end = i;
        if (charIndex < end) break;
        start = i + 1;
        end = -1;
    }
    return start;
}

// Наведение на компонент пути: запоминаем и через паузу открываем первый уровень.
void Panel::SchedulePathHover(int charIndex) {
    if (ftp_) {
        HidePathHover();
        return;
    }
    if (charIndex < 0) {
        HidePathHover();
        return;
    }
    if (charIndex == hoverChars_ && !hover_.empty()) return;   // уже на этом компоненте
    hoverChars_ = charIndex;

    if (pathEdit_) SetTimer(pathEdit_, kHoverShowTimer, 350, nullptr);
}

// Клик по компоненту пути — переход в этот каталог.
void Panel::NavigatePathPart(int charIndex) {
    const std::wstring target = PathPartTarget(charIndex);
    if (target.empty()) return;
    LogDebug(L"path click: '" + target + L"'");
    if (!Navigate(target)) {
        MessageBoxW(GetParent(hwnd_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
    }
    FocusList();
}

void Panel::HidePathHover() {
    if (pathEdit_) {
        KillTimer(pathEdit_, kHoverShowTimer);
        KillTimer(pathEdit_, kHoverHideTimer);
        KillTimer(pathEdit_, kHoverCascadeTimer);
    }
    CloseLevelsFrom(0);
    hoverChars_ = -1;
}

// Окно уровня: рамка + список с иконками и прокруткой.
bool Panel::CreateLevelWindow(HoverLevel& lv) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HoverProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = kHoverClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    lv.win = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kHoverClass, L"", WS_POPUP | WS_BORDER,
                             0, 0, 10, 10, hwnd_, nullptr, g_inst, this);
    if (!lv.win) return false;

    // LVS_SHAREIMAGELISTS обязателен: иначе при закрытии уровня уничтожается ОБЩИЙ
    // список значков и иконки пропадают во всех панелях.
    lv.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
                                  LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_VSCROLL,
                              0, 0, 10, 10, lv.win, (HMENU)(INT_PTR)1, g_inst, nullptr);
    if (lv.list) {
        SetWindowSubclass(lv.list, HoverListProc, 4, (DWORD_PTR)this);
        ListView_SetExtendedListViewStyle(lv.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetImageList(lv.list, ShellSmallImageList(), LVSIL_SMALL);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Имя";
        col.cx = Scale(280, dpi_);
        ListView_InsertColumn(lv.list, 0, &col);
    }
    return true;
}

// Заполняем уровень: только каталоги, порядок как в панели.
void Panel::FillLevel(HoverLevel& lv) {
    if (!lv.list) return;
    ListView_DeleteAllItems(lv.list);
    lv.items.clear();

    std::vector<Entry> all;
    DWORD err = 0;
    if (!ListDir(lv.dir, showHidden_, all, err)) {
        lv.dir.clear();
        return;
    }
    std::sort(all.begin(), all.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, 0, false); });

    LVITEMW item{};
    const size_t limit = 400;
    for (size_t i = 0; i < all.size() && i < limit; ++i) {
        if (!all[i].isDirFs()) continue;                 // в списке только папки
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = (int)lv.items.size();
        item.pszText = (LPWSTR)all[i].name.c_str();
        item.iImage = IconForEntry(all[i], lv.dir);
        ListView_InsertItem(lv.list, &item);
        lv.items.push_back(all[i]);
        if (lv.items.size() >= limit) break;
    }
}

// Открыть уровень каскада в точке (x, y). Возвращает индекс уровня или -1.
int Panel::OpenLevel(const std::wstring& dir, int x, int y) {
    if (dir.empty() || !IsDirectory(dir)) return -1;

    HoverLevel lv;
    lv.dir = dir;
    if (!CreateLevelWindow(lv)) return -1;
    FillLevel(lv);
    if (lv.items.empty()) {
        DestroyWindow(lv.win);
        return -1;                                       // нет подкаталогов — уровень не нужен
    }

    const int rowH = Scale(18, dpi_);
    const int count = (int)lv.items.size();
    const int rows = (count < 18) ? (count < 3 ? 3 : count) : 18;
    const int width = Scale(300, dpi_);
    const int height = rows * rowH + Scale(6, dpi_);

    SetWindowPos(lv.win, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    MoveWindow(lv.list, 1, 1, width - 2, height - 2, TRUE);
    hover_.push_back(std::move(lv));
    return (int)hover_.size() - 1;
}

// Закрыть уровни с index и глубже.
void Panel::CloseLevelsFrom(size_t index) {
    if (index >= hover_.size()) return;
    closingHover_ = true;
    for (size_t i = hover_.size(); i-- > index;) {
        if (hover_[i].win) DestroyWindow(hover_[i].win);
    }
    hover_.resize(index);
    closingHover_ = false;
    if (pathEdit_) KillTimer(pathEdit_, kHoverCascadeTimer);
}

// Первый уровень: под компонентом пути.
void Panel::ShowPathHover() {
    if (!pathEdit_ || hoverChars_ < 0 || pathEditing_) return;

    const std::wstring dir = PathPartTarget(hoverChars_);
    if (dir.empty() || !IsDirectory(dir)) {
        HidePathHover();
        return;
    }

    CloseLevelsFrom(0);

    RECT er{};
    GetWindowRect(pathEdit_, &er);
    int x = er.left;
    const LRESULT pos = SendMessageW(pathEdit_, EM_POSFROMCHAR, (WPARAM)PathPartStart(hoverChars_), 0);
    if (pos != -1) x += (int)(short)LOWORD(pos);

    if (OpenLevel(dir, x, er.bottom) < 0) HidePathHover();
}

// Мышь над самим списком: не даём таймеру скрытия сработать (иначе список исчезал под курсором).
LRESULT CALLBACK Panel::HoverListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                      DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    if (self && self->pathEdit_) {
        switch (msg) {
            case WM_MOUSEMOVE: {
                // где мышь: уровень и пункт
                int levelIdx = -1, item = -1;
                for (size_t i = 0; i < self->hover_.size(); ++i) {
                    if (self->hover_[i].list == hwnd) {
                        levelIdx = (int)i;
                        break;
                    }
                }
                if (levelIdx >= 0) {
                    LVHITTESTINFO ht{};
                    ht.pt.x = GET_X_LPARAM(lParam);
                    ht.pt.y = GET_Y_LPARAM(lParam);
                    item = ListView_HitTest(self->hover_[levelIdx].list, &ht);
                    self->hover_[levelIdx].hoverItem = item;

                    if (item >= 0) {
                        self->cascadePendingLevel_ = levelIdx;
                        self->cascadePendingItem_ = item;
                        SetTimer(self->pathEdit_, kHoverCascadeTimer, 300, nullptr);
                    } else {
                        self->cascadePendingLevel_ = -1;
                        KillTimer(self->pathEdit_, kHoverCascadeTimer);
                    }
                }

                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                KillTimer(self->pathEdit_, kHoverHideTimer);
                break;
            }

            case WM_MOUSELEAVE:
                // Ушли со списка — закрываем, если курсор не на другом уровне каскада.
                SetTimer(self->pathEdit_, kHoverHideTimer, 250, nullptr);
                break;

            case WM_LBUTTONDOWN: {
                // Выбор папки — переход в панели.
                for (size_t i = 0; i < self->hover_.size(); ++i) {
                    if (self->hover_[i].list != hwnd) continue;
                    LVHITTESTINFO ht{};
                    ht.pt.x = GET_X_LPARAM(lParam);
                    ht.pt.y = GET_Y_LPARAM(lParam);
                    const int item = ListView_HitTest(self->hover_[i].list, &ht);
                    if (item >= 0 && item < (int)self->hover_[i].items.size()) {
                        const std::wstring target =
                            JoinPath(self->hover_[i].dir, self->hover_[i].items[item].name);
                        self->HidePathHover();
                        if (!self->Navigate(target)) {
                            MessageBoxW(GetParent(self->hwnd_), self->NavigateError().c_str(), L"Переход",
                                        MB_ICONERROR);
                        }
                        self->FocusList();
                    }
                    break;
                }
                break;
            }

            case WM_LBUTTONDBLCLK:
                // Двойной клик ведёт себя как одиночный (переход), список уже закрыт первым кликом.
                break;

            default:
                break;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Окно уровня: каретка не нужна, колесо и клики уходят в список.
LRESULT CALLBACK Panel::HoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Panel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }

    int levelIdx = -1;
    if (self) {
        for (size_t i = 0; i < self->hover_.size(); ++i) {
            if (self->hover_[i].win == hwnd) {
                levelIdx = (int)i;
                break;
            }
        }
    }

    switch (msg) {
        case WM_MOUSEMOVE:
            if (self && self->pathEdit_) KillTimer(self->pathEdit_, kHoverHideTimer);
            break;

        case WM_MOUSELEAVE:
            if (self && self->pathEdit_) SetTimer(self->pathEdit_, kHoverHideTimer, 250, nullptr);
            break;

        case WM_MOUSEWHEEL:
            if (levelIdx >= 0 && self->hover_[levelIdx].list) {
                SendMessageW(self->hover_[levelIdx].list, WM_MOUSEWHEEL, wParam, lParam);
                return 0;
            }
            break;

        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && hdr->code == NM_CUSTOMDRAW && levelIdx >= 0) {
                auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                    // Стрелка ► у папки, на которую наведена мышь: у неё откроется подуровень.
                    if (self->hover_[levelIdx].hoverItem == (int)cd->nmcd.dwItemSpec) {
                        RECT rc{};
                        ListView_GetItemRect(self->hover_[levelIdx].list, (int)cd->nmcd.dwItemSpec, &rc,
                                             LVIR_BOUNDS);
                        const int cx = rc.right - Scale(9, self->dpi_);
                        const int cy = (rc.top + rc.bottom) / 2;
                        const int s = Scale(4, self->dpi_);
                        POINT pts[3] = {{cx - s, cy - s}, {cx - s, cy + s}, {cx + s, cy}};
                        HBRUSH br = CreateSolidBrush(RGB(60, 60, 60));
                        HGDIOBJ ob = SelectObject(cd->nmcd.hdc, br);
                        Polygon(cd->nmcd.hdc, pts, 3);
                        SelectObject(cd->nmcd.hdc, ob);
                        DeleteObject(br);
                    }
                }
                return CDRF_DODEFAULT;
            }
            break;
        }

        case WM_DESTROY:
            // Одиночное разрушение окна (не массовое) — убираем уровень из цепочки.
            if (self && !self->closingHover_) {
                for (size_t i = 0; i < self->hover_.size(); ++i) {
                    if (self->hover_[i].win == hwnd) {
                        self->hover_.erase(self->hover_.begin() + i);
                        break;
                    }
                }
            }
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Войти в режим правки пути: клик по полю, белый фон, мигающий курсор.
void Panel::EnterPathEdit() {
    if (!pathEdit_ || pathEditing_) return;
    HidePathHover();
    pathEditing_ = true;
    SendMessageW(pathEdit_, EM_SETREADONLY, FALSE, 0);
    Layout();                       // появляется кнопка ✓, поле сужается
    InvalidateRect(pathEdit_, nullptr, TRUE);
    LogDebug(L"path edit: вход в режим правки");
}

// Выйти из правки: поле снова только показывает путь.
void Panel::LeavePathEdit(bool restore) {
    if (!pathEdit_) return;
    pathEditing_ = false;
    SendMessageW(pathEdit_, EM_SETREADONLY, TRUE, 0);
    SendMessageW(pathEdit_, EM_SETSEL, 0, 0);
    if (restore) UpdateHeader();
    Layout();                       // кнопка ✓ снова скрыта
    InvalidateRect(pathEdit_, nullptr, TRUE);
}

// Перейти по тексту из поля пути (Enter или кнопка ✓).
void Panel::ApplyPathText() {
    if (!pathEdit_) return;
    const int len = GetWindowTextLengthW(pathEdit_);
    std::wstring text;
    text.resize(len);
    if (len > 0) GetWindowTextW(pathEdit_, &text[0], len + 1);
    text = Trim(text);
    if (text.empty()) {
        UpdateHeader();
        return;
    }
    if (!Navigate(text)) {
        MessageBoxW(GetParent(hwnd_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
        LeavePathEdit(true);
        return;
    }
    LeavePathEdit(false);
    FocusList();
}

// ▼ — история посещённых каталогов.
void Panel::ShowPathHistory() {
    if (history_.empty()) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const int limit = 24;
    int shown = 0;
    for (int i = (int)history_.size() - 1; i >= 0 && shown < limit; --i, ++shown) {
        AppendMenuW(menu, MF_STRING, 100 + shown, history_[i].c_str());
    }

    RECT rc{};
    GetWindowRect(pathHist_, &rc);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0,
                                   hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd < 100) return;

    const int index = (int)history_.size() - 1 - (cmd - 100);
    if (index < 0 || index >= (int)history_.size()) return;
    if (!Navigate(history_[index])) MessageBoxW(GetParent(hwnd_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
    FocusList();
}

// ★ — избранные каталоги (закладки) плюс добавление текущего.
void Panel::ShowPathFavorites() {
    Settings& st = GetSettings();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    int shown = 0;
    for (size_t i = 0; i < st.bookmarks.size(); ++i) {
        AppendMenuW(menu, MF_STRING, 200 + (int)i, st.bookmarks[i].c_str());
        ++shown;
    }
    if (shown) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, L"Добавить текущий каталог");

    RECT rc{};
    GetWindowRect(pathFav_, &rc);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0,
                                   hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
        if (path_.empty() || inArchive_) return;
        bool exists = false;
        for (auto& b : st.bookmarks) {
            if (SamePath(b, path_)) exists = true;
        }
        if (!exists) st.bookmarks.push_back(path_);
        SaveSettings();
        return;
    }
    if (cmd < 200) return;

    const size_t index = (size_t)(cmd - 200);
    if (index >= st.bookmarks.size()) return;
    if (!Navigate(st.bookmarks[index])) MessageBoxW(GetParent(hwnd_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
    FocusList();
}

// ---------- маска файлов (этап 5 план-UI) ----------

void Panel::UpdateMaskField() {
    if (maskEdit_) SetWindowTextW(maskEdit_, mask_.c_str());
}

// Последние 8 масок — общая история в настройках; повтор маски поднимает его наверх.
void Panel::PushMaskHistory(const std::wstring& mask) {
    Settings& st = GetSettings();
    for (auto it = st.maskHistory.begin(); it != st.maskHistory.end();) {
        if (SameName(*it, mask)) {
            it = st.maskHistory.erase(it);
        } else {
            ++it;
        }
    }
    st.maskHistory.push_back(mask);
    if (st.maskHistory.size() > 8) {
        st.maskHistory.erase(st.maskHistory.begin(), st.maskHistory.end() - 8);
    }
    SaveSettings();
}

// Применить маску и перечитать каталог: Enter в поле или выбор из истории.
void Panel::SetMask(const std::wstring& mask) {
    std::wstring text = Trim(mask);
    if (text.empty()) text = L"*.*";
    mask_ = text;
    PushMaskHistory(text);
    UpdateMaskField();
    Reload();
}

// При запуске: просто выставить поле, фильтр применится при чтении каталога.
void Panel::RestoreMask(const std::wstring& mask) {
    std::wstring text = Trim(mask);
    if (text.empty()) text = L"*.*";
    mask_ = text;
    UpdateMaskField();
}

void Panel::EnterMaskEdit() {
    if (!maskEdit_ || maskEditing_) return;
    maskEditing_ = true;
    SendMessageW(maskEdit_, EM_SETREADONLY, FALSE, 0);
    SetFocus(maskEdit_);
    SendMessageW(maskEdit_, EM_SETSEL, 0, -1);   // всё выделено: ввод заменяет маску
    InvalidateRect(maskEdit_, nullptr, TRUE);
}

void Panel::LeaveMaskEdit(bool restore) {
    if (!maskEdit_) return;
    maskEditing_ = false;
    SendMessageW(maskEdit_, EM_SETREADONLY, TRUE, 0);
    SendMessageW(maskEdit_, EM_SETSEL, 0, 0);
    if (restore) UpdateMaskField();
    InvalidateRect(maskEdit_, nullptr, TRUE);
}

void Panel::ConfirmMaskEdit() {
    if (!maskEditing_) return;
    wchar_t buf[256]{};
    GetWindowTextW(maskEdit_, buf, 256);
    LeaveMaskEdit(false);
    SetMask(buf);
    FocusList();
}

void Panel::CancelMaskEdit() {
    if (!maskEditing_) return;
    LeaveMaskEdit(true);
    FocusList();
}

// ▼ — история масок: свежие сверху, плюс «*.* — все файлы».
void Panel::ShowMaskHistory() {
    Settings& st = GetSettings();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    int shown = 0;
    for (int i = (int)st.maskHistory.size() - 1; i >= 0; --i, ++shown) {
        AppendMenuW(menu, MF_STRING, 100 + shown, st.maskHistory[i].c_str());
    }
    if (shown) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1, L"*.*  (все файлы)");
    } else {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(история пуста)");
    }

    RECT rc{};
    GetWindowRect(maskHist_, &rc);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0,
                                   hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) {
        SetMask(L"*.*");
        FocusList();
    } else if (cmd >= 100) {
        const int index = (int)st.maskHistory.size() - 1 - (cmd - 100);
        if (index >= 0 && index < (int)st.maskHistory.size()) {
            SetMask(st.maskHistory[index]);
            FocusList();
        }
    }
}

// Поле маски: клик — правка, Enter/Tab — применить, Esc — отменить, уход фокуса — отмена.
LRESULT CALLBACK Panel::MaskEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                      DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    switch (msg) {
        case WM_SETFOCUS:
            // Поле на неактивной панели получило фокус — активируем её (как поле пути).
            if (self && !self->active_) {
                PostMessageW(GetParent(self->hwnd_), WM_APP + 303, (WPARAM)self->side_, 0);
            }
            break;
        case WM_LBUTTONDOWN:
            if (self && !self->maskEditing_) self->EnterMaskEdit();
            break;
        case WM_KEYDOWN:
            if (!self) break;
            if (wParam == VK_RETURN || wParam == VK_TAB) {
                self->ConfirmMaskEdit();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                self->CancelMaskEdit();
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            if (self && self->maskEditing_) self->LeaveMaskEdit(true);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Маска действует только на файлы; каталоги и «..» остаются всегда (как в TC).
// «*.*» и «*» — показать всё (в том числе файлы без расширения).
void Panel::FilterByMask() {
    const auto masks = SplitMasks(mask_);
    bool all = masks.empty();
    for (auto& m : masks) {
        if (m == L"*.*" || m == L"*") {
            all = true;
            break;
        }
    }
    if (all) return;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&masks](const Entry& e) {
                                      if (e.isUp || e.isDir) return false;
                                      const std::wstring lower = Lower(e.name);
                                      for (auto& m : masks) {
                                          if (MatchMask(lower, m)) return false;
                                      }
                                      return true;
                                  }),
                   entries_.end());
}

// Кнопки ✓ ★ ▼: плоские, знак рисуем сами.
void Panel::DrawPathButton(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH fill = CreateSolidBrush(pressed ? RGB(200, 198, 192) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldBrush);

    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const int s = Scale(4, dpi_);

    switch ((int)dis->CtlID) {
        case kPathApplyId: {                       // галочка
            HPEN thick = CreatePen(PS_SOLID, Scale(2, dpi_), RGB(30, 110, 30));
            HGDIOBJ op = SelectObject(dc, thick);
            MoveToEx(dc, cx - s, cy, nullptr);
            LineTo(dc, cx - Scale(1, dpi_), cy + s);
            LineTo(dc, cx + s, cy - s);
            SelectObject(dc, op);
            DeleteObject(thick);
            break;
        }
        case kPathFavId: {                         // звёздочка «*» с лучами
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - Scale(2, dpi_), cy - Scale(2, dpi_), cx + Scale(2, dpi_), cy + Scale(2, dpi_));
            for (int i = 0; i < 3; ++i) {
                const int dy = (i - 1) * Scale(3, dpi_);
                MoveToEx(dc, cx, cy + dy, nullptr);
                LineTo(dc, cx, cy + dy);
            }
            MoveToEx(dc, cx - s, cy, nullptr);
            LineTo(dc, cx + s, cy);
            MoveToEx(dc, cx - s + 1, cy - s + 1, nullptr);
            LineTo(dc, cx + s - 1, cy + s - 1);
            MoveToEx(dc, cx - s + 1, cy + s - 1, nullptr);
            LineTo(dc, cx + s - 1, cy - s + 1);
            SelectObject(dc, ob);
            break;
        }
        case kPathHistId:                        // треугольник вниз
        case kMaskHistId:                        // тот же знак у истории масок
        {
            POINT pts[3] = {{cx - s, cy - s / 2}, {cx + s, cy - s / 2}, {cx, cy + s}};
            HBRUSH br = CreateSolidBrush(RGB(60, 60, 60));
            HGDIOBJ ob = SelectObject(dc, br);
            Polygon(dc, pts, 3);
            SelectObject(dc, ob);
            DeleteObject(br);
            break;
        }
        default:
            break;
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void Panel::FocusList() {
    if (list_) SetFocus(list_);
}

// Список содержимого архива для текущего внутреннего каталога.
bool Panel::LoadArchiveListing(const std::wstring& focus) {
    lastError_.clear();

    std::vector<ArchiveEntry> all;
    if (!ListArchive(archiveFile_, all)) {
        lastError_ = L"Не удалось прочитать архив:\n" + archiveFile_;
        return false;
    }

    std::vector<Entry> loaded;
    Entry up;
    up.name = L"..";
    up.isDir = true;
    up.isUp = true;
    up.icon = IconForEntry(up, path_);
    loaded.push_back(std::move(up));

    const std::wstring prefix = archiveInner_.empty() ? L"" : archiveInner_ + L"\\";
    std::vector<std::wstring> seenDirs;

    for (auto& e : all) {
        std::wstring name = e.name;
        for (auto& ch : name) {
            if (ch == L'/') ch = L'\\';
        }
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) continue;

        std::wstring rest = name.substr(prefix.size());
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (rest.empty()) continue;

        const size_t slash = rest.find(L'\\');
        if (slash == std::wstring::npos) {
            Entry en;
            en.name = rest;
            en.isDir = e.isDir;
            en.size = e.size.empty() ? 0 : wcstoull(e.size.c_str(), nullptr, 10);
            en.icon = IconForEntry(en, path_);
            loaded.push_back(std::move(en));
        } else {
            const std::wstring dirName = rest.substr(0, slash);
            bool known = false;
            for (auto& d : seenDirs) {
                if (d == dirName) {
                    known = true;
                    break;
                }
            }
            if (known) continue;
            seenDirs.push_back(dirName);

            Entry en;
            en.name = dirName;
            en.isDir = true;
            en.icon = IconForEntry(en, path_);
            loaded.push_back(std::move(en));
        }
    }

    path_ = archiveFile_;
    entries_ = std::move(loaded);
    FilterByMask();                        // внутри архива — тот же фильтр

    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });

    ApplyEntries(focus, 0);
    UpdateHeader();
    UpdateStatus();
    return true;
}

bool Panel::LoadFtpListing(const std::wstring& url, const std::wstring& focus) {
    lastError_.clear();
    const ULONGLONG started = GetTickCount64();

    FtpSite site;
    std::wstring remote;
    if (!FtpParseUrl(url, site, remote)) {
        lastError_ = L"Некорректный FTP-адрес:\n" + url;
        return false;
    }
    const std::wstring normalized = FtpMakeUrl(site, remote);
    if (!site.password.empty()) FtpRememberPassword(normalized, site.password);

    // Заблокированная вкладка держит свой каталог и для FTP.
    if (!navigatingHistory_ && TabLocked(tab_) && tab_ < (int)tabs_.size() &&
        !SamePath(normalized, tabs_[tab_])) {
        lastError_ = L"Вкладка заблокирована: " + tabs_[tab_];
        return false;
    }

    if (!site.user.empty() && !FtpPasswordFor(normalized, site.password)) {
        lastError_ = L"Нет пароля для " + site.user + L"@" + site.host + L".\n"
                     L"Подключитесь через меню «Сеть» → «FTP: подключиться…».";
        return false;
    }

    std::unique_ptr<FtpConnection> conn;
    std::wstring error;
    if (!FtpConnect(site, conn, error)) {
        lastError_ = L"Не удалось открыть FTP-каталог:\n" + normalized + L"\n\n" + error;
        return false;
    }
    const ULONGLONG connectedAt = GetTickCount64();

    std::vector<FtpEntryInfo> listing;
    const bool listed = FtpList(*conn, remote, listing, error);
    FtpDisconnect(conn);
    if (!listed) {
        lastError_ = L"Не удалось прочитать FTP-каталог:\n" + normalized + L"\n\n" + error;
        return false;
    }
    const ULONGLONG listedAt = GetTickCount64();

    std::vector<Entry> loaded;
    if (remote != L"/") {
        Entry up;
        up.name = L"..";
        up.isDir = true;
        up.isUp = true;
        up.icon = IconForFtpEntry(up);
        loaded.push_back(std::move(up));
    }
    for (auto& item : listing) {
        Entry e;
        e.name = item.name;
        e.isDir = item.isDir;
        e.size = item.size;
        e.ft = item.ft;
        e.attr = item.attr;
        e.icon = IconForFtpEntry(e);
        loaded.push_back(std::move(e));
    }
    const ULONGLONG builtAt = GetTickCount64();

    path_ = normalized;
    ftp_ = true;
    inArchive_ = false;
    archiveFile_.clear();
    archiveInner_.clear();
    entries_ = std::move(loaded);
    FilterByMask();

    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });

    ApplyEntries(focus, 0);

    if (tab_ >= 0 && tab_ < (int)tabs_.size() && !TabLocked(tab_)) {
        if (tabs_[tab_] != path_) {
            tabs_[tab_] = path_;
            Layout();
        }
    }

    UpdateHeader();
    UpdateStatus();
    PushHistoryPoint();
    LogDebug(L"FTP: " + path_ + L", записей " + std::to_wstring(entries_.size()) +
             L", connect=" + std::to_wstring(connectedAt - started) +
             L" list=" + std::to_wstring(listedAt - connectedAt) +
             L" build=" + std::to_wstring(builtAt - listedAt) +
             L" total=" + std::to_wstring(GetTickCount64() - started) + L" мс");
    return true;
}

bool Panel::Navigate(const std::wstring& dir, const std::wstring& focus, bool keepCursor) {
    lastError_.clear();

    // FTP-URL: отдельный источник списка, обычные пути к нему не применимы.
    if (IsFtpUrl(dir)) {
        inArchive_ = false;
        archiveFile_.clear();
        archiveInner_.clear();
        return LoadFtpListing(dir, focus);
    }

    // Внутри архива: переход по его каталогам или выход наружу.
    if (inArchive_) {
        const std::wstring prefix = archiveFile_ + L"\\";
        if (dir == archiveFile_) {
            archiveInner_.clear();
            return LoadArchiveListing(focus);
        }
        if (dir.rfind(prefix, 0) == 0) {
            std::wstring inner = dir.substr(prefix.size());
            while (!inner.empty() && (inner.back() == L'\\' || inner.back() == L'/')) inner.pop_back();
            archiveInner_ = inner;
            return LoadArchiveListing(focus);
        }
        // Уходим из архива в обычный каталог
        inArchive_ = false;
        archiveFile_.clear();
        archiveInner_.clear();
    }

    std::wstring target = NormalizeDir(dir);
    if (target.empty()) {
        lastError_ = L"Пустой путь";
        return false;
    }

    // Заблокированная вкладка держит свой каталог.
    if (!navigatingHistory_ && TabLocked(tab_) && tab_ < (int)tabs_.size() && !SamePath(target, tabs_[tab_])) {
        lastError_ = L"Вкладка заблокирована: " + tabs_[tab_];
        return false;
    }

    // Файл-архив открываем как папку.
    if (IsArchivePath(target) && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && !IsDirectory(target)) {
        inArchive_ = true;
        archiveFile_ = target;
        archiveInner_.clear();
        return LoadArchiveListing(focus);
    }

    ULONGLONG t0 = GetTickCount64();
    std::vector<Entry> loaded;
    DWORD err = 0;
    bool listed = ListDir(target, showHidden_, loaded, err);
    ULONGLONG t1 = GetTickCount64();
    if (!listed) {
        lastError_ = L"Не удалось открыть папку:\n" + target + L"\n\n" + ErrText(err);
        return false;
    }

    int fallback = 0;
    if (keepCursor) {
        int cur = CursorIndex();
        if (cur >= 0) fallback = cur;
    }

    std::wstring previousPath = path_;
    path_ = target;
    ftp_ = false;
    entries_ = std::move(loaded);
    FilterByMask();                        // маска действует и при смене каталога

    if (!IsRootPath(path_)) {
        Entry up;
        up.name = L"..";
        up.isDir = true;
        up.isUp = true;
        up.icon = IconForEntry(up, path_);
        entries_.insert(entries_.begin(), std::move(up));
    }

    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });

    ULONGLONG t2 = GetTickCount64();
    ApplyEntries(focus, fallback);

    // Вкладка показывает текущий каталог (если не заблокирована).
    if (tab_ >= 0 && tab_ < (int)tabs_.size() && !TabLocked(tab_)) {
        if (tabs_[tab_] != path_) {
            tabs_[tab_] = path_;
            Layout();
        }
    }

    UpdateHeader();
    UpdateStatus();
    ULONGLONG t3 = GetTickCount64();
    LogDebug(L"Navigate: list=" + std::to_wstring(t1 - t0) + L" ms, sort=" + std::to_wstring(t2 - t1) +
             L" ms, apply=" + std::to_wstring(t3 - t2) + L" ms, entries=" + std::to_wstring(entries_.size()));

    // История навигации: обычный переход добавляет точку, переход по истории — нет.
    PushHistoryPoint();
    (void)previousPath;
    return true;
}

void Panel::PushHistoryPoint() {
    if (navigatingHistory_) return;
    if (histPos_ >= 0 && histPos_ < (int)history_.size() && history_[histPos_] == path_) return;
    history_.resize(histPos_ + 1);
    history_.push_back(path_);
    histPos_ = (int)history_.size() - 1;
}

// Быстрый поиск: первая запись, начинающаяся с набранных букв.
bool Panel::FocusPrefix(const std::wstring& prefix) {
    if (prefix.empty()) return false;
    const std::wstring needle = Lower(prefix);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].isUp) continue;
        const std::wstring name = Lower(entries_[i].name);
        if (name.rfind(needle, 0) == 0) {
            SetCursorIndex((int)i);
            return true;
        }
    }
    return false;
}

bool Panel::GoBack() {
    if (!CanGoBack()) return false;
    int target = histPos_ - 1;
    navigatingHistory_ = true;
    bool ok = Navigate(history_[target]);
    navigatingHistory_ = false;
    if (ok) histPos_ = target;
    return ok;
}

bool Panel::GoForward() {
    if (!CanGoForward()) return false;
    int target = histPos_ + 1;
    navigatingHistory_ = true;
    bool ok = Navigate(history_[target]);
    navigatingHistory_ = false;
    if (ok) histPos_ = target;
    return ok;
}

void Panel::ApplyEntries(const std::wstring& focus, int fallbackIndex) {
    UpdateDriveBar();
    SyncTreeToPath();
    UpdateVolume();
    if (tabs_.empty()) {
        if (!path_.empty()) {          // до первой навигации вкладку не создаём
            tabs_.push_back(path_);
            tab_ = 0;
            RebuildTabBar();
        }
    } else {
        if (tab_ < 0) tab_ = 0;
        if (tab_ < (int)tabs_.size()) tabs_[tab_] = path_;
        UpdateTabText();
    }
    if (!list_) return;
    ListView_SetItemCountEx(list_, (int)entries_.size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    int index = -1;
    if (!focus.empty()) {
        for (int i = 0; i < (int)entries_.size(); i++) {
            if (SameName(entries_[i].name, focus)) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) index = fallbackIndex;
    if (index >= (int)entries_.size()) index = (int)entries_.size() - 1;
    if (index < 0) index = 0;
    SetCursorIndex(index);
    InvalidateRect(list_, nullptr, FALSE);
}

void Panel::Reload() {
    std::wstring focus;
    Entry* cur = Cursor();
    if (cur && !cur->isUp) focus = cur->name;

    std::vector<std::wstring> marked;
    for (auto& e : entries_) {
        if (e.marked) marked.push_back(e.name);
    }

    std::wstring dir = path_;
    if (!Navigate(dir, focus, true)) {
        std::wstring parent = ParentPath(dir);
        if (!parent.empty() && !SamePath(parent, dir)) Navigate(parent);
    }

    for (auto& e : entries_) {
        for (auto& m : marked) {
            if (SameName(e.name, m)) {
                e.marked = true;
                break;
            }
        }
    }
    if (list_) InvalidateRect(list_, nullptr, FALSE);
}

bool Panel::GoUp() {
    if (ftp_) {
        const std::wstring root = FtpRootUrl(path_);
        if (SamePath(root, path_)) return false;
        const std::wstring leaf = FtpLeafName(path_);
        return Navigate(FtpParentUrl(path_), leaf);
    }

    if (inArchive_) {
        if (archiveInner_.empty()) {
            // Выход из архива в его каталог
            const std::wstring file = archiveFile_;
            const std::wstring parent = ParentPath(file);
            inArchive_ = false;
            archiveFile_.clear();
            archiveInner_.clear();
            const std::wstring leaf = file.substr(file.find_last_of(L'\\') + 1);
            return Navigate(parent, leaf);
        }
        std::wstring inner = archiveInner_;
        const size_t slash = inner.find_last_of(L'\\');
        archiveInner_ = (slash == std::wstring::npos) ? L"" : inner.substr(0, slash);
        return LoadArchiveListing(L"");
    }

    if (IsRootPath(path_)) return false;
    std::wstring parent = ParentPath(path_);
    std::wstring child = path_.substr(path_.find_last_of(L'\\') + 1);
    return Navigate(parent, child);
}

bool Panel::GoRoot() {
    if (ftp_) {
        const std::wstring root = FtpRootUrl(path_);
        if (SamePath(root, path_)) return false;
        return Navigate(root);
    }

    std::wstring root = DriveRootOf(path_);
    if (SamePath(root, path_)) return false;
    return Navigate(root);
}

int Panel::CursorIndex() const {
    if (list_) {
        int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
        if (focused >= 0) return focused;
    }
    // Запасной путь: в списке без фокуса LVIS_FOCUSED может не стоять
    // (или сниматься, например кликом по пустому месту) — используем
    // сохранённую строку курсора. Нужен неактивной панели (рамка курсора).
    if (cursorRow_ >= 0 && cursorRow_ < (int)entries_.size()) return cursorRow_;
    return -1;
}

void Panel::SetCursorIndex(int index) {
    if (!list_) return;
    if (index < 0 || index >= (int)entries_.size()) index = 0;
    const int old = CursorIndex();
    cursorRow_ = index;   // рамка неактивной панели рисуется по этому индексу
    // Заливка строки курсора (LVIS_SELECTED) — только на активной панели;
    // на неактивной курсор рисуем пунктирной рамкой (DrawCursorFrame).
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list_, index, active_ ? (LVIS_SELECTED | LVIS_FOCUSED) : LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    // Список без фокуса не всегда перерисовывает строки при смене LVIS_FOCUSED:
    // рамка осталась бы на старой строке, на новой не появилась бы. Перерисовываем явно.
    if (!active_) {
        if (old >= 0 && old != index) ListView_RedrawItems(list_, old, old);
        ListView_RedrawItems(list_, index, index);
    }
    ListView_EnsureVisible(list_, index, FALSE);
}

Entry* Panel::Cursor() {
    int index = CursorIndex();
    if (index < 0 || index >= (int)entries_.size()) return nullptr;
    return &entries_[index];
}

std::wstring Panel::FullPathOf(const Entry& e) const {
    if (e.isUp) return ftp_ ? FtpParentUrl(path_) : ParentPath(path_);
    if (ftp_) return FtpJoinUrl(path_, e.name);
    return JoinPath(path_, e.name);
}

std::wstring Panel::FocusedPath() {
    Entry* e = Cursor();
    if (!e || e->isUp) return path_;
    return FullPathOf(*e);
}

std::vector<std::wstring> Panel::SelectedNames() const {
    std::vector<std::wstring> out;
    for (auto& e : entries_) {
        if (e.marked && !e.isUp) out.push_back(e.name);
    }
    if (out.empty()) {
        int index = CursorIndex();
        if (index >= 0 && index < (int)entries_.size() && !entries_[index].isUp) {
            out.push_back(entries_[index].name);
        }
    }
    return out;
}

std::vector<std::wstring> Panel::SelectedPaths() const {
    std::vector<std::wstring> out;
    for (auto& name : SelectedNames()) out.push_back(JoinPath(path_, name));
    return out;
}

void Panel::ToggleMarkCursor() {
    int index = CursorIndex();
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (e.isUp) return;
    e.marked = !e.marked;
    RECT rc{};
    if (ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS)) InvalidateRect(list_, &rc, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();

    // Как в TC: после отметки курсор переходит на следующую строку.
    if (index + 1 < (int)entries_.size()) {
        programmaticCursor_ = true;
        SetCursorIndex(index + 1);
        programmaticCursor_ = false;
        lastCursor_ = index + 1;
    }
}

// Отметить диапазон (Shift+клик).
void Panel::MarkRange(int from, int to) {
    if (from > to) std::swap(from, to);
    if (from < 0) from = 0;
    if (to >= (int)entries_.size()) to = (int)entries_.size() - 1;
    for (int i = from; i <= to; ++i) {
        if (entries_[i].isUp) continue;
        entries_[i].marked = true;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();
}

// Переключить отметку одной записи (Ctrl+клик).
void Panel::ToggleMarkAt(int index) {
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (e.isUp) return;
    e.marked = !e.marked;
    RECT rc{};
    if (ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS)) InvalidateRect(list_, &rc, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();
}

void Panel::MarkAll(bool mark, bool includingDirs) {
    for (auto& e : entries_) {
        if (e.isUp) continue;
        if (!includingDirs && e.isDir) continue;
        e.marked = mark;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();
}

void Panel::InvertMarks() {
    for (auto& e : entries_) {
        if (e.isUp) continue;
        e.marked = !e.marked;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();
}

// Отметка по маске: «*.txt» или несколько через пробел/запятую.
size_t Panel::MarkByMask(const std::wstring& maskText, int mode) {
    auto masks = SplitMasks(maskText);
    size_t hit = 0;
    for (auto& e : entries_) {
        if (e.isUp) continue;
        bool match = masks.empty();
        const std::wstring lower = Lower(e.name);
        for (auto& m : masks) {
            if (MatchMask(lower, m)) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        e.marked = (mode == 0) ? true : (mode == 1 ? false : !e.marked);
        ++hit;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
    StartMarkedDirSizes();
    return hit;
}

void Panel::ClearMarks() {
    for (auto& e : entries_) e.marked = false;
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
}

void Panel::Sort(int column, bool reverse) {
    sortColumn_ = column;
    sortReverse_ = reverse;
    std::wstring focus;
    Entry* cur = Cursor();
    if (cur && !cur->isUp) focus = cur->name;

    std::stable_sort(entries_.begin(), entries_.end(),
                     [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });
    ApplyEntries(focus, 0);
    UpdateSortArrows();
    InvalidateRect(list_, nullptr, FALSE);
}

void Panel::UpdateStatus() {
    if (!status_) return;

    // Путь элемента под курсором, как в TC («..» — родительский каталог);
    // при нехватке места путь сокращается слева: «…\src\main.cpp».
    std::wstring head = path_;
    if (Entry* cur = Cursor()) head = FullPathOf(*cur);
    RECT rc{};
    GetClientRect(status_, &rc);
    HFONT f = (HFONT)SendMessageW(status_, WM_GETFONT, 0, 0);
    SetWindowTextW(status_, CompactPathLeft(head, rc.right - rc.left - Scale(8, dpi_), f).c_str());

    // Справа — сводка по отметкам, как в TC. Папки учитываем после подсчёта их размера.
    if (!statusInfo_) return;
    size_t marked = 0;
    ULONGLONG markedSize = 0;
    int pendingDirs = 0;
    for (auto& e : entries_) {
        if (e.isUp) continue;
        if (!e.marked) continue;
        ++marked;
        if (e.isDir) {
            if (ftp_ || inArchive_) continue;       // размер таких каталогов не считаем
            if (e.dirSizeKnown) markedSize += e.dirSize;
            else ++pendingDirs;
        } else {
            markedSize += e.size;
        }
    }
    std::wstring info;
    if (marked) {
        info = L"Отмечено: " + std::to_wstring(marked) + L" · " + FmtSize(markedSize);
        if (pendingDirs > 0) info += L" + …";
    }
    SetWindowTextW(statusInfo_, info.c_str());
}

void Panel::FillDispInfo(NMLVDISPINFO* info) {
    int index = info->item.iItem;
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (info->item.mask & LVIF_TEXT) {
        std::wstring text;
        switch (info->item.iSubItem) {
            case 0:
                text = e.isUp ? L".." : e.stem();
                break;
            case 1:
                // У каталогов (и «..») колонка «Тип» пустая, как в TC.
                text = (e.isUp || e.isDir) ? L"" : Upper(e.ext()).substr(0, 8);
                if (!text.empty() && text[0] == L'.') text.erase(0, 1);
                break;
            case 2:
                if (e.isDir) {
                    text = e.dirSizeBusy ? L"…" : (e.dirSizeKnown ? FmtSize(e.dirSize) : L"<Папка>");
                } else {
                    text = FmtSize(e.size);
                }
                break;
            case 3:
                text = e.isUp ? L"" : FmtDate(e.ft);
                break;
            default:
                break;
        }
        wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
    }
    if (info->item.mask & LVIF_IMAGE) {
        info->item.iImage = ftp_ ? IconForFtpEntry(e) : IconForEntry(e, path_);
    }
}

// Имя отмеченного/скрытого файла: ListView не применяет clrText к первой колонке.
void Panel::DrawRowName(NMLVCUSTOMDRAW* cd) {
    const int index = (int)cd->nmcd.dwItemSpec;
    if (index < 0 || index >= (int)entries_.size()) return;

    Entry& e = entries_[index];
    if (!e.marked && !e.hidden()) return;
    if (ListView_GetItemState(list_, index, LVIS_SELECTED) & LVIS_SELECTED) return;   // выделенную не трогаем

    RECT label{};
    if (!ListView_GetItemRect(list_, index, &label, LVIR_LABEL)) return;

    // Стираем исходную чёрную надпись с запасом, иначе будут видны остатки.
    // Слева запас ограничен правым краем значка: иначе затирается половина иконки
    // у скрытых и отмеченных строк (значок идёт сразу перед текстом).
    RECT erase = label;
    erase.left -= Scale(6, dpi_);
    erase.right += Scale(6, dpi_);
    RECT iconRect{};
    if (ListView_GetItemRect(list_, index, &iconRect, LVIR_ICON) && iconRect.right > erase.left) {
        erase.left = iconRect.right;
    }
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(cd->nmcd.hdc, &erase, bg);
    DeleteObject(bg);

    SetBkMode(cd->nmcd.hdc, TRANSPARENT);
    SetTextColor(cd->nmcd.hdc, e.marked ? kMarkedText : kHiddenText);
    HGDIOBJ old = SelectObject(cd->nmcd.hdc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    RECT tr = label;   // рисуем там же, где рисовал ListView
    // В колонке «Имя» показывается имя без расширения (расширение — в колонке «Тип»).
    const std::wstring text = e.isUp ? e.name : e.stem();
    DrawTextW(cd->nmcd.hdc, text.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(cd->nmcd.hdc, old);
}

void Panel::DrawRow(NMLVCUSTOMDRAW* cd) {
    int index = (int)cd->nmcd.dwItemSpec;
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (e.marked) {
        cd->clrText = kMarkedText;
    } else if (e.hidden()) {
        cd->clrText = kHiddenText;
    }
}

// Пунктирная рамка обычным пером (а не DrawFocusRect: тот рисует в XOR, и при
// повторной отрисовке строки за один цикл рамка бы стёрлась).
namespace {
void DrawDottedFrame(HDC dc, RECT rc) {
    InflateRect(&rc, -1, -1);   // линия по центру пера — не залезаем на соседние строки
    HPEN pen = CreatePen(PS_DOT, 1, GetSysColor(COLOR_WINDOWTEXT));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, rc.left, rc.top, nullptr);
    LineTo(dc, rc.right, rc.top);
    LineTo(dc, rc.right, rc.bottom);
    LineTo(dc, rc.left, rc.bottom);
    LineTo(dc, rc.left, rc.top);
    SelectObject(dc, old);
    DeleteObject(pen);
}
}

// Пунктирная рамка вокруг строки курсора неактивной панели — как в TC
// (вместо серой заливки невыделенного списка).
void Panel::DrawCursorFrame(NMLVCUSTOMDRAW* cd) {
    if (active_) return;
    const int index = (int)cd->nmcd.dwItemSpec;
    if (index < 0 || index >= (int)entries_.size()) return;
    // Строка курсора — по CursorIndex(): LVNI_FOCUSED, а если фокус в списке
    // не стоял — сохранённый cursorRow_. На LVIS_FOCUSED напрямую не полагаемся.
    if (index != CursorIndex()) return;
    RECT rc{};
    if (!ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS)) return;
    DrawDottedFrame(cd->nmcd.hdc, rc);
    static int logged = 0;   // временная диагностика этапа 4 — убрать после приёмки
    if (logged++ < 40) {
        LogDebug(L"рамка: custom-draw, панель " + std::to_wstring(side_) +
                 L", строка " + std::to_wstring(index));
    }
}

// Та же рамка, но поверх уже отрисованного списка — вызывается из ListProc
// ПОСЛЕ DefSubclassProc(WM_PAINT). Не зависит от стадий custom draw
// (CDDS_ITEMPOSTPAINT на части систем не приходит) и от LVIS_FOCUSED.
void Panel::PaintCursorOverlay() {
    if (!list_ || active_) return;
    const int index = CursorIndex();
    if (index < 0 || index >= (int)entries_.size()) return;
    RECT rc{};
    if (!ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS)) return;
    RECT client{}, vis{};
    GetClientRect(list_, &client);
    if (!IntersectRect(&vis, &rc, &client)) return;   // строка вне видимой части
    HDC dc = GetDC(list_);
    if (!dc) return;
    DrawDottedFrame(dc, rc);
    ReleaseDC(list_, dc);
    static int logged = 0;   // временная диагностика этапа 4 — убрать после приёмки
    if (logged++ < 40) {
        LogDebug(L"рамка: overlay, панель " + std::to_wstring(side_) +
                 L", строка " + std::to_wstring(index));
    }
}

// ---------- дерево каталогов ----------

void Panel::CreateTree() {
    if (tree_) return;
    tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kTvId, g_inst, nullptr);
    if (tree_) {
        if (font_) SendMessageW(tree_, WM_SETFONT, (WPARAM)font_, TRUE);
        FillTreeRoot();
    }
}

HTREEITEM Panel::AddTreeItem(HTREEITEM parent, const std::wstring& path, const std::wstring& name) {
    treePaths_.push_back(path);

    TVINSERTSTRUCTW ins{};
    ins.hParent = parent ? parent : TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.pszText = (LPWSTR)name.c_str();
    ins.item.lParam = (LPARAM)(treePaths_.size() - 1);
    ins.item.cChildren = 1;      // рисуем «+», дети появятся при раскрытии
    return TreeView_InsertItem(tree_, &ins);
}

void Panel::FillTreeRoot() {
    if (!tree_) return;
    TreeView_DeleteAllItems(tree_);
    treePaths_.clear();

    for (auto& d : GetDrives()) {
        std::wstring root;
        root.push_back(d.letter);
        root += L":\\";
        std::wstring name = root;
        AddTreeItem(nullptr, root, name);
    }
}

// Дети узла: только каталоги, без скрытых и системных, с заглушкой для «+».
void Panel::PopulateTreeChildren(HTREEITEM item) {
    if (!tree_ || !item) return;

    // Уже заполнено? Проверяем первого ребёнка.
    HTREEITEM first = TreeView_GetChild(tree_, item);
    if (first) {
        TVITEMW tv{};
        tv.mask = TVIF_HANDLE | TVIF_CHILDREN;
        tv.hItem = first;
        if (TreeView_GetItem(tree_, &tv) && tv.lParam != 0) return;
    }

    const std::wstring dir = TreePathOf(item);
    if (dir.empty()) return;

    // Убираем заглушку
    while (HTREEITEM child = TreeView_GetChild(tree_, item)) TreeView_DeleteItem(tree_, child);

    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            AddTreeItem(item, JoinPath(dir, data.cFileName), data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
}

std::wstring Panel::TreePathOf(HTREEITEM item) const {
    if (!tree_ || !item) return {};
    TVITEMW tv{};
    tv.mask = TVIF_HANDLE | TVIF_PARAM;
    tv.hItem = item;
    if (!TreeView_GetItem(tree_, &tv)) return {};
    const size_t index = (size_t)tv.lParam;
    if (index >= treePaths_.size()) return {};
    return treePaths_[index];
}

// Раскрываем дерево до текущего каталога панели.
void Panel::SyncTreeToPath() {
    if (ftp_ || !tree_ || !treeVisible_ || path_.empty()) return;

    std::wstring current = path_;
    std::vector<std::wstring> chain;
    while (current.size() > 3) {
        chain.push_back(current);
        current = ParentPath(current);
        if (current.empty()) break;
    }
    if (current.size() == 3) chain.push_back(current);

    HTREEITEM parent = TVI_ROOT;
    HTREEITEM match = nullptr;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        HTREEITEM child = TreeView_GetChild(tree_, parent);
        HTREEITEM found = nullptr;
        while (child) {
            if (SamePath(TreePathOf(child), *it)) {
                found = child;
                break;
            }
            child = TreeView_GetNextSibling(tree_, child);
        }
        if (!found) break;
        TreeView_Expand(tree_, found, TVE_EXPAND);
        PopulateTreeChildren(found);
        match = found;
        parent = found;
    }
    if (match) TreeView_SelectItem(tree_, match);
}

void Panel::ToggleTree() {
    treeVisible_ = !treeVisible_;
    if (treeVisible_) {
        CreateTree();
        if (tree_) ShowWindow(tree_, SW_SHOW);
        SyncTreeToPath();
    } else if (tree_) {
        ShowWindow(tree_, SW_HIDE);
    }
    Layout();
}

// Контекстное меню Проводника для текущей записи (IContextMenu оболочки).
void Panel::ShowShellContextMenu(int screenX, int screenY) {
    if (ftp_) {
        MessageBoxW(GetParent(hwnd_), L"Контекстное меню Проводника для FTP недоступно.", L"FTP",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }
    Entry* e = Cursor();
    if (!e || !list_) return;

    const std::wstring full = e->isUp ? path_ : JoinPath(path_, e->name);

    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(full.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) return;

    IContextMenu* menu = nullptr;
    if (FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu))) || !menu) {
        item->Release();
        return;
    }

    HMENU popup = CreatePopupMenu();
    const UINT kFirst = 1;
    const UINT kLast = 0x6FFF;
    bool shown = false;

    if (popup && SUCCEEDED(menu->QueryContextMenu(popup, 0, kFirst, kLast, CMF_NORMAL))) {
        int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenX, screenY, 0, hwnd_, nullptr);
        if (cmd > 0) {
            CMINVOKECOMMANDINFOEX info{};
            info.cbSize = sizeof(info);
            info.hwnd = hwnd_;
            info.lpVerb = MAKEINTRESOURCEA((UINT)(cmd - kFirst));
            info.lpVerbW = MAKEINTRESOURCEW((UINT)(cmd - kFirst));
            info.nShow = SW_SHOWNORMAL;
            info.fMask = CMIC_MASK_UNICODE;
            shown = SUCCEEDED(menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info)));
        }
    }

    if (popup) DestroyMenu(popup);
    menu->Release();
    item->Release();

    // Команда оболочки могла изменить файлы — обновляем список.
    if (shown) Reload();
}

// Размер каталога считаем в фоне: рекурсия по большому дереву иначе подвесила бы окно.
void Panel::CalculateDirSize(int index) {
    if (ftp_ || inArchive_) {
        MessageBoxW(GetParent(hwnd_), L"Размер каталога здесь не считается.", L"Размер", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (!e.isDir || e.isUp || e.dirSizeKnown || e.dirSizeBusy) return;

    const std::wstring target = JoinPath(path_, e.name);
    const std::wstring name = e.name;
    e.dirSizeBusy = true;
    ++dirSizeActive_;
    InvalidateRect(list_, nullptr, FALSE);

    const int item = index;
    HWND self = hwnd_;
    std::thread([this, target, name, item, self]() {
        const ULONGLONG size = DirectorySize(target);
        {
            std::lock_guard<std::mutex> lock(dirSizeMutex_);
            dirSizeFinished_[item] = DirSizeResult{name, target, size};
        }
        PostMessageW(self, WM_APP + 500, (WPARAM)item, 0);
    }).detach();
}

// Отмеченные папки считаем в фоне, но не больше четырёх одновременно:
// иначе Ctrl+A на каталоге с сотнями папок поднял бы сотни потоков.
void Panel::StartMarkedDirSizes() {
    if (ftp_ || inArchive_) return;
    const int kMaxActive = 4;
    for (int i = 0; i < (int)entries_.size() && dirSizeActive_ < kMaxActive; ++i) {
        Entry& e = entries_[i];
        if (e.isUp || !e.isDir || !e.marked) continue;
        if (e.dirSizeKnown || e.dirSizeBusy) continue;
        CalculateDirSize(i);
    }
}

// Клик по списку: запоминаем, был ли зажат Shift/Ctrl — по ним отмечаем диапазон или одну запись.
LRESULT CALLBACK Panel::ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    if (self && msg == WM_LBUTTONDOWN) {
        self->clickShift_ = (wParam & MK_SHIFT) != 0;
        self->clickCtrl_ = (wParam & MK_CONTROL) != 0;
    }
    // Фокус пришёл списку неактивной панели (клик мышью) — активируем её,
    // как в TC. Активацию согласовываем через главное окно (WM_APP+303),
    // чтобы снялась активность другой панели.
    if (self && msg == WM_SETFOCUS && !self->active_) {
        PostMessageW(GetParent(self->hwnd_), WM_APP + 303, (WPARAM)self->side_, 0);
    }
    // Рамка курсора неактивной панели: рисуем ПОСЛЕ штатной отрисовки списка,
    // не полагаясь на стадии custom draw (основной путь — PaintCursorOverlay).
    if (self && msg == WM_PAINT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        self->PaintCursorOverlay();
        return result;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Поле пути: Enter — переход, Esc — отмена, F4/Alt+↓ — история, F-клавиши и стрелки отдаём панели.
LRESULT CALLBACK Panel::PathEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    switch (msg) {
        case WM_SETFOCUS:
            // Важно: не возвращаем 0, иначе поле не получит фокус по-настоящему и не покажет каретку.
            // Активацию панели делаем через главное окно (WM_APP+303): прямой SetActive(true)
            // оставлял бы вторую панель тоже «активной» (заголовок и курсор).
            if (self && !self->active_) {
                PostMessageW(GetParent(self->hwnd_), WM_APP + 303, (WPARAM)self->side_, 0);
            }
            break;

        case WM_LBUTTONDOWN: {
            if (!self) break;
            if (self->InFtp()) {
                // URL-путь не разбирается по компонентам: клик сразу включает правку.
                self->EnterPathEdit();
                break;
            }
            if (self->PathEditing()) break;          // в правке работает обычное поведение поля
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (self->PathClickInEmptyArea(x)) {
                self->EnterPathEdit();      // свободное место: включаем правку…
                break;                      // …и отдаём клик полю, как обычному EDIT
            }
            // Переход по компоненту откладываем на полтаймера двойного клика: если это начало
            // двойного клика, переход отменяется и поле уходит в правку.
            self->pendingPathChar_ = self->PathCharIndexAt(x, y);
            SetTimer(hwnd, kPathClickTimer, (UINT)(GetDoubleClickTime() / 2), nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kHoverShowTimer) {
                KillTimer(hwnd, kHoverShowTimer);
                if (self && !self->PathEditing()) self->ShowPathHover();
                return 0;
            }
            if (wParam == kHoverCascadeTimer) {
                KillTimer(hwnd, kHoverCascadeTimer);
                if (!self || self->closingHover_) return 0;
                if (self->cascadePendingLevel_ < 0 ||
                    self->cascadePendingLevel_ >= (int)self->hover_.size()) return 0;
                HoverLevel& lv = self->hover_[self->cascadePendingLevel_];
                if (self->cascadePendingItem_ < 0 || self->cascadePendingItem_ >= (int)lv.items.size()) return 0;
                if (lv.hoverItem != self->cascadePendingItem_) return 0;   // мышь ушла с пункта

                const std::wstring child = JoinPath(lv.dir, lv.items[self->cascadePendingItem_].name);
                self->CloseLevelsFrom((size_t)self->cascadePendingLevel_ + 1);

                RECT pr{};
                GetWindowRect(lv.win, &pr);
                RECT ir{};
                ListView_GetItemRect(lv.list, self->cascadePendingItem_, &ir, LVIR_BOUNDS);
                const int x = pr.right - Scale(6, self->dpi_);
                const int y = pr.top + ir.top + Scale(2, self->dpi_);
                self->OpenLevel(child, x, y);          // нет подкаталогов — уровень не откроется
                return 0;
            }
            if (wParam == kHoverHideTimer) {
                KillTimer(hwnd, kHoverHideTimer);
                if (!self) return 0;
                // Если курсор над любым уровнем каскада — не скрываем, ждём дальше.
                POINT pt{};
                GetCursorPos(&pt);
                bool over = false;
                for (const auto& lv : self->hover_) {
                    if (!lv.win || !IsWindowVisible(lv.win)) continue;
                    RECT rc{};
                    GetWindowRect(lv.win, &rc);
                    if (PtInRect(&rc, pt)) {
                        over = true;
                        break;
                    }
                }
                if (over) {
                    SetTimer(self->pathEdit_, kHoverHideTimer, 200, nullptr);
                    return 0;
                }
                self->HidePathHover();
                return 0;
            }
            if (wParam == kPathClickTimer) {
                KillTimer(hwnd, kPathClickTimer);
                if (self && !self->PathEditing()) self->NavigatePathPart(self->pendingPathChar_);
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            if (!self) break;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            if (self->PathEditing()) {
                self->HidePathHover();
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            if (self->PathClickInEmptyArea(x)) {
                self->HidePathHover();
                break;
            }
            self->SchedulePathHover(self->PathCharIndexAt(x, GET_Y_LPARAM(lParam)));
            break;
        }

        case WM_MOUSELEAVE:
            if (self && self->pathEdit_) SetTimer(self->pathEdit_, kHoverHideTimer, 250, nullptr);
            break;

        case WM_LBUTTONDBLCLK:
            if (self) self->HidePathHover();
            KillTimer(hwnd, kPathClickTimer);   // отменяем отложенный переход
            if (self && !self->PathEditing()) {
                self->EnterPathEdit();
                break;                      // двойной клик обработает поле (выделение слова)
            }
            break;

        case WM_KILLFOCUS:
            if (self && self->PathEditing()) self->LeavePathEdit(true);
            break;

        case WM_KEYDOWN:
            if (!self) break;
            switch (wParam) {
                case VK_RETURN:
                    self->ApplyPathText();
                    return 0;
                case VK_ESCAPE:
                    self->LeavePathEdit(true);
                    self->FocusList();
                    return 0;
                case VK_F4:
                    self->ShowPathHistory();
                    return 0;
                case VK_DOWN:
                    if (GetKeyState(VK_MENU) & 0x8000) {
                        self->ShowPathHistory();
                        return 0;
                    }
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Panel::ProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Panel* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Panel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->Proc(hwnd, msg, wParam, lParam);
}

LRESULT Panel::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            Layout();
            return 0;

        case WM_CONTEXTMENU: {
            // Курсор под мышью (если ПКМ по элементу списка), иначе — на текущей записи.
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (x != -1 && y != -1 && list_) {
                POINT pt{x, y};
                ScreenToClient(list_, &pt);
                LVHITTESTINFO hit{};
                hit.pt = pt;
                const int index = ListView_HitTest(list_, &hit);
                if (index >= 0) SetCursorIndex(index);
            } else {
                RECT rc{};
                int index = CursorIndex();
                if (index >= 0 && ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS)) {
                    POINT pt{rc.left, rc.bottom};
                    ClientToScreen(list_, &pt);
                    x = pt.x;
                    y = pt.y;
                }
            }
            ShowShellContextMenu(x, y);
            return 0;
        }

        case WM_APP + 500: {
            const int index = (int)wParam;
            DirSizeResult result;
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(dirSizeMutex_);
                auto it = dirSizeFinished_.find(index);
                if (it != dirSizeFinished_.end()) {
                    result = it->second;
                    dirSizeFinished_.erase(it);
                    have = true;
                }
            }
            if (dirSizeActive_ > 0) --dirSizeActive_;

            if (have) {
                int targetIndex = -1;
                if (index >= 0 && index < (int)entries_.size()) {
                    const Entry& at = entries_[index];
                    if (SamePath(at.name, result.name) && SamePath(result.target, JoinPath(path_, at.name))) {
                        targetIndex = index;
                    }
                }
                // Сортировка могла переставить строки — ищем папку по имени и полному пути.
                if (targetIndex < 0) {
                    for (int i = 0; i < (int)entries_.size(); ++i) {
                        const Entry& candidate = entries_[i];
                        if (SamePath(candidate.name, result.name) &&
                            SamePath(result.target, JoinPath(path_, candidate.name))) {
                            targetIndex = i;
                            break;
                        }
                    }
                }
                if (targetIndex >= 0) {
                    Entry& e = entries_[targetIndex];
                    e.dirSizeBusy = false;
                    e.dirSizeKnown = true;
                    e.dirSize = result.size;
                    LogDebug(L"dirsize '" + e.name + L"' = " + std::to_wstring(e.dirSize) + L" байт");
                    if (list_) ListView_RedrawItems(list_, targetIndex, targetIndex);
                    UpdateStatus();
                }
            }
            StartMarkedDirSizes();
            return 0;
        }

        case WM_DRAWITEM: {
            auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON) {
                if (dis->CtlID >= kTabBase && dis->CtlID < kTabBase + 32) {
                    DrawTabButton(dis);
                    return TRUE;
                }
                if (dis->CtlID >= kDriveBase && dis->CtlID < kDriveBase + 26) {
                    DrawDriveButton(dis);
                    return TRUE;
                }
                if (dis->CtlID == kDriveRootId || dis->CtlID == kDriveUpId) {
                    DrawNavButton(dis);
                    return TRUE;
                }
                if (dis->CtlID == kPathApplyId || dis->CtlID == kPathFavId || dis->CtlID == kPathHistId ||
                    dis->CtlID == kMaskHistId) {
                    DrawPathButton(dis);
                    return TRUE;
                }
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == kPathApplyId) {
                ApplyPathText();
                return 0;
            }
            if (id == kPathFavId) {
                ShowPathFavorites();
                return 0;
            }
            if (id == kPathHistId) {
                ShowPathHistory();
                return 0;
            }
            if (id == kMaskHistId) {
                ShowMaskHistory();
                return 0;
            }
            if (id >= kTabBase && id < kTabBase + 32) {
                ActivateTab(id - kTabBase);
                FocusList();
                return 0;
            }
            if (id >= kDriveBase && id < kDriveBase + 26) {
                wchar_t root[4] = {(wchar_t)(L'A' + (id - kDriveBase)), L':', L'\\', 0};
                Navigate(root);
                FocusList();
                return 0;
            }
            if (id == kDriveRootId) {
                if (inArchive_) LoadArchiveListing(L"");   // корень архива
                else GoRoot();
                FocusList();
                return 0;
            }
            if (id == kDriveUpId) {
                GoUp();
                FocusList();
                return 0;
            }
            break;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            HWND child = (HWND)lParam;
            if (child == pathEdit_) {
                SetBkMode(dc, OPAQUE);
                SetTextColor(dc, RGB(0, 0, 0));
                if (pathEditing_) {
                    SetBkColor(dc, RGB(255, 255, 255));
                    return (LRESULT)GetStockObject(WHITE_BRUSH);
                }
                SetBkColor(dc, active_ ? kActiveHeaderBg : kInactiveHeaderBg);
                return (LRESULT)(active_ ? brActive_ : brInactive_);
            }
            if (child == maskEdit_) {
                // Поле маски всегда на белом фоне с чёрным текстом (макет: .pmask #fff).
                SetBkMode(dc, OPAQUE);
                SetTextColor(dc, RGB(0, 0, 0));
                SetBkColor(dc, RGB(255, 255, 255));
                return (LRESULT)GetStockObject(WHITE_BRUSH);
            }
            if (child == volume_ && msg == WM_CTLCOLORSTATIC) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(102, 102, 102));   // #666, как .dfree в макете
                return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
            }
            if (child == statusInfo_ && msg == WM_CTLCOLORSTATIC) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(90, 90, 90));       // приглушённая сводка справа статуса
                return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
            }
            if (msg == WM_CTLCOLOREDIT) return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
        }

        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr && tip_ && hdr->hwndFrom == tip_ && hdr->code == TTN_NEEDTEXTW) {
                auto tt = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                {
                    POINT pt{};
                    GetCursorPos(&pt);
                    LVHITTESTINFO ht{};
                    ht.pt = pt;
                    ScreenToClient(list_, &ht.pt);
                    const int hit = ListView_HitTest(list_, &ht);
                    if (hit >= 0 && hit < (int)entries_.size()) {
                        tipText_ = FullPathOf(entries_[hit]);
                        if (entries_[hit].isDir && !tipText_.empty() && tipText_.back() != L'\\') {
                            tipText_.push_back(L'\\');   // у каталогов путь с завершающей чертой, как в TC
                        }
                    } else {
                        tipText_ = DisplayPath();
                    }
                }
                tt->lpszText = tipText_.data();
                return 0;
            }
            if (hdr && hdr->code == HDN_ENDTRACKW) {
                SaveColumnWidths();
                return 0;
            }
            if (hdr && tree_ && hdr->hwndFrom == tree_) {
                if (hdr->code == TVN_SELCHANGEDW) {
                    auto* tv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                    std::wstring target = TreePathOf(tv->itemNew.hItem);
                    LogDebug(L"tree select '" + target + L"' current='" + path_ + L"'");
                    if (!target.empty() && !SamePath(target, path_)) Navigate(target);
                } else if (hdr->code == TVN_ITEMEXPANDINGW) {
                    auto* tv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                    PopulateTreeChildren(tv->itemNew.hItem);
                }
                return 0;
            }
            if (!hdr || hdr->hwndFrom != list_) break;

            switch (hdr->code) {
                case LVN_BEGINDRAG:
                    // Список начал перетаскивание: решение принимает главное окно.
                    PostMessageW(GetParent(hwnd_), WM_APP + 300, (WPARAM)side_, 0);
                    return 0;
                case LVN_GETDISPINFOW:
                    FillDispInfo(reinterpret_cast<NMLVDISPINFO*>(lParam));
                    return 0;

                case NM_CUSTOMDRAW: {
                    auto cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        DrawRow(cd);
                        return CDRF_NEWFONT | CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
                    }
                    if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                        DrawRow(cd);
                        return CDRF_NEWFONT;
                    }
                    if ((cd->nmcd.dwDrawStage & CDDS_ITEMPOSTPAINT) == CDDS_ITEMPOSTPAINT) {
                        // Пост-отрисовка строки. Сравнение по маске: при включённых
                        // subitem-уведомлениях стадия может прийти с флагом CDDS_SUBITEM.
                        // Имя в первой колонке ListView рисует своим цветом, поэтому перерисовываем его сами.
                        DrawRowName(cd);
                        // Неактивная панель: пунктирная рамка вокруг строки курсора.
                        DrawCursorFrame(cd);
                        return CDRF_DODEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }

                case LVN_ITEMCHANGED: {
                    // Клик с Shift отмечает диапазон, с Ctrl — переключает отметку одной записи (как в TC).
                    auto info = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((info->uChanged & LVIF_STATE) && (info->uNewState & LVIS_SELECTED) &&
                        !(info->uOldState & LVIS_SELECTED)) {
                        if (!programmaticCursor_ && clickShift_ && lastCursor_ >= 0 && lastCursor_ != info->iItem) {
                            MarkRange(lastCursor_, info->iItem);
                        } else if (!programmaticCursor_ && clickCtrl_) {
                            ToggleMarkAt(info->iItem);
                        }
                        clickShift_ = clickCtrl_ = false;
                        lastCursor_ = info->iItem;
                    }
                    UpdateStatus();
                    PostMessageW(GetParent(hwnd_), WM_APP + 301, (WPARAM)side_, 0);
                    return 0;
                }

                case LVN_COLUMNCLICK: {
                    auto info = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if (sortColumn_ == info->iSubItem) {
                        sortReverse_ = !sortReverse_;
                    } else {
                        sortColumn_ = info->iSubItem;
                        sortReverse_ = (sortColumn_ == 2 || sortColumn_ == 3);
                    }
                    Sort(sortColumn_, sortReverse_);
                    UpdateSortArrows();
                    return 0;
                }

                case NM_DBLCLK: {
                    // Двойной щелчок: попросить главное окно открыть элемент.
                    auto info = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    if (info->iItem >= 0) SetCursorIndex(info->iItem);
                    PostMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(0x8001, 0), (LPARAM)hwnd_);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }

        case WM_DESTROY:
            if (brActive_) DeleteObject(brActive_);
            if (brInactive_) DeleteObject(brInactive_);
            brActive_ = brInactive_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
