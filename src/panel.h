// Панель файлов: заголовок с путём, список, строка состояния.
#pragma once

#include "common.h"

#include <atomic>
#include <mutex>
#include <set>
#include "util.h"

class Panel {
public:
    explicit Panel(int side) : side_(side) {}

    bool Create(HWND parent, int id);
    void SetFont(HFONT font, UINT dpi);
    void RebuildDriveBar();
    void Layout();
    void SetActive(bool active);
    bool Active() const { return active_; }

    bool Navigate(const std::wstring& dir, const std::wstring& focus = L"", bool keepCursor = false);
    void Reload();
    bool GoUp();
    bool GoRoot();

    const std::wstring& Path() const { return path_; }
    std::wstring NavigateError() const { return lastError_; }
    HWND Hwnd() const { return hwnd_; }
    HWND List() const { return list_; }
    HWND PathEdit() const { return pathEdit_; }
    bool PathEditing() const { return pathEditing_; }
    void CancelPathEdit() {
        if (!pathEditing_) return;
        LeavePathEdit(true);
        FocusList();
    }
    void ConfirmPathEdit() {
        if (!pathEditing_) return;
        ApplyPathText();
    }
    void FocusList();

    int CursorIndex() const;
    void SetCursorIndex(int index);
    Entry* Cursor();
    const std::vector<Entry>& Entries() const { return entries_; }
    std::wstring FullPathOf(const Entry& e) const;
    std::wstring FocusedPath();
    std::vector<std::wstring> SelectedNames() const;
    std::vector<std::wstring> SelectedPaths() const;

    // Маска файлов: «*.*» справа от поля пути, история последних 8 (этап 5 план-UI).
    HWND MaskEdit() const { return maskEdit_; }
    bool MaskEditing() const { return maskEditing_; }
    const std::wstring& Mask() const { return mask_; }
    void SetMask(const std::wstring& mask);       // применить и перечитать каталог
    void RestoreMask(const std::wstring& mask);   // выставить без перечитывания (при запуске)
    void ConfirmMaskEdit();
    void CancelMaskEdit();

    void ToggleMarkCursor();
    void MarkAll(bool mark, bool includingDirs = true);
    void InvertMarks();
    // Shift+стрелки: диапазон от якоря до index, с корректным сокращением назад.
    void ShiftMarkTo(int index);
    // Режим: 0 — отметить, 1 — снять, 2 — инвертировать (Num + / - / *).
    size_t MarkByMask(const std::wstring& maskText, int mode);
    void MarkRange(int from, int to);
    void ToggleMarkAt(int index);
    void ClearMarks();

    // Сохранение/восстановление состояния панели для настроек.
    void RestoreState(const std::vector<std::wstring>& tabs, int active);
    void CaptureState(std::vector<std::wstring>& tabs, int& active) const;

    // Быстрый поиск по буквам: перевод курсора на первую запись с этим префиксом.
    bool FocusPrefix(const std::wstring& prefix);

    // Дерево каталогов (Ctrl+F8) слева от списка.
    // Размер каталога под курсором считается в фоне.
    void CalculateDirSize(int index);
    // Запускает подсчёт для отмеченных папок (не более 4 одновременно).
    void StartMarkedDirSizes();

    void ToggleTree();
    bool TreeVisible() const { return treeVisible_; }

    // Архив как папка: путь = файл архива + внутренний каталог.
    bool InArchive() const { return inArchive_; }
    const std::wstring& ArchiveFile() const { return archiveFile_; }
    const std::wstring& ArchiveInner() const { return archiveInner_; }
    // FTP: путь панели — ftp://user@host[:port]/каталог.
    bool InFtp() const { return ftp_; }
    std::wstring DisplayPath() const;
    bool LoadArchiveListing(const std::wstring& focus);

    bool GoBack();
    bool GoForward();
    bool CanGoBack() const { return histPos_ > 0; }
    bool CanGoForward() const { return histPos_ >= 0 && histPos_ + 1 < (int)history_.size(); }

    void AddTab();
    void CloseTab();
    void ActivateTab(int index);
    void NextTab();
    void CloseTabAt(int index);
    void CloseAllTabs();
    void AddTabFor(const std::wstring& dir, const std::wstring& name);
    void DuplicateTab(int index);
    void ToggleTabLock(int index);
    void ShowTabMenu(int index, POINT pt);
    void DrawTabButton(DRAWITEMSTRUCT* dis);
    std::wstring TabTitle(int index) const;
    int TabIndexFromHwnd(HWND wnd) const;
    RECT TabCloseRect(int index) const;
    static LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    const std::vector<std::wstring>& TabTitles() const { return tabNames_; }
    const std::vector<bool>& TabLocks() const { return tabLocked_; }
    void SetTabMeta(const std::vector<std::wstring>& names, const std::vector<int>& locks);
    bool TabLocked(int index) const {
        return index >= 0 && index < (int)tabLocked_.size() && tabLocked_[index];
    }
    int TabCount() const { return (int)tabs_.size(); }
    int ActiveTab() const { return tab_; }

    void Sort(int column, bool reverse);
    int SortColumn() const { return sortColumn_; }
    bool SortReverse() const { return sortReverse_; }

    int Side() const { return side_; }
    UINT Dpi() const { return dpi_; }
    void SetShowHidden(bool value) { showHidden_ = value; }
    bool ShowHidden() const { return showHidden_; }

private:
    static LRESULT CALLBACK ProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);

    void ApplyEntries(const std::wstring& focus, int fallbackIndex);
    void ResetShiftMark();
    void ShowShellContextMenu(int screenX, int screenY);
    void CreateTree();
    void FillTreeRoot();
    HTREEITEM AddTreeItem(HTREEITEM parent, const std::wstring& path, const std::wstring& name);
    void PopulateTreeChildren(HTREEITEM item);
    void SyncTreeToPath();
    std::wstring TreePathOf(HTREEITEM item) const;
    void CreateDriveBar();
    void UpdateDriveBar();
    void UpdateVolume();
    void DrawNavButton(DRAWITEMSTRUCT* dis);
    void RebuildTabBar();
    void UpdateTabText();
    void UpdateHeader();
    void UpdateStatus();
    void UpdateColumns(int width);
    void UpdateSortArrows();
    void SaveColumnWidths();
    static LRESULT CALLBACK PathEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK MaskEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void EnterMaskEdit();
    void LeaveMaskEdit(bool restore);
    void ShowMaskHistory();
    void UpdateMaskField();
    void PushMaskHistory(const std::wstring& mask);
    void FilterByMask();
    void ApplyPathText();
    void EnterPathEdit();
    void NavigatePathPart(int charIndex);
    std::wstring PathPartTarget(int charIndex) const;
    int PathPartStart(int charIndex) const;
    // Выпадающий список содержимого каталога при наведении на компонент пути (как в TC).
    void SchedulePathHover(int charIndex);
    void ShowPathHover();
    void HidePathHover();
    void FillPathHover(const std::wstring& dir);
    static LRESULT CALLBACK HoverProc(HWND, UINT, WPARAM, LPARAM);
    // Каскад выпадающих списков папок: уровень 0 — от компонента пути, дальше — вложенные.
    struct HoverLevel {
        HWND win = nullptr;
        HWND list = nullptr;
        std::wstring dir;
        std::vector<Entry> items;      // только каталоги
        int hoverItem = -1;            // пункт под курсором
    };
    bool CreateLevelWindow(HoverLevel& lv);
    void FillLevel(HoverLevel& lv);
    int OpenLevel(const std::wstring& dir, int x, int y);
    void CloseLevelsFrom(size_t index);
    static LRESULT CALLBACK HoverListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    int PathCharIndexAt(int x, int y) const;
    bool PathClickInEmptyArea(int x) const;
    void LeavePathEdit(bool restore);
    void ShowPathHistory();
    void ShowPathFavorites();
    void DrawPathButton(DRAWITEMSTRUCT* dis);

    bool pathEditing_ = false;      // поле пути в режиме правки (мигает курсор)
    int pendingPathChar_ = 0;       // символ пути, по которому был клик (переход по таймеру)

    int lastCursor_ = 0;           // прошлая позиция курсора (для Shift+клик)
    bool programmaticCursor_ = false;   // курсор двигаем сами, а не мышью
    int cursorRow_ = -1;           // строка курсора: рамка неактивной панели рисуется по ней — LVIS_FOCUSED в списке без фокуса ненадёжен
    bool clickShift_ = false;      // Shift был зажат в момент клика
    bool clickCtrl_ = false;       // Ctrl был зажат в момент клика
    int shiftAnchor_ = -1;         // начало диапазона, отмеченного Shift+стрелками
    std::set<int> shiftMarked_;    // индексы, отмеченные этим диапазоном

    std::vector<HoverLevel> hover_;
    int hoverChars_ = -1;              // компонент пути под курсором
    int cascadePendingLevel_ = -1;     // уровень и пункт, на которых ждём открытия подуровня
    int cascadePendingItem_ = -1;
    bool closingHover_ = false;        // массовое закрытие уровней (защита от WM_DESTROY)

    HWND tip_ = nullptr;           // подсказка с полным путём
    std::wstring tipText_;         // текст подсказки (живёт до следующего запроса)

    HWND pathEdit_ = nullptr;      // редактируемое поле пути
    HWND pathApply_ = nullptr;     // ✓ — перейти по введённому пути
    HWND pathFav_ = nullptr;       // ★ — избранные каталоги
    HWND pathHist_ = nullptr;      // ▼ — история каталогов
    HWND maskEdit_ = nullptr;      // поле маски файлов («*.*»)
    HWND maskHist_ = nullptr;      // ▼ — история масок
    HWND statusInfo_ = nullptr;    // сводка «Отмечено: …» у правого края статуса
    bool maskEditing_ = false;
    std::wstring mask_ = L"*.*";

    void DrawRowName(NMLVCUSTOMDRAW* cd);
    void DrawRow(NMLVCUSTOMDRAW* cd);
    void DrawCursorFrame(NMLVCUSTOMDRAW* cd);
    void PaintCursorOverlay();   // пунктир поверх WM_PAINT списка — не зависит от стадий custom draw
    void FillDispInfo(NMLVDISPINFO* info);

    int side_ = 0;
    bool active_ = false;
    HWND hwnd_ = nullptr;
    HWND header_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND volume_ = nullptr;
    HWND rootBtn_ = nullptr;   // «\» — в корень диска/архива
    HWND upBtn_ = nullptr;     // «..» — на уровень вверх
    std::atomic<ULONGLONG> dirSizeResult_{0};
    int dirSizeIndex_ = -1;
    struct DirSizeResult {
        std::wstring name;
        std::wstring target;
        ULONGLONG size = 0;
    };
    std::mutex dirSizeMutex_;
    std::map<int, DirSizeResult> dirSizeFinished_;
    int dirSizeActive_ = 0;
    HWND tree_ = nullptr;
    bool treeVisible_ = false;
    std::vector<std::wstring> treePaths_;   // lParam узла = индекс в этом векторе
    void DrawDriveButton(DRAWITEMSTRUCT* dis);
    std::vector<HICON> driveIcons_;
    std::vector<int> driveIds_;            // ID кнопки: kDriveBase + буква (0..25)

    std::vector<HWND> driveButtons_;
    std::vector<wchar_t> driveLetters_;
    std::vector<HWND> tabButtons_;
    std::vector<std::wstring> tabNames_;    // пользовательские подписи вкладок (пусто — имя каталога)
    std::vector<bool> tabLocked_;           // заблокированные вкладки
    std::vector<std::wstring> tabs_;
    int tab_ = 0;
    bool inArchive_ = false;
    std::wstring archiveFile_;
    std::wstring archiveInner_;     // пусто = корень архива
    bool ftp_ = false;              // текущий каталог — FTP
    bool LoadFtpListing(const std::wstring& url, const std::wstring& focus);
    void PushHistoryPoint();
    std::vector<std::wstring> history_;
    int histPos_ = -1;
    bool navigatingHistory_ = false;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::wstring path_;
    std::wstring lastError_;
    std::vector<Entry> entries_;
    int sortColumn_ = 0;
    bool sortReverse_ = false;
    bool showHidden_ = true;

    HBRUSH brActive_ = nullptr;
    HBRUSH brInactive_ = nullptr;
};
