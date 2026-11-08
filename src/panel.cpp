// Реализация панели.
#include "panel.h"

#include <commctrl.h>

namespace {
const wchar_t* kPanelClass = L"POSPanel2";
const int kHeaderId = 1;
const int kListId = 2;
const int kStatusId = 3;
const int kDriveBase = 100;   // 100..125 — кнопки дисков
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

    header_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kHeaderId, g_inst, nullptr);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kListId, g_inst, nullptr);

    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetImageList(list_, ShellSmallImageList(), LVSIL_SMALL);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 240;
    col.iSubItem = 0;
    col.pszText = (LPWSTR)L"Имя";
    ListView_InsertColumn(list_, 0, &col);
    col.cx = 70; col.iSubItem = 1; col.pszText = (LPWSTR)L"Тип";
    ListView_InsertColumn(list_, 1, &col);
    col.cx = 100; col.iSubItem = 2; col.pszText = (LPWSTR)L"Размер";
    ListView_InsertColumn(list_, 2, &col);
    col.cx = 120; col.iSubItem = 3; col.pszText = (LPWSTR)L"Дата";
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
        wchar_t letter = (wchar_t)(L'A' + i);
        wchar_t text[4] = {letter, L':', 0, 0};
        HWND btn = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(kDriveBase + i), g_inst, nullptr);
        if (btn) {
            driveButtons_.push_back(btn);
            driveLetters_.push_back(letter);
        }
    }
    UpdateDriveBar();
}

// Текущий диск выделяется стилем кнопки по умолчанию.
void Panel::UpdateDriveBar() {
    if (driveButtons_.empty()) return;
    wchar_t current = path_.empty() ? 0 : path_[0];
    for (size_t i = 0; i < driveButtons_.size(); ++i) {
        bool isCurrent = (towupper(current) == towupper(driveLetters_[i]));
        LONG style = GetWindowLongW(driveButtons_[i], GWL_STYLE);
        LONG want = isCurrent ? (style | BS_DEFPUSHBUTTON) : (style & ~BS_DEFPUSHBUTTON);
        if (want != style) {
            SetWindowLongW(driveButtons_[i], GWL_STYLE, want);
            InvalidateRect(driveButtons_[i], nullptr, TRUE);
        }
    }
}

// ---------- вкладки ----------

void Panel::RebuildTabBar() {
    for (auto h : tabButtons_) DestroyWindow(h);
    tabButtons_.clear();
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::wstring title = LeafOf(tabs_[i]);
        HWND btn = CreateWindowExW(0, L"BUTTON", title.c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(kTabBase + (int)i), g_inst, nullptr);
        if (btn) {
            if (font_) SendMessageW(btn, WM_SETFONT, (WPARAM)font_, TRUE);
            tabButtons_.push_back(btn);
        }
    }
    Layout();
}

void Panel::UpdateTabText() {
    if (tab_ < 0 || tab_ >= (int)tabButtons_.size() || tab_ >= (int)tabs_.size()) return;
    SetWindowTextW(tabButtons_[tab_], LeafOf(tabs_[tab_]).c_str());
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
    } else if (active >= 0 && active < (int)tabs.size()) {
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

    int driveH = Scale(24, dpi_);
    int volH = Scale(18, dpi_);
    int tabH = Scale(24, dpi_);
    int headerH = Scale(24, dpi_);
    int statusH = Scale(22, dpi_);
    int listH = height - driveH - volH - tabH - headerH - statusH;
    if (listH < 0) listH = 0;

    HDWP dwp = BeginDeferWindowPos(5 + (int)driveButtons_.size() + (int)tabButtons_.size());
    if (dwp) {
        int x = 0;
        int bw = Scale(26, dpi_);
        for (auto h : driveButtons_) {
            dwp = DeferWindowPos(dwp, h, nullptr, x, 1, bw, driveH - 3, SWP_NOZORDER);
            x += bw + Scale(1, dpi_);
        }
    }
    if (dwp) dwp = DeferWindowPos(dwp, volume_, nullptr, 0, driveH, width, volH, SWP_NOZORDER);
    if (dwp) {
        int x = 0;
        int tw = Scale(120, dpi_);
        for (auto h : tabButtons_) {
            dwp = DeferWindowPos(dwp, h, nullptr, x, driveH + volH + 1, tw, tabH - 3, SWP_NOZORDER);
            x += tw + Scale(1, dpi_);
        }
    }
    int y = driveH + volH + tabH;
    if (dwp) dwp = DeferWindowPos(dwp, header_, nullptr, 0, y, width, headerH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, list_, nullptr, 0, y + headerH, width, listH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, status_, nullptr, 0, y + headerH + listH, width, statusH, SWP_NOZORDER);
    if (dwp) EndDeferWindowPos(dwp);

    UpdateColumns(width);
    UpdateHeader();   // ширина заголовка известна только после раскладки
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void Panel::UpdateColumns(int width) {
    if (!list_) return;
    int extW = Scale(70, dpi_);
    int sizeW = Scale(110, dpi_);
    int dateW = Scale(130, dpi_);
    int nameW = width - extW - sizeW - dateW - Scale(24, dpi_);
    if (nameW < Scale(90, dpi_)) nameW = Scale(90, dpi_);
    ListView_SetColumnWidth(list_, 0, nameW);
    ListView_SetColumnWidth(list_, 1, extW);
    ListView_SetColumnWidth(list_, 2, sizeW);
    ListView_SetColumnWidth(list_, 3, dateW);
}

void Panel::SetActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    UpdateHeader();
}

// Строка под панелью дисков: сколько свободно на текущем томе.
void Panel::UpdateVolume() {
    if (!volume_) return;
    SetWindowTextW(volume_, VolumeSummary(path_).c_str());
}

void Panel::UpdateHeader() {
    if (!header_) return;

    std::wstring full = path_;
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

bool Panel::Navigate(const std::wstring& dir, const std::wstring& focus, bool keepCursor) {
    lastError_.clear();
    std::wstring target = NormalizeDir(dir);
    if (target.empty()) {
        lastError_ = L"Пустой путь";
        return false;
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
        up.icon = IconForEntry(up);
        entries_.insert(entries_.begin(), std::move(up));
    }

    std::sort(entries_.begin(), entries_.end(),
              [this](const Entry& a, const Entry& b) { return CmpEntry(a, b, sortColumn_, sortReverse_); });

    ULONGLONG t2 = GetTickCount64();
    ApplyEntries(focus, fallback);
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
    UpdateVolume();
    if (tabs_.empty()) {
        tabs_.push_back(path_);
        tab_ = 0;
        RebuildTabBar();
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
    // Свободное место показывается отдельной строкой под дисками (см. UpdateVolume).
    SetWindowTextW(status_, buf);
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
                text = e.isDir ? L"<Папка>" : FmtSize(e.size);
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
        info->item.iImage = IconForEntry(e);
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
            if (!hdr || hdr->hwndFrom != list_) break;

            switch (hdr->code) {
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

                case LVN_COLUMNCLICK: {
                    auto info = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if (sortColumn_ == info->iSubItem) {
                        sortReverse_ = !sortReverse_;
                    } else {
                        sortColumn_ = info->iSubItem;
                        sortReverse_ = (sortColumn_ == 2 || sortColumn_ == 3);
                    }
                    Sort(sortColumn_, sortReverse_);
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
