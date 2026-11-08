// Реализация панели.
#include "panel.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include "archive.h"
#include "settings.h"
#include "dialogs.h"

#include <thread>

namespace {
const wchar_t* kPanelClass = L"POSPanel2";
const int kHeaderId = 1;
const int kListId = 2;
const int kStatusId = 3;
const int kDriveBase = 100;   // 100..125 — кнопки дисков
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

    header_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT | SS_NOTIFY,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kHeaderId, g_inst, nullptr);
    if (header_) SetWindowSubclass(header_, HeaderProc, 1, (DWORD_PTR)this);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS,   // без LVS_NOSORTHEADER: клик по заголовку сортирует
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kListId, g_inst, nullptr);

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

    volume_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kVolumeId, g_inst, nullptr);

    CreateDriveBar();
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

// Кнопка диска: значок тома + буква; текущий диск — светлее и с рамкой.
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
    const bool isCurrent = towupper(current) == towupper(driveLetters_[index]);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH fill = CreateSolidBrush(isCurrent ? RGB(255, 255, 255) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);

    if (isCurrent || pressed) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    int textLeft = rc.left + Scale(3, dpi_);
    if (index < (int)driveIcons_.size() && driveIcons_[index]) {
        DrawIconEx(dc, rc.left + Scale(3, dpi_), (rc.top + rc.bottom) / 2 - Scale(8, dpi_),
                   driveIcons_[index], 16, 16, 0, nullptr, DI_NORMAL);
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
    if (header_) SendMessageW(header_, WM_SETFONT, (WPARAM)font, TRUE);
    if (list_) SendMessageW(list_, WM_SETFONT, (WPARAM)font, TRUE);
    if (status_) SendMessageW(status_, WM_SETFONT, (WPARAM)font, TRUE);
    if (volume_) SendMessageW(volume_, WM_SETFONT, (WPARAM)font, TRUE);
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

    HDWP dwp = BeginDeferWindowPos(5 + (int)driveButtons_.size() + (int)tabButtons_.size());
    int drivesRight = 0;
    if (dwp) {
        int x = 0;
        const int bw = Scale(44, dpi_);
        for (auto h : driveButtons_) {
            dwp = DeferWindowPos(dwp, h, nullptr, x, 1, bw, driveH - 2, SWP_NOZORDER);
            x += bw + Scale(1, dpi_);
        }
        drivesRight = x;
    }
    // «1,2 Тб свободно из 3,6 Тб» — справа от кнопок, в той же строке.
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

    if (dwp) dwp = DeferWindowPos(dwp, header_, nullptr, 0, y, width, headerH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, list_, nullptr, listX, y + headerH, listW, listH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, status_, nullptr, 0, y + headerH + listH, width, statusH, SWP_NOZORDER);
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
    if (inArchive_) {
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
    if (!header_) return;

    std::wstring full = DisplayPath();
    if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
    full += L"*.*";

    RECT rc{};
    GetClientRect(header_, &rc);
    int avail = (rc.right - rc.left) - Scale(8, dpi_);

    std::wstring text = full;
    if (avail > 0) {
        HDC dc = GetDC(header_);
        HGDIOBJ old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        auto widthOf = [&](const std::wstring& s) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
            return sz.cx;
        };

        if (widthOf(text) > avail) {
            // Отбрасываем ведущие компоненты пути, пока не влезем: "…\VSC\POSCommander2\*.*"
            std::wstring tail = full;
            for (int guard = 0; guard < 64; ++guard) {
                size_t pos = tail.find(L'\\');
                if (pos == std::wstring::npos) break;
                tail = tail.substr(pos + 1);
                std::wstring candidate = L"…\\" + tail;
                if (widthOf(candidate) <= avail) {
                    text = candidate;
                    break;
                }
                if (tail.find(L'\\') == std::wstring::npos) break;
            }
            if (widthOf(text) > avail) {
                std::wstring clipped = text;
                while (clipped.size() > 1 && widthOf(clipped + L"…") > avail) clipped.pop_back();
                text = clipped + L"…";
            }
        }
        SelectObject(dc, old);
        ReleaseDC(header_, dc);
    }

    SetWindowTextW(header_, text.c_str());
    InvalidateRect(header_, nullptr, TRUE);
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

    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });

    ApplyEntries(focus, 0);
    UpdateHeader();
    UpdateStatus();
    return true;
}

bool Panel::Navigate(const std::wstring& dir, const std::wstring& focus, bool keepCursor) {
    lastError_.clear();

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
    entries_ = std::move(loaded);

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
    if (!navigatingHistory_) {
        if (histPos_ >= 0 && histPos_ < (int)history_.size() && history_[histPos_] == path_) {
            // тот же каталог — точку не дублируем
        } else {
            history_.resize(histPos_ + 1);
            history_.push_back(path_);
            histPos_ = (int)history_.size() - 1;
        }
    }
    (void)previousPath;
    return true;
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
    std::wstring root = DriveRootOf(path_);
    if (SamePath(root, path_)) return false;
    return Navigate(root);
}

int Panel::CursorIndex() const {
    if (!list_) return -1;
    return ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
}

void Panel::SetCursorIndex(int index) {
    if (!list_) return;
    if (index < 0 || index >= (int)entries_.size()) index = 0;
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, index, FALSE);
}

Entry* Panel::Cursor() {
    int index = CursorIndex();
    if (index < 0 || index >= (int)entries_.size()) return nullptr;
    return &entries_[index];
}

std::wstring Panel::FullPathOf(const Entry& e) const {
    if (e.isUp) return ParentPath(path_);
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
}

void Panel::MarkAll(bool mark, bool includingDirs) {
    for (auto& e : entries_) {
        if (e.isUp) continue;
        if (!includingDirs && e.isDir) continue;
        e.marked = mark;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
}

void Panel::InvertMarks() {
    for (auto& e : entries_) {
        if (e.isUp) continue;
        e.marked = !e.marked;
    }
    InvalidateRect(list_, nullptr, FALSE);
    UpdateStatus();
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
    size_t files = 0, dirs = 0, marked = 0;
    ULONGLONG total = 0, markedSize = 0;
    for (auto& e : entries_) {
        if (e.isUp) continue;
        if (e.isDir) {
            dirs++;
        } else {
            files++;
            total += e.size;
        }
        if (e.marked) {
            marked++;
            if (!e.isDir) markedSize += e.size;
        }
    }

    wchar_t buf[512];
    if (marked) {
        swprintf(buf, 512, L"Отмечено: %zu · %ls   |   Файлов: %zu · %ls   |   Папок: %zu",
                 marked, FmtSize(markedSize).c_str(), files, FmtSize(total).c_str(), dirs);
    } else {
        swprintf(buf, 512, L"Файлов: %zu · %ls   |   Папок: %zu", files, FmtSize(total).c_str(), dirs);
    }
    // Слева — сведения о записи под курсором, как в TC, затем сводка.
    std::wstring head;
    if (Entry* cur = Cursor(); cur && !cur->isUp) {
        head = cur->name;
        head += L"   ·   ";
        head += cur->isDir ? L"<DIR>" : FmtSize(cur->size);
        const std::wstring date = FmtDate(cur->ft);
        if (!date.empty()) {
            head += L"   ·   ";
            head += date;
        }
        head += L"   ·   ";
        head += AttrMask(cur->attr);
    }

    const std::wstring text = head.empty() ? std::wstring(buf) : head + L"   |   " + buf;
    SetWindowTextW(status_, text.c_str());
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
                text = e.isUp ? L"<Папка>" : (e.isDir ? L"<Папка>" : Upper(e.ext()).substr(0, 8));
                if (!text.empty() && text[0] == L'.') text.erase(0, 1);
                break;
            case 2:
                if (e.isUp) {
                    text = L"<Папка>";
                } else if (e.isDir) {
                    text = e.dirSizeBusy ? L"…" : (e.dirSizeKnown ? FmtSize(e.dirSize) : L"<DIR>");
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
        info->item.iImage = IconForEntry(e, path_);
    }
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
    if (!tree_ || !treeVisible_ || path_.empty()) return;

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
    if (index < 0 || index >= (int)entries_.size()) return;
    Entry& e = entries_[index];
    if (!e.isDir || e.isUp) return;

    const std::wstring target = JoinPath(path_, e.name);
    e.dirSizeBusy = true;
    InvalidateRect(list_, nullptr, FALSE);

    const int item = index;
    HWND self = hwnd_;
    std::thread([this, target, item, self]() {
        const ULONGLONG size = DirectorySize(target);
        dirSizeResult_.store(size);
        dirSizeIndex_ = item;
        PostMessageW(self, WM_APP + 500, 0, 0);
    }).detach();
}

// Заголовок пути: клик по компоненту — переход, двойной клик — ввод пути (как в TC).
LRESULT CALLBACK Panel::HeaderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Panel*>(ref);
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;

        case WM_LBUTTONDOWN: {
            if (!self) break;
            const int x = GET_X_LPARAM(lParam);
            const DWORD now = GetTickCount();
            int dx = x - self->headerClickX_;
            if (dx < 0) dx = -dx;
            const bool dbl = self->headerClickX_ >= 0 &&
                             (int)(now - self->headerClickTick_) <= (int)GetDoubleClickTime() &&
                             dx <= GetSystemMetrics(SM_CXDOUBLECLK) / 2;
            self->headerClickTick_ = now;
            self->headerClickX_ = x;
            if (dbl) {
                self->headerClickX_ = -1;
                self->OnHeaderDblClick();
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (self) self->OnHeaderClick(GET_X_LPARAM(lParam));
            return 0;

        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void Panel::OnHeaderDblClick() {
    if (inArchive_) return;
    std::wstring value = DisplayPath();
    if (!InputBox(GetParent(header_), L"Переход", L"Путь:", value, true)) return;
    value = Trim(value);
    if (value.empty()) return;
    if (!Navigate(value)) {
        MessageBoxW(GetParent(header_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
    }
}

// Разбивает путь на компоненты: «E:\Work\VSC» → [E:, Work, VSC].
static std::vector<std::wstring> SplitPathParts(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring acc;
    for (size_t i = 0; i < path.size(); ++i) {
        const wchar_t ch = path[i];
        if (ch == L'\\') {
            if (!acc.empty()) parts.push_back(acc);
            acc.clear();
        } else {
            acc.push_back(ch);
        }
    }
    if (!acc.empty()) parts.push_back(acc);
    return parts;
}

void Panel::OnHeaderClick(int x) {
    if (inArchive_ || !header_) return;

    // Видимый текст без хвоста «\*.*»
    std::wstring shownText;
    {
        const int len = GetWindowTextLengthW(header_);
        shownText.resize(len);
        if (len > 0) GetWindowTextW(header_, &shownText[0], len + 1);
    }
    if (const size_t maskPos = shownText.find(L"*.*"); maskPos != std::wstring::npos) {
        shownText = shownText.substr(0, maskPos);
    }
    while (!shownText.empty() && shownText.back() == L'\\') shownText.pop_back();
    if (shownText.empty()) return;

    const std::vector<std::wstring> real = SplitPathParts(DisplayPath());
    const std::vector<std::wstring> shown = SplitPathParts(shownText);
    if (real.empty() || shown.empty()) return;

    // Какая видимая часть под курсором
    HDC dc = GetDC(header_);
    HGDIOBJ oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    int left = Scale(4, dpi_);
    int clicked = -1;
    for (size_t i = 0; i < shown.size(); ++i) {
        SIZE partSize{};
        GetTextExtentPoint32W(dc, shown[i].c_str(), (int)shown[i].size(), &partSize);
        if (x >= left && x < left + partSize.cx) {
            clicked = (int)i;
            break;
        }
        left += partSize.cx;
        SIZE sepSize{};
        GetTextExtentPoint32W(dc, L"\\", 1, &sepSize);
        left += sepSize.cx;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(header_, dc);
    if (clicked < 0) return;

    // Сопоставляем с конца: «…» съедает начало пути
    int base = (int)real.size() - (int)shown.size();
    if (base < 0) base = 0;
    const int index = base + clicked;
    if (index < 0 || index >= (int)real.size()) return;

    std::wstring target;
    for (int i = 0; i <= index; ++i) {
        target += real[i];
        if (i < index) target += L"\\";
    }
    if (index == 0 && real[0].size() == 2 && real[0][1] == L':') target += L"\\";

    if (!Navigate(target)) {
        MessageBoxW(GetParent(header_), NavigateError().c_str(), L"Переход", MB_ICONERROR);
    }
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
            const int index = dirSizeIndex_;
            if (index >= 0 && index < (int)entries_.size()) {
                Entry& e = entries_[index];
                e.dirSizeBusy = false;
                e.dirSizeKnown = true;
                e.dirSize = dirSizeResult_.load();
                LogDebug(L"dirsize '" + e.name + L"' = " + std::to_wstring(e.dirSize) + L" байт");
                if (list_) ListView_RedrawItems(list_, index, index);
                UpdateStatus();
            }
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
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
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
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            HWND child = (HWND)lParam;
            if (child == header_) {
                SetBkMode(dc, OPAQUE);
                SetTextColor(dc, RGB(0, 0, 0));
                SetBkColor(dc, active_ ? kActiveHeaderBg : kInactiveHeaderBg);
                return (LRESULT)(active_ ? brActive_ : brInactive_);
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
        }

        case WM_NOTIFY: {
            auto hdr = reinterpret_cast<NMHDR*>(lParam);
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
                        return CDRF_NEWFONT;
                    }
                    return CDRF_DODEFAULT;
                }

                case LVN_ITEMCHANGED: {
                    // Курсор переехал — обновляем строку состояния и быстрый просмотр.
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
