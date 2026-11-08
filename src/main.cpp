// Главное окно POSCommander 2: две панели, меню, командная строка, F-клавиши.
#include "common.h"
#include "dialogs.h"
#include "panel.h"
#include "util.h"
#include "ops.h"
#include "selftest.h"
#include "settings.h"

#include <windowsx.h>

#include <cstdio>

HINSTANCE g_inst = nullptr;

namespace {

const wchar_t* kMainClass = L"POSCommander2Main";
const wchar_t* kTitle = L"POSCommander 2";

enum : int {
    IDM_VIEW = 100,
    IDM_EDIT,
    IDM_COPY,
    IDM_MOVE,
    IDM_RENAME,
    IDM_MKDIR,
    IDM_DELETE,
    IDM_PROPERTIES,
    IDM_EXIT,
    IDM_NAV_BACK, IDM_NAV_FORWARD, IDM_NAV_UP,

    IDM_SEL_ALL = 120,
    IDM_SEL_NONE,
    IDM_SEL_INVERT,
    IDM_SEL_MASK,

    IDM_SORT_NAME = 140,
    IDM_SORT_EXT,
    IDM_SORT_SIZE,
    IDM_SORT_DATE,
    IDM_REVERSE,

    IDM_REFRESH = 160,
    IDM_HIDDEN,
    IDM_SWAP,
    IDM_CONSOLE,
    IDM_EXPLORER,
    IDM_DRIVE_LEFT,
    IDM_DRIVE_RIGHT,
    IDM_ABOUT,
};

const int ID_PANEL_LEFT = 1000;
const int ID_PANEL_RIGHT = 1001;
const int ID_PROMPT_LABEL = 1002;
const int ID_PROMPT = 1003;
const int ID_FKEY_FIRST = 1010;
const int ID_OPEN_ITEM = 0x8001;

struct FKeyDef {
    const wchar_t* text;
    int command;
};

const FKeyDef kFKeys[] = {
    {L"F3 Просмотр", IDM_VIEW},
    {L"F4 Правка", IDM_EDIT},
    {L"F5 Копирование", IDM_COPY},
    {L"F6 Перемещение", IDM_MOVE},
    {L"F7 Каталог", IDM_MKDIR},
    {L"F8 Удаление", IDM_DELETE},
    {L"Alt+F4 Выход", IDM_EXIT},
};
const int kFKeyCount = (int)(sizeof(kFKeys) / sizeof(kFKeys[0]));

// Тулбар: кнопки и их команды.
const int ID_TB_FIRST = 1100;
struct TbDef { const wchar_t* text; int command; };
const TbDef kToolbar[] = {
    {L"Обновить", IDM_REFRESH},   {L"Назад", IDM_NAV_BACK},   {L"Вперёд", IDM_NAV_FORWARD},
    {L"Вверх", IDM_NAV_UP},       {L"Копия", IDM_COPY},       {L"Перенос", IDM_MOVE},
    {L"Каталог", IDM_MKDIR},      {L"Удалить", IDM_DELETE},   {L"Свойства", IDM_PROPERTIES},
};
const int kToolbarCount = (int)(sizeof(kToolbar) / sizeof(kToolbar[0]));

class App {
public:
    bool Create(HINSTANCE inst);
    int Run();

private:
    static LRESULT CALLBACK ProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);
    bool PreTranslate(const MSG& msg);

    void CreateChildren();
    void LoadPanels();
    void BuildMenu();
    void Layout();
    void ApplyFont();
    HFONT MakeUiFont();
    void SetActivePanel(int index);
    Panel* ActivePanel();
    Panel* OtherPanel();
    void Dispatch(int id);
    void UpdatePromptLabel();
    void OpenCursor();
    void GoUpActive();
    void DoCopyMove(bool move);
    void DoRename();
    void DoMkdir();
    void DoDelete();
    void DoProperties();
    void DoSort(int column);
    void ShowDrivesMenu(int side);
    void RunCommandLine();
    void AppendToPrompt(const std::wstring& text, bool quoted);
    void ShowError(const std::wstring& text);
    int SplitX() const;

    HWND hwnd_ = nullptr;
    HMENU menu_ = nullptr;
    HWND label_ = nullptr;
    HWND prompt_ = nullptr;
    HWND fkeys_[kFKeyCount]{};
    HWND toolbar_[kToolbarCount]{};
    Panel left_{0};
    Panel right_{1};
    int activeSide_ = 0;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    bool showHidden_ = true;
    int splitPercent_ = 50;
    bool draggingSplit_ = false;
    std::vector<std::wstring> history_;
};

// ---------- создание ----------

bool App::Create(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return false;

    BuildMenu();
    const Settings& saved = GetSettings();
    int wx = saved.hasPosition ? saved.x : CW_USEDEFAULT;
    int wy = saved.hasPosition ? saved.y : CW_USEDEFAULT;
    hwnd_ = CreateWindowExW(0, kMainClass, kTitle, WS_OVERLAPPEDWINDOW, wx, wy, saved.w, saved.h,
                            nullptr, menu_, inst, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, saved.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void App::BuildMenu() {
    menu_ = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_VIEW, L"&Просмотр\tF3");
    AppendMenuW(file, MF_STRING, IDM_EDIT, L"&Правка\tF4");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_COPY, L"&Копировать\tF5");
    AppendMenuW(file, MF_STRING, IDM_MOVE, L"&Переместить\tF6");
    AppendMenuW(file, MF_STRING, IDM_RENAME, L"Пере&именовать\tF2");
    AppendMenuW(file, MF_STRING, IDM_MKDIR, L"Создать &каталог\tF7");
    AppendMenuW(file, MF_STRING, IDM_DELETE, L"&Удалить\tF8");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PROPERTIES, L"С&войства\tAlt+Enter");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"В&ыход\tAlt+F4");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)file, L"&Файл");

    HMENU sel = CreatePopupMenu();
    AppendMenuW(sel, MF_STRING, IDM_SEL_ALL, L"Выделить &всё\tCtrl+A");
    AppendMenuW(sel, MF_STRING, IDM_SEL_NONE, L"&Снять выделение");
    AppendMenuW(sel, MF_STRING, IDM_SEL_INVERT, L"&Инвертировать");
    AppendMenuW(sel, MF_STRING, IDM_SEL_MASK, L"По &маске…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)sel, L"В&ыделение");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_SORT_NAME, L"По &имени\tCtrl+F3");
    AppendMenuW(view, MF_STRING, IDM_SORT_EXT, L"По &типу\tCtrl+F4");
    AppendMenuW(view, MF_STRING, IDM_SORT_SIZE, L"По &размеру\tCtrl+F5");
    AppendMenuW(view, MF_STRING, IDM_SORT_DATE, L"По &дате\tCtrl+F6");
    AppendMenuW(view, MF_STRING, IDM_REVERSE, L"Обратный &порядок");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_HIDDEN, L"Скрытые &файлы\tCtrl+H");
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Обновить\tCtrl+R");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)view, L"&Вид");

    HMENU cmd = CreatePopupMenu();
    AppendMenuW(cmd, MF_STRING, IDM_SWAP, L"С&менить панель\tCtrl+U");
    AppendMenuW(cmd, MF_STRING, IDM_CONSOLE, L"Командная &строка");
    AppendMenuW(cmd, MF_STRING, IDM_EXPLORER, L"&Проводник");
    AppendMenuW(cmd, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(cmd, MF_STRING, IDM_DRIVE_LEFT, L"Диск &левой панели\tAlt+F1");
    AppendMenuW(cmd, MF_STRING, IDM_DRIVE_RIGHT, L"Диск &правой панели\tAlt+F2");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)cmd, L"&Команды");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&О программе");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)help, L"&Помощь");
}

HFONT App::MakeUiFont() {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, (int)dpi_, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

void App::CreateChildren() {
    dpi_ = WindowDpi(hwnd_);
    font_ = MakeUiFont();
    InitShellIcons();

    left_.Create(hwnd_, ID_PANEL_LEFT);
    right_.Create(hwnd_, ID_PANEL_RIGHT);

    label_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE, 0, 0, 10, 10,
                             hwnd_, (HMENU)(INT_PTR)ID_PROMPT_LABEL, g_inst, nullptr);

    prompt_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)ID_PROMPT, g_inst, nullptr);

    for (int i = 0; i < kToolbarCount; i++) {
        toolbar_[i] = CreateWindowExW(0, L"BUTTON", kToolbar[i].text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT, 0, 0, 10, 10,
                                      hwnd_, (HMENU)(INT_PTR)(ID_TB_FIRST + i), g_inst, nullptr);
    }

    for (int i = 0; i < kFKeyCount; i++) {
        fkeys_[i] = CreateWindowExW(0, L"BUTTON", kFKeys[i].text,
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT, 0, 0, 10, 10,
                                    hwnd_, (HMENU)(INT_PTR)(ID_FKEY_FIRST + i), g_inst, nullptr);
    }

    ApplyFont();

    const Settings& st = GetSettings();
    showHidden_ = st.showHidden;
    splitPercent_ = st.splitPercent;

    left_.SetShowHidden(showHidden_);
    right_.SetShowHidden(showHidden_);
    left_.Sort(st.sortColumn, st.sortReverse);
    right_.Sort(st.sortColumn, st.sortReverse);

    left_.SetActive(true);
    SetActivePanel(0);
    UpdatePromptLabel();

    // Каталоги читаем после первого показа окна: на 100 000 файлов это сотни миллисекунд,
    // и окно не должно их ждать.
    PostMessageW(hwnd_, WM_APP + 101, 0, 0);
}

// Отложенная загрузка панелей (после показа окна).
void App::LoadPanels() {
    const Settings& st = GetSettings();
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);

    if (!st.leftTabs.empty()) left_.RestoreState(st.leftTabs, st.leftActive);
    else left_.Navigate(cwd);

    if (!st.rightTabs.empty()) right_.RestoreState(st.rightTabs, st.rightActive);
    else right_.Navigate(cwd);

    ActivePanel()->FocusList();
    UpdatePromptLabel();
}

void App::ApplyFont() {
    if (!font_) return;
    left_.SetFont(font_, dpi_);
    right_.SetFont(font_, dpi_);
    if (label_) SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (prompt_) SendMessageW(prompt_, WM_SETFONT, (WPARAM)font_, TRUE);
    for (auto h : fkeys_) {
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);
    }
    for (auto h : toolbar_) {
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);
    }
}

// ---------- раскладка ----------

int App::SplitX() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return MulDiv(rc.right - rc.left, splitPercent_, 100);
}

void App::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    int gap = Scale(4, dpi_);
    int tbH = Scale(30, dpi_);
    int fkeyH = Scale(30, dpi_);
    int promptH = Scale(26, dpi_);
    int panelsY = tbH + gap;
    int panelsH = height - tbH - fkeyH - promptH - gap * 3;   // запас снизу, чтобы ряд F-клавиш не лип к краю
    if (panelsH < Scale(80, dpi_)) panelsH = Scale(80, dpi_);

    int leftW = SplitX() - gap / 2;
    if (leftW < Scale(120, dpi_)) leftW = Scale(120, dpi_);
    int rightX = leftW + gap;
    int rightW = width - rightX;
    if (rightW < Scale(120, dpi_)) rightW = Scale(120, dpi_);

    HDWP dwp = BeginDeferWindowPos(10 + kToolbarCount);
    if (dwp) {
        int x = 0;
        int bw = Scale(96, dpi_);
        for (int i = 0; i < kToolbarCount; i++) {
            dwp = DeferWindowPos(dwp, toolbar_[i], nullptr, x, 0, bw, tbH, SWP_NOZORDER);
            x += bw + Scale(2, dpi_);
        }
    }
    if (dwp) dwp = DeferWindowPos(dwp, left_.Hwnd(), nullptr, 0, panelsY, leftW, panelsH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, right_.Hwnd(), nullptr, rightX, panelsY, rightW, panelsH, SWP_NOZORDER);

    int labelW = Scale(330, dpi_);
    if (labelW > width / 2) labelW = width / 2;
    int bottomY = panelsY + panelsH;
    if (dwp) dwp = DeferWindowPos(dwp, label_, nullptr, 0, bottomY, labelW, promptH, SWP_NOZORDER);
    if (dwp)
        dwp = DeferWindowPos(dwp, prompt_, nullptr, labelW, bottomY, width - labelW, promptH, SWP_NOZORDER);

    int fkeyY = bottomY + promptH;
    int each = width / kFKeyCount;
    for (int i = 0; i < kFKeyCount; i++) {
        int x = i * each;
        int w = (i == kFKeyCount - 1) ? width - x : each;
        if (dwp) dwp = DeferWindowPos(dwp, fkeys_[i], nullptr, x, fkeyY, w, fkeyH, SWP_NOZORDER);
    }
    if (dwp) EndDeferWindowPos(dwp);

    // Инвалидация: без неё после resize/разворота остаются куски старой отрисовки.
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// ---------- панели ----------

Panel* App::ActivePanel() { return activeSide_ == 0 ? &left_ : &right_; }
Panel* App::OtherPanel() { return activeSide_ == 0 ? &right_ : &left_; }

void App::SetActivePanel(int index) {
    activeSide_ = index;
    left_.SetActive(index == 0);
    right_.SetActive(index == 1);
    UpdatePromptLabel();
}

void App::UpdatePromptLabel() {
    if (!label_) return;
    std::wstring text = ActivePanel()->Path();
    if (!text.empty() && text.back() != L'\\') text.push_back(L'\\');
    text += L">";
    SetWindowTextW(label_, text.c_str());
}

// ---------- команды ----------

void App::ShowError(const std::wstring& text) {
    MessageBoxW(hwnd_, text.c_str(), kTitle, MB_ICONERROR | MB_OK);
}

void App::OpenCursor() {
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e) return;

    if (e->isUp) {
        panel->GoUp();
        return;
    }
    if (e->isDir) {
        std::wstring target = JoinPath(panel->Path(), e->name);
        if (!panel->Navigate(target)) ShowError(panel->NavigateError());
        return;
    }

    std::wstring full = JoinPath(panel->Path(), e->name);
    HINSTANCE r = ShellExecuteW(hwnd_, L"open", full.c_str(), nullptr, panel->Path().c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShowError(L"Не удалось открыть:\n" + full);
}

void App::GoUpActive() {
    Panel* panel = ActivePanel();
    if (!panel->GoUp()) panel->GoRoot();
}

void App::DoCopyMove(bool move) {
    Panel* panel = ActivePanel();
    Panel* other = OtherPanel();
    std::vector<std::wstring> sources = panel->SelectedPaths();
    LogDebug(L"DoCopyMove move=" + std::to_wstring(move ? 1 : 0) + L" sources=" + std::to_wstring(sources.size()) +
             L" cursor=" + std::to_wstring(panel->CursorIndex()) + L" activeSide=" + std::to_wstring(activeSide_));
    if (sources.empty()) return;

    std::wstring target = other->Path();
    std::wstring prompt;
    if (sources.size() == 1) {
        std::wstring name = sources[0].substr(sources[0].find_last_of(L'\\') + 1);
        prompt = (move ? L"Переместить «" : L"Копировать «") + name + L"» в:";
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, move ? L"Переместить файлов: %zu в:" : L"Копировать файлов: %zu в:", sources.size());
        prompt = buf;
    }

    if (!InputBox(hwnd_, move ? L"Перемещение" : L"Копирование", prompt, target, true)) return;
    target = Trim(target);
    if (target.empty()) return;

    if (!IsDirectory(target)) {
        if (sources.size() == 1 && !target.empty() && target.back() != L'\\') {
            // Новое имя для единственного файла — копия рядом.
            std::wstring parent = ParentPath(target);
            if (!parent.empty() && !IsDirectory(parent)) CreateDirectoryW(parent.c_str(), nullptr);
        } else {
            CreateDirectoryW(target.c_str(), nullptr);
        }
    }

    if (!ShellCopyMove(hwnd_, sources, target, move)) {
        ShowError(L"Операция не выполнена или отменена.");
    }
    panel->ClearMarks();
    panel->Reload();
    other->Reload();
    UpdatePromptLabel();
}

void App::DoRename() {
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e || e->isUp) return;
    std::wstring name = e->name;
    if (!InputBox(hwnd_, L"Переименование", L"Новое имя:", name, false)) return;
    name = Trim(name);
    if (name.empty() || SamePath(name, e->name)) return;

    std::wstring from = JoinPath(panel->Path(), e->name);
    std::wstring to = JoinPath(panel->Path(), name);
    LogDebug(L"rename from='" + from + L"' to='" + to + L"'");
    DWORD renameErr = 0;
    if (!RenamePath(from, to, renameErr)) {
        LogDebug(L"rename FAILED err=" + std::to_wstring(renameErr));
        ShowError(L"Не удалось переименовать:\n" + ErrText(renameErr));
        return;
    }
    LogDebug(L"rename OK");
    panel->Reload();
    panel->SetCursorIndex(panel->CursorIndex());
    UpdatePromptLabel();
}

void App::DoMkdir() {
    Panel* panel = ActivePanel();
    std::wstring name;
    if (!InputBox(hwnd_, L"Создать каталог", L"Имя каталога:", name, false)) return;
    name = Trim(name);
    if (name.empty()) return;

    std::wstring full = JoinPath(panel->Path(), name);
    if (IsDirectory(full)) {
        ShowError(L"Каталог уже существует:\n" + full);
        return;
    }
    if (!MakeDir(full)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            ShowError(L"Не удалось создать каталог:\n" + ErrText(err));
            return;
        }
    }
    panel->Reload();
    panel->SetCursorIndex(panel->CursorIndex());
}

void App::DoDelete() {
    LogDebug(L"DoDelete enter");
    Panel* panel = ActivePanel();
    std::vector<std::wstring> targets = panel->SelectedPaths();
    if (targets.empty()) return;

    std::wstring question;
    if (targets.size() == 1) {
        std::wstring name = targets[0].substr(targets[0].find_last_of(L'\\') + 1);
        question = L"Удалить «" + name + L"» в корзину?";
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L"Удалить объектов: %zu в корзину?", targets.size());
        question = buf;
    }
    if (MessageBoxW(hwnd_, question.c_str(), L"Удаление", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    if (!DeleteViaShell(hwnd_, targets)) ShowError(L"Удаление не выполнено или отменено.");

    panel->ClearMarks();
    panel->Reload();
}

void App::DoProperties() {
    Panel* panel = ActivePanel();
    std::wstring path = panel->FocusedPath();
    if (path.empty()) return;
    SHObjectProperties(hwnd_, SHOP_FILEPATH, path.c_str(), nullptr);
}

void App::DoSort(int column) {
    Panel* panel = ActivePanel();
    bool reverse = (panel->SortColumn() == column) ? !panel->SortReverse() : (column == 2 || column == 3);
    panel->Sort(column, reverse);
}

void App::ShowDrivesMenu(int side) {
    Panel* panel = side == 0 ? &left_ : &right_;
    HMENU popup = CreatePopupMenu();
    auto drives = GetDrives();
    for (size_t i = 0; i < drives.size(); i++) {
        wchar_t text[64];
        const wchar_t* kind = L"диск";
        switch (drives[i].type) {
            case DRIVE_REMOVABLE: kind = L"сменный"; break;
            case DRIVE_FIXED: kind = L"жёсткий"; break;
            case DRIVE_REMOTE: kind = L"сетевой"; break;
            case DRIVE_CDROM: kind = L"CD/DVD"; break;
            case DRIVE_RAMDISK: kind = L"RAM-диск"; break;
            default: break;
        }
        swprintf(text, 64, L"%c:\\\t%ls", towupper(drives[i].letter), kind);
        AppendMenuW(popup, MF_STRING, 2000 + (int)i, text);
    }
    if (drives.empty()) AppendMenuW(popup, MF_STRING | MF_GRAYED, 0, L"Нет доступных дисков");

    POINT pt{};
    GetCursorPos(&pt);
    int choice = (int)TrackPopupMenu(popup, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_,
                                     nullptr);
    DestroyMenu(popup);

    if (choice >= 2000 && choice < 2000 + (int)drives.size()) {
        int index = choice - 2000;
        wchar_t root[8] = {drives[index].letter, L':', L'\\', 0};
        SetActivePanel(side);
        if (!panel->Navigate(root)) ShowError(panel->NavigateError());
        UpdatePromptLabel();
    }
}

void App::RunCommandLine() {
    wchar_t buf[4096]{};
    GetWindowTextW(prompt_, buf, 4096);
    std::wstring command = Trim(buf);
    if (command.empty()) return;

    history_.push_back(command);
    if (history_.size() > 50) history_.erase(history_.begin());
    SendMessageW(prompt_, CB_ADDSTRING, 0, (LPARAM)command.c_str());
    SetWindowTextW(prompt_, L"");

    std::wstring lower = Lower(command);
    if (lower.rfind(L"cd ", 0) == 0 || lower.rfind(L"cd\\", 0) == 0) {
        std::wstring dir = Trim(command.substr(2));
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) return;
        if (!ActivePanel()->Navigate(dir)) ShowError(ActivePanel()->NavigateError());
        UpdatePromptLabel();
        return;
    }

    std::wstring cwd = ActivePanel()->Path();
    HINSTANCE r = ShellExecuteW(hwnd_, L"open", command.c_str(), nullptr, cwd.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShowError(L"Не удалось выполнить:\n" + command);
}

void App::Dispatch(int id) {
    LogDebug(L"Dispatch id=" + std::to_wstring(id));
    Panel* panel = ActivePanel();
    switch (id) {
        case IDM_VIEW: {
            Entry* e = panel->Cursor();
            if (!e || e->isUp || e->isDir) {
                MessageBoxW(hwnd_, L"Просмотр доступен только для файлов.", kTitle, MB_ICONINFORMATION);
                return;
            }
            ShowLister(hwnd_, panel->FocusedPath());
            break;
        }
        case IDM_EDIT: {
            Entry* e = panel->Cursor();
            if (!e || e->isUp || e->isDir) return;
            std::wstring path = panel->FocusedPath();
            std::wstring params = L"\"" + path + L"\"";
            ShellExecuteW(hwnd_, L"open", L"notepad.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_COPY:
            DoCopyMove(false);
            break;
        case IDM_MOVE:
            DoCopyMove(true);
            break;
        case IDM_RENAME:
            DoRename();
            break;
        case IDM_MKDIR:
            DoMkdir();
            break;
        case IDM_DELETE:
            DoDelete();
            break;
        case IDM_PROPERTIES:
            DoProperties();
            break;
        case IDM_EXIT:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        case IDM_SEL_ALL:
            panel->MarkAll(true);
            break;
        case IDM_SEL_NONE:
            panel->MarkAll(false);
            break;
        case IDM_SEL_INVERT:
            panel->InvertMarks();
            break;
        case IDM_SEL_MASK: {
            std::wstring mask = L"*.*";
            if (!InputBox(hwnd_, L"Выделить по маске", L"Маска (например *.txt):", mask, false)) return;
            auto masks = SplitMasks(mask);
            for (auto& e : const_cast<std::vector<Entry>&>(panel->Entries())) {
                if (e.isUp || e.isDir) continue;
                for (auto& m : masks) {
                    if (MatchMask(Lower(e.name), m)) {
                        e.marked = true;
                        break;
                    }
                }
            }
            panel->Reload();
            break;
        }
        case IDM_SORT_NAME:
            DoSort(0);
            break;
        case IDM_SORT_EXT:
            DoSort(1);
            break;
        case IDM_SORT_SIZE:
            DoSort(2);
            break;
        case IDM_SORT_DATE:
            DoSort(3);
            break;
        case IDM_REVERSE:
            panel->Sort(panel->SortColumn(), !panel->SortReverse());
            break;
        case IDM_HIDDEN:
            showHidden_ = !showHidden_;
            left_.SetShowHidden(showHidden_);
            right_.SetShowHidden(showHidden_);
            left_.Reload();
            right_.Reload();
            break;
        case IDM_NAV_BACK:
            panel->GoBack();
            break;
        case IDM_NAV_FORWARD:
            panel->GoForward();
            break;
        case IDM_NAV_UP:
            GoUpActive();
            break;
        case IDM_REFRESH:
            panel->Reload();
            break;
        case IDM_SWAP: {
            std::wstring leftPath = left_.Path();
            std::wstring rightPath = right_.Path();
            left_.Navigate(rightPath);
            right_.Navigate(leftPath);
            UpdatePromptLabel();
            break;
        }
        case IDM_CONSOLE:
            ShellExecuteW(hwnd_, L"open", L"cmd.exe", L"/k", panel->Path().c_str(), SW_SHOWNORMAL);
            break;
        case IDM_EXPLORER:
            ShellExecuteW(hwnd_, L"open", panel->Path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_DRIVE_LEFT:
            ShowDrivesMenu(0);
            break;
        case IDM_DRIVE_RIGHT:
            ShowDrivesMenu(1);
            break;
        case IDM_ABOUT:
            MessageBoxW(hwnd_, L"POSCommander 2\nДвухпанельный файловый менеджер на Win32 API.\n\nC++ / MinGW · без внешних зависимостей",
                        L"О программе", MB_ICONINFORMATION | MB_OK);
            break;
        default:
            break;
    }
    // Возвращаем фокус в список — как в Total Commander после команды.
    if (GetFocus() != prompt_) ActivePanel()->FocusList();
    UpdatePromptLabel();
}

// ---------- клавиатура ----------

bool App::PreTranslate(const MSG& msg) {
    if (msg.message != WM_KEYDOWN) return false;
    LogDebug(L"vk=" + std::to_wstring(msg.wParam));
    HWND focus = GetFocus();
    bool inLeftList = focus && focus == left_.List();
    bool inRightList = focus && focus == right_.List();
    bool inList = inLeftList || inRightList;
    bool inPrompt = focus && (focus == prompt_ || GetParent(focus) == prompt_);

    if (inLeftList && activeSide_ != 0) SetActivePanel(0);
    if (inRightList && activeSide_ != 1) SetActivePanel(1);

    if (inPrompt) {
        switch (msg.wParam) {
            case VK_RETURN:
                RunCommandLine();
                return true;
            case VK_ESCAPE:
                SetWindowTextW(prompt_, L"");
                ActivePanel()->FocusList();
                return true;
            case VK_TAB:
                ActivePanel()->FocusList();
                return true;
            default:
                break;
        }
    }

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    Panel* panel = ActivePanel();

    if (alt && msg.wParam == VK_F1) {
        ShowDrivesMenu(0);
        return true;
    }
    if (alt && msg.wParam == VK_F2) {
        ShowDrivesMenu(1);
        return true;
    }

    switch (msg.wParam) {
        case VK_F3:
            Dispatch(IDM_VIEW);
            return true;
        case VK_F4:
            Dispatch(IDM_EDIT);
            return true;
        case VK_F5:
            Dispatch(IDM_COPY);
            return true;
        case VK_F6:
            Dispatch(IDM_MOVE);
            return true;
        case VK_F7:
            Dispatch(IDM_MKDIR);
            return true;
        case VK_F8:
            Dispatch(IDM_DELETE);
            return true;
        case VK_F2:
            Dispatch(IDM_RENAME);
            return true;
        case VK_RETURN:
            if (!inPrompt) {
                OpenCursor();
                return true;
            }
            break;
        case VK_BACK:
            if (!inPrompt) {
                GoUpActive();
                return true;
            }
            break;
        case VK_TAB:
            if (!inPrompt) {
                if (ctrl) {
                    ActivePanel()->NextTab();
                    ActivePanel()->FocusList();
                    return true;
                }
                SetActivePanel(activeSide_ == 0 ? 1 : 0);
                ActivePanel()->FocusList();
                return true;
            }
            break;
        case VK_INSERT:
        case VK_SPACE:
            if (!inPrompt) {
                panel->ToggleMarkCursor();
                return true;
            }
            break;
        case VK_DELETE:
            if (!inPrompt && !inList) break;
            Dispatch(IDM_DELETE);
            return true;
        default:
            break;
    }

    if (ctrl) {
        switch (msg.wParam) {
            case 'A':
                panel->MarkAll(true);
                return true;
            case 'U':
                Dispatch(IDM_SWAP);
                return true;
            case 'R':
                panel->Reload();
                return true;
            case 'H':
                Dispatch(IDM_HIDDEN);
                return true;
            case 'T':
                panel->AddTab();
                return true;
            case 'W':
                panel->CloseTab();
                return true;
            case VK_F3:
                Dispatch(IDM_SORT_NAME);
                return true;
            case VK_F4:
                Dispatch(IDM_SORT_EXT);
                return true;
            case VK_F5:
                Dispatch(IDM_SORT_SIZE);
                return true;
            case VK_F6:
                Dispatch(IDM_SORT_DATE);
                return true;
            default:
                break;
        }
    }

    if (shift && msg.wParam == VK_F6) {
        Dispatch(IDM_RENAME);
        return true;
    }
    return false;
}

// ---------- сообщения ----------

LRESULT CALLBACK App::ProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->Proc(hwnd, msg, wParam, lParam);
}

LRESULT App::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateChildren();
            Layout();
            return 0;

        case WM_APP + 101:
            LoadPanels();
            return 0;

        case WM_SIZE:
            Layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scale(600, dpi_);
            mmi->ptMinTrackSize.y = Scale(360, dpi_);
            return 0;
        }

        case WM_SETFOCUS:
            if (ActivePanel()) ActivePanel()->FocusList();
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_OPEN_ITEM) {
                HWND from = (HWND)lParam;
                if (from == left_.Hwnd()) SetActivePanel(0);
                if (from == right_.Hwnd()) SetActivePanel(1);
                OpenCursor();
                return 0;
            }
            if (id >= ID_TB_FIRST && id < ID_TB_FIRST + kToolbarCount) {
                Dispatch(kToolbar[id - ID_TB_FIRST].command);
                return 0;
            }
            if (id >= ID_FKEY_FIRST && id < ID_FKEY_FIRST + kFKeyCount) {
                Dispatch(kFKeys[id - ID_FKEY_FIRST].command);
                return 0;
            }
            if (id == ID_PROMPT) {
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    int sel = (int)SendMessageW(prompt_, CB_GETCURSEL, 0, 0);
                    if (sel >= 0) {
                        wchar_t buf[4096]{};
                        SendMessageW(prompt_, CB_GETLBTEXT, sel, (LPARAM)buf);
                        SetWindowTextW(prompt_, buf);
                    }
                }
                return 0;
            }
            Dispatch(id);
            return 0;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                if (abs(pt.x - SplitX()) <= Scale(3, dpi_)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            if (abs(x - SplitX()) <= Scale(3, dpi_)) {
                draggingSplit_ = true;
                SetCapture(hwnd_);
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (draggingSplit_) {
                int x = GET_X_LPARAM(lParam);
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                int width = rc.right - rc.left;
                if (width > 0) {
                    int percent = MulDiv(x, 100, width);
                    if (percent < 15) percent = 15;
                    if (percent > 85) percent = 85;
                    splitPercent_ = percent;
                    Layout();
                }
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP:
            if (draggingSplit_) {
                draggingSplit_ = false;
                ReleaseCapture();
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY: {
            Settings& st = GetSettings();
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hwnd_, &wp)) {
                st.x = wp.rcNormalPosition.left;
                st.y = wp.rcNormalPosition.top;
                st.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                st.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                st.hasPosition = true;
                st.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            }
            st.splitPercent = splitPercent_;
            st.showHidden = showHidden_;
            st.sortColumn = left_.SortColumn();
            st.sortReverse = left_.SortReverse();
            left_.CaptureState(st.leftTabs, st.leftActive);
            right_.CaptureState(st.rightTabs, st.rightActive);
            SaveSettings();
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int App::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (PreTranslate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;

    if (cmdLine && wcsstr(cmdLine, L"--selftest")) return RunSelfTest();

    LoadSettings();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    App app;
    int code = 0;
    if (app.Create(inst)) {
        code = app.Run();
    } else {
        MessageBoxW(nullptr, L"Не удалось создать главное окно.", L"POSCommander 2", MB_ICONERROR);
        code = 1;
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return code;
}
