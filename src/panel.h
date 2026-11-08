// Панель файлов: заголовок с путём, список, строка состояния.
#pragma once

#include "common.h"
#include "util.h"

class Panel {
public:
    explicit Panel(int side) : side_(side) {}

    bool Create(HWND parent, int id);
    void SetFont(HFONT font, UINT dpi);
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
    void FocusList();

    int CursorIndex() const;
    void SetCursorIndex(int index);
    Entry* Cursor();
    const std::vector<Entry>& Entries() const { return entries_; }
    std::wstring FullPathOf(const Entry& e) const;
    std::wstring FocusedPath();
    std::vector<std::wstring> SelectedNames() const;
    std::vector<std::wstring> SelectedPaths() const;

    void ToggleMarkCursor();
    void MarkAll(bool mark, bool includingDirs = true);
    void InvertMarks();
    void ClearMarks();

    // Сохранение/восстановление состояния панели для настроек.
    void RestoreState(const std::vector<std::wstring>& tabs, int active);
    void CaptureState(std::vector<std::wstring>& tabs, int& active) const;

    // Быстрый поиск по буквам: перевод курсора на первую запись с этим префиксом.
    bool FocusPrefix(const std::wstring& prefix);

    // Дерево каталогов (Ctrl+F8) слева от списка.
    void ToggleTree();
    bool TreeVisible() const { return treeVisible_; }

    // Архив как папка: путь = файл архива + внутренний каталог.
    bool InArchive() const { return inArchive_; }
    const std::wstring& ArchiveFile() const { return archiveFile_; }
    const std::wstring& ArchiveInner() const { return archiveInner_; }
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
    int TabCount() const { return (int)tabs_.size(); }

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
    void RebuildTabBar();
    void UpdateTabText();
    void UpdateHeader();
    void UpdateStatus();
    void UpdateColumns(int width);
    void DrawRow(NMLVCUSTOMDRAW* cd);
    void FillDispInfo(NMLVDISPINFO* info);

    int side_ = 0;
    bool active_ = false;
    HWND hwnd_ = nullptr;
    HWND header_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND volume_ = nullptr;
    HWND tree_ = nullptr;
    bool treeVisible_ = false;
    std::vector<std::wstring> treePaths_;   // lParam узла = индекс в этом векторе
    std::vector<HWND> driveButtons_;
    std::vector<wchar_t> driveLetters_;
    std::vector<HWND> tabButtons_;
    std::vector<std::wstring> tabs_;
    int tab_ = 0;
    bool inArchive_ = false;
    std::wstring archiveFile_;
    std::wstring archiveInner_;     // пусто = корень архива
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
