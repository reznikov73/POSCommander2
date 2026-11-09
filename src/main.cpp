// Главное окно POSCommander 2: две панели, меню, командная строка, F-клавиши.
#include "common.h"
#include "dialogs.h"
#include "panel.h"
#include "util.h"
#include "ops.h"
#include "selftest.h"
#include "settings.h"
#include "search.h"
#include "archive.h"
#include "plugins.h"
#include "wcx.h"

#include <windowsx.h>
#include <winnetwk.h>

#include <commctrl.h>

#include <iphlpapi.h>

#include <cmath>
#include <cstdio>
#include <map>

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
    IDM_BOOKMARK_ADD = 170, IDM_BOOKMARK_SHOW,
    IDM_TREE = 175,
    IDM_PACK = 178,
    IDM_BATCH_RENAME = 179,
    IDM_PLUGINS = 182,
    IDM_QUEUE = 184,
    IDM_DIRSIZE = 186,
    IDM_NEWFILE = 190,
    IDM_QUICKVIEW = 191,
    IDM_SEARCH = 192,
    IDM_NET_BROWSE = 193,
    IDM_NET_MAP = 194,
    IDM_NET_UNMAP = 195,
    IDM_NET_UNC = 196,
    IDM_TAB_COPY = 205,
    IDM_TAB_LOCK = 204,
    IDM_TAB_CLOSEALL = 203,
    IDM_TAB_CLOSE = 202,
    IDM_TAB_NEW = 201,
    IDM_UNPACK = 197,
    IDM_SYNC = 198,

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
const int ID_TERM = 1004;            // кнопка «Терминал» справа от командной строки
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

// Подписи под ряд F-клавиш: обычные, с Shift, с Ctrl и с Alt (как в TC).
struct FKeyLabel {
    const wchar_t* normal;
    const wchar_t* shift;
    const wchar_t* ctrl;
    const wchar_t* alt;
};
const FKeyLabel kFKeyLabels[] = {
    {L"F3 Просмотр", L"Shift+F3", L"Ctrl+F3 Имя", L"Alt+F3"},
    {L"F4 Правка", L"Shift+F4 Новый", L"Ctrl+F4 Тип", L"Alt+F4 Выход"},
    {L"F5 Копия", L"Shift+F5 Копия сюда", L"Ctrl+F5 Размер", L"Alt+F5 Упаковать"},
    {L"F6 Перенос", L"Shift+F6 Переимен.", L"Ctrl+F6 Дата", L"Alt+F6"},
    {L"F7 Каталог", L"Shift+F7", L"Ctrl+F7", L"Alt+F7 Поиск"},
    {L"F8 Удалить", L"Shift+F8", L"Ctrl+F8 Дерево", L"Alt+F8"},
    {L"Alt+F4 Выход", L"Alt+Shift+Enter Размеры", L"Ctrl+W Закрыть вкладку", L"Alt+F4 Выход"},
};
static_assert(sizeof(kFKeyLabels) / sizeof(kFKeyLabels[0]) == sizeof(kFKeys) / sizeof(kFKeys[0]),
              "подписи F-клавиш должны совпадать с кнопками");

// Тулбар: кнопки и их команды.
const UINT_PTR kTitleTimer = 10;      // обновление заголовка окна

const int ID_TB_FIRST = 1100;
// Сепаратор в тулбаре: не кнопка, команда 0.
struct TbDef { const wchar_t* text; int command; bool separator; };
// Тулбар как в TC: 17 кнопок 24x24 в группах с вертикальными сепараторами.
// F3-F8 продублированы иконками — как в Total Commander.
const TbDef kToolbar[] = {
    {L"Назад",                       IDM_NAV_BACK,      false},
    {L"Вперёд",                      IDM_NAV_FORWARD,   false},
    {L"Вверх",                       IDM_NAV_UP,        false},
    {L"Дерево каталогов",             IDM_TREE,           false},
    {L"",                            0,                 true},
    {L"Просмотр (F3)",                IDM_VIEW,           false},
    {L"Правка (F4)",                  IDM_EDIT,           false},
    {L"Копирование (F5)",            IDM_COPY,           false},
    {L"Перемещение (F6)",            IDM_MOVE,           false},
    {L"Новый каталог (F7)",          IDM_MKDIR,          false},
    {L"Удаление (F8)",               IDM_DELETE,         false},
    {L"",                            0,                 true},
    {L"Упаковать (Alt+F5)",          IDM_PACK,           false},
    {L"Распаковать",                 IDM_UNPACK,         false},
    {L"Пакетное переименование (Ctrl+M)", IDM_BATCH_RENAME, false},
    {L"",                            0,                 true},
    {L"Поиск файлов",                IDM_SEARCH,         false},
    {L"Синхронизация каталогов",      IDM_SYNC,           false},
    {L"Обновить (Ctrl+R)",           IDM_REFRESH,        false},
    {L"",                            0,                 true},
    {L"Свойства (Alt+Enter)",        IDM_PROPERTIES,     false},
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
    void AddBookmark();
    void PackSelection();
    void BatchRename();
    void QueueCopyMove(const std::vector<std::wstring>& items, const std::wstring& target, bool move,
                       bool overwrite = false);
    void RunNextJob();
    void ShowQueueWindow();
    static LRESULT CALLBACK QueueProc(HWND, UINT, WPARAM, LPARAM);
    void RefreshQueueWindow();
    int PanelAtScreen(POINT screen) const;
    // Распаковать один файл из архива во временный каталог; вернуть путь или пусто.
    std::wstring ExtractOneFromArchive(Panel* panel, const std::wstring& innerName);
    void DropOnPanel(int fromSide, int toSide);
    void ShowBookmarksMenu();
    void RunCommandLine();
    void SaveState();
    void UpdateTitle();
    void BrowseNetwork();
    void MapNetworkDrive();
    void UnmapNetworkDrive();
    void OpenUncPath();
    void RebuildDriveBars();
    void ToggleQuickView();
    void DrawFlatButton(DRAWITEMSTRUCT* dis);
    void DrawTermButton(DRAWITEMSTRUCT* dis);
    static LRESULT CALLBACK TbBtnProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    HICON ToolIcon(int command);
    void UpdateFKeyLabels(int kind);
    int FKeyCommand(int index, int kind) const;
    void RefreshQuickView();
    void AppendToPrompt(const std::wstring& text, bool quoted);
    void ShowError(const std::wstring& text);
    int SplitX() const;

    HWND hwnd_ = nullptr;
    HMENU menu_ = nullptr;
    HWND label_ = nullptr;
    HWND prompt_ = nullptr;
    HWND term_ = nullptr;               // кнопка «Терминал» у командной строки
    int histPos_ = -1;                  // позиция в истории команд (-1 — новая строка)
    HFONT boldFont_ = nullptr;          // жирный шрифт для клавиши в F-кнопках
    HWND fkeys_[kFKeyCount]{};
    // Данные для строки состояния в заголовке окна (как в TC).
    ULONGLONG titleCpuIdle_ = 0, titleCpuKernel_ = 0, titleCpuUser_ = 0;
    struct IfCounter {
        DWORD index = 0;
        DWORD in = 0;
        DWORD out = 0;
    };
    std::vector<IfCounter> netCounters_;

    int hoverBtn_ = -1;                 // индекс кнопки тулбара под курсором (-1 — нет)
    std::map<int, HICON> toolIcons_;    // кэш цветных значков тулбара по команде
    HWND tip_ = nullptr;                // подсказки кнопок тулбара
    HWND quickView_ = nullptr;          // Ctrl+Q: просмотр на неактивной панели
    int quickViewSide_ = -1;
    HWND toolbar_[kToolbarCount]{};
    Panel left_{0};
    Panel right_{1};
    int activeSide_ = 0;
    int modKind_ = -1;                  // 0 — без модификаторов, 1 — Shift, 2 — Ctrl, 3 — Alt
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    bool showHidden_ = true;
    int splitPercent_ = 50;
    std::wstring quickFilter_;          // буфер быстрого поиска по буквам
    bool dragging_ = false;             // внутреннее перетаскивание файлов
    int dragFromSide_ = 0;              // из какой панели тянут

    // Очередь операций копирования/перемещения.
    struct CopyJob {
        std::vector<std::wstring> items;
        std::wstring target;
        bool move = false;
        bool overwrite = false;         // перезаписывать без запроса
        std::wstring status;            // ожидает / выполняется / готово / ошибка
    };
    std::vector<CopyJob> jobs_;
    int activeJob_ = -1;
    HWND queueWnd_ = nullptr;
    HWND queueList_ = nullptr;
    HWND queueStatus_ = nullptr;
    ULONGLONG quickFilterAt_ = 0;       // время последнего символа
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
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
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
    // Порядок и состав пунктов — как в TCUP 9.0a (docs/tc-ui-reference.md):
    // Файл, Выделение, Навигация, Сеть, Вид, Вкладки, Конфигурация, Инструменты, Запуск, Помощь.
    menu_ = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_VIEW, L"&Просмотр\tF3");
    AppendMenuW(file, MF_STRING, IDM_EDIT, L"&Правка\tF4");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_COPY, L"&Копировать\tF5");
    AppendMenuW(file, MF_STRING, IDM_MOVE, L"&Переместить\tF6");
    AppendMenuW(file, MF_STRING, IDM_RENAME, L"Пере&именовать\tF2");
    AppendMenuW(file, MF_STRING, IDM_MKDIR, L"Создать &каталог\tF7");
    AppendMenuW(file, MF_STRING, IDM_NEWFILE, L"&Новый файл\tShift+F4");
    AppendMenuW(file, MF_STRING, IDM_DELETE, L"&Удалить\tF8");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PROPERTIES, L"С&войства\tAlt+Enter");
    AppendMenuW(file, MF_STRING, IDM_DIRSIZE, L"Размер &каталога\tAlt+Shift+Enter");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PACK, L"У&паковать…\tAlt+F5");
    AppendMenuW(file, MF_STRING, IDM_UNPACK, L"Рас&паковать…");
    AppendMenuW(file, MF_STRING, IDM_BATCH_RENAME, L"Пакетное &переименование…\tCtrl+M");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"В&ыход\tAlt+F4");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)file, L"&Файл");

    HMENU sel = CreatePopupMenu();
    AppendMenuW(sel, MF_STRING, IDM_SEL_ALL, L"Выделить &всё\tCtrl+A");
    AppendMenuW(sel, MF_STRING, IDM_SEL_NONE, L"&Снять выделение");
    AppendMenuW(sel, MF_STRING, IDM_SEL_INVERT, L"&Инвертировать");
    AppendMenuW(sel, MF_STRING, IDM_SEL_MASK, L"По &маске…\tNum +");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)sel, L"В&ыделение");

    HMENU nav = CreatePopupMenu();
    AppendMenuW(nav, MF_STRING, IDM_NAV_UP, L"&Вверх\tCtrl+PgUp");
    AppendMenuW(nav, MF_STRING, IDM_NAV_BACK, L"&Назад\tAlt+←");
    AppendMenuW(nav, MF_STRING, IDM_NAV_FORWARD, L"Вперёд\tAlt+→");
    AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(nav, MF_STRING, IDM_TREE, L"&Дерево каталогов\tCtrl+F8");
    AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(nav, MF_STRING, IDM_DRIVE_LEFT, L"Диск &левой панели\tAlt+F1");
    AppendMenuW(nav, MF_STRING, IDM_DRIVE_RIGHT, L"Диск &правой панели\tAlt+F2");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)nav, L"&Навигация");

    HMENU net = CreatePopupMenu();
    AppendMenuW(net, MF_STRING, IDM_NET_BROWSE, L"&Обзор сети…");
    AppendMenuW(net, MF_STRING, IDM_NET_UNC, L"Перейти по &UNC-пути…");
    AppendMenuW(net, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(net, MF_STRING, IDM_NET_MAP, L"&Подключить сетевой диск…");
    AppendMenuW(net, MF_STRING, IDM_NET_UNMAP, L"&Отключить сетевой диск…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)net, L"&Сеть");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_SORT_NAME, L"По &имени\tCtrl+F3");
    AppendMenuW(view, MF_STRING, IDM_SORT_EXT, L"По &типу\tCtrl+F4");
    AppendMenuW(view, MF_STRING, IDM_SORT_SIZE, L"По &размеру\tCtrl+F5");
    AppendMenuW(view, MF_STRING, IDM_SORT_DATE, L"По &дате\tCtrl+F6");
    AppendMenuW(view, MF_STRING, IDM_REVERSE, L"Обратный &порядок");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_HIDDEN, L"Скрытые &файлы\tCtrl+H");
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Обновить\tCtrl+R");
    AppendMenuW(view, MF_STRING, IDM_QUICKVIEW, L"&Быстрый просмотр\tCtrl+Q");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_BOOKMARK_ADD, L"Добавить &закладку\tCtrl+D");
    AppendMenuW(view, MF_STRING, IDM_BOOKMARK_SHOW, L"&Закладки…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)view, L"&Вид");

    HMENU tabs = CreatePopupMenu();
    AppendMenuW(tabs, MF_STRING, IDM_TAB_NEW, L"&Новая вкладка\tCtrl+T");
    AppendMenuW(tabs, MF_STRING, IDM_TAB_CLOSE, L"&Закрыть вкладку\tCtrl+W");
    AppendMenuW(tabs, MF_STRING, IDM_TAB_CLOSEALL, L"Закрыть &все вкладки\tCtrl+Shift+W");
    AppendMenuW(tabs, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tabs, MF_STRING, IDM_TAB_LOCK, L"&Блокировать вкладку");
    AppendMenuW(tabs, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tabs, MF_STRING, IDM_TAB_COPY, L"&Копировать вкладку на другую панель");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)tabs, L"В&кладки");

    HMENU cfg = CreatePopupMenu();
    AppendMenuW(cfg, MF_STRING, IDM_PLUGINS, L"&Плагины Total Commander…");
    AppendMenuW(cfg, MF_STRING, IDM_QUEUE, L"&Очередь операций…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)cfg, L"&Конфигурация");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_SEARCH, L"&Поиск файлов\tAlt+F7");
    AppendMenuW(tools, MF_STRING, IDM_SYNC, L"&Синхронизация каталогов");
    AppendMenuW(tools, MF_STRING, IDM_BATCH_RENAME, L"&Пакетное переименование…\tCtrl+M");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_SWAP, L"С&менить панели\tCtrl+U");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)tools, L"&Инструменты");

    HMENU startMenu = CreatePopupMenu();
    AppendMenuW(startMenu, MF_STRING, IDM_EXPLORER, L"&Проводник здесь");
    AppendMenuW(startMenu, MF_STRING, IDM_CONSOLE, L"&Командная строка здесь");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)startMenu, L"&Запуск");

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
        toolbar_[i] = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                      hwnd_, (HMENU)(INT_PTR)(ID_TB_FIRST + i), g_inst, nullptr);
        // Кнопкам (не сепараторам) — subclass для hover-подсветки, как в TC.
        if (!kToolbar[i].separator) SetWindowSubclass(toolbar_[i], TbBtnProc, 5, (DWORD_PTR)this);
    }

    // Кнопка «Терминал» справа от командной строки: запускает cmd в каталоге активной панели.
    term_ = CreateWindowExW(0, L"BUTTON", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                            hwnd_, (HMENU)(INT_PTR)ID_TERM, g_inst, nullptr);

    // История командной строки из настроек — в выпадающий список.
    history_ = GetSettings().commandHistory;
    for (auto& item : history_) SendMessageW(prompt_, CB_ADDSTRING, 0, (LPARAM)item.c_str());

    for (int i = 0; i < kFKeyCount; i++) {
        fkeys_[i] = CreateWindowExW(0, L"BUTTON", kFKeys[i].text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                    hwnd_, (HMENU)(INT_PTR)(ID_FKEY_FIRST + i), g_inst, nullptr);
    }

    // Подсказки к иконкам тулбара: сами кнопки без подписей.
    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                           WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                           CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, g_inst, nullptr);
    if (tip_) {
        for (int i = 0; i < kToolbarCount; i++) {
            if (kToolbar[i].separator) continue;   // сепараторам подсказки не нужны
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd_;
            ti.uId = (UINT_PTR)toolbar_[i];
            ti.lpszText = (LPWSTR)kToolbar[i].text;
            SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }

    ApplyFont();

    // Приём файлов, перетащенных из Проводника.
    DragAcceptFiles(hwnd_, TRUE);

    // Заголовок окна: дата/время, сеть, память, ЦП — обновляем раз в секунду, как TC.
    SetTimer(hwnd_, kTitleTimer, 1000, nullptr);
    UpdateTitle();

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

    // Маски панелей из настроек: фильтр применяется при чтении каталога.
    left_.RestoreMask(st.leftMask);
    right_.RestoreMask(st.rightMask);

    if (!st.leftTabs.empty()) left_.RestoreState(st.leftTabs, st.leftActive);
    else left_.Navigate(cwd);

    if (!st.rightTabs.empty()) right_.RestoreState(st.rightTabs, st.rightActive);
    else right_.Navigate(cwd);

    left_.SetTabMeta(st.leftTabNames, st.leftTabLocks);
    right_.SetTabMeta(st.rightTabNames, st.rightTabLocks);

    // Возвращаем курсор и активную панель.
    left_.SetCursorIndex(st.leftCursor);
    right_.SetCursorIndex(st.rightCursor);
    if (st.activeSide == 1) SetActivePanel(1);
    ActivePanel()->FocusList();
    UpdatePromptLabel();

    // Временная диагностика этапа 4 (рамка курсора неактивной панели) — в %TEMP%\posc2-debug.log.
    wchar_t stamp[32]{};
    swprintf(stamp, 32, L"%hs %hs", __DATE__, __TIME__);
    LogDebug(L"старт: сборка " + std::wstring(stamp) + L", активная панель " +
             std::to_wstring(activeSide_) + L", курсоры " + std::to_wstring(left_.CursorIndex()) + L"/" +
             std::to_wstring(right_.CursorIndex()) + L" (" + std::to_wstring((int)left_.Entries().size()) + L"/" +
             std::to_wstring((int)right_.Entries().size()) + L" записей)");
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
    if (term_) SendMessageW(term_, WM_SETFONT, (WPARAM)font_, TRUE);
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
    int tbH = Scale(28, dpi_);          // ряд тулбара: кнопки 24×24 + поля
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
        const int bh = Scale(24, dpi_);              // кнопка 24×24, как в TC
        const int by = (tbH - bh) / 2;
        for (int i = 0; i < kToolbarCount; i++) {
            const int w = kToolbar[i].separator ? Scale(7, dpi_) : bh;
            dwp = DeferWindowPos(dwp, toolbar_[i], nullptr, x, by, w, bh, SWP_NOZORDER);
            x += w + Scale(2, dpi_);
        }
    }
    if (dwp) dwp = DeferWindowPos(dwp, left_.Hwnd(), nullptr, 0, panelsY, leftW, panelsH, SWP_NOZORDER);
    if (dwp) dwp = DeferWindowPos(dwp, right_.Hwnd(), nullptr, rightX, panelsY, rightW, panelsH, SWP_NOZORDER);

    int labelW = Scale(330, dpi_);
    if (labelW > width / 2) labelW = width / 2;
    int bottomY = panelsY + panelsH;
    const int termW = promptH;                       // квадратная кнопка «Терминал», как в TC
    int promptW = width - labelW - termW;
    if (promptW < Scale(80, dpi_)) promptW = Scale(80, dpi_);
    if (dwp) dwp = DeferWindowPos(dwp, label_, nullptr, 0, bottomY, labelW, promptH, SWP_NOZORDER);
    if (dwp)
        dwp = DeferWindowPos(dwp, prompt_, nullptr, labelW, bottomY, promptW, promptH, SWP_NOZORDER);
    if (dwp)
        dwp = DeferWindowPos(dwp, term_, nullptr, labelW + promptW, bottomY, termW, promptH, SWP_NOZORDER);

    if (quickView_) {
        Panel& other = (quickViewSide_ == 0) ? left_ : right_;
        RECT lr{};
        if (GetWindowRect(other.List(), &lr)) {
            MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT*>(&lr), 2);
            SetWindowPos(quickView_, HWND_TOP, lr.left, lr.top, lr.right - lr.left, lr.bottom - lr.top,
                         SWP_NOACTIVATE);
        }
    }

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

// Сокращение длинного пути слева, как в TC: «...\net10.0>».
// Отрезаем ведущие компоненты, пока строка не влезет в ширину подписи.
static std::wstring CompactPathLeft(HDC dc, const std::wstring& text, int width) {
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    if (sz.cx <= width) return text;

    size_t pos = 0;
    while (true) {
        const size_t next = text.find(L'\\', pos);
        if (next == std::wstring::npos) break;
        pos = next + 1;
        const std::wstring candidate = L"..." + text.substr(pos - 1);
        GetTextExtentPoint32W(dc, candidate.c_str(), (int)candidate.size(), &sz);
        if (sz.cx <= width) return candidate;
    }
    // Совсем длинное имя одного компонента: оставить хвост.
    const size_t keep = 12;
    return L"..." + text.substr(text.size() > keep ? text.size() - keep : 0);
}

void App::UpdatePromptLabel() {
    if (!label_) return;
    std::wstring text = ActivePanel()->Path();
    if (!text.empty() && text.back() != L'\\') text.push_back(L'\\');
    text += L">";

    // Длинный путь сокращаем слева с многоточием под ширину подписи (как в TC: «...\net10.0>»).
    RECT rc{};
    GetClientRect(label_, &rc);
    const int width = rc.right - rc.left;
    if (width > 0) {
        HDC dc = GetDC(label_);
        HGDIOBJ old = SelectObject(dc, font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        const std::wstring compact = CompactPathLeft(dc, text, width);
        SelectObject(dc, old);
        ReleaseDC(label_, dc);
        SetWindowTextW(label_, compact.c_str());
        return;
    }
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

    // Файл внутри архива: распаковываем во временный каталог и открываем.
    if (panel->InArchive()) {
        std::wstring inner = panel->ArchiveInner().empty() ? e->name : panel->ArchiveInner() + L"\\" + e->name;
        std::wstring temp = ExtractOneFromArchive(panel, inner);
        if (temp.empty()) return;
        HINSTANCE hr = ShellExecuteW(hwnd_, L"open", temp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) ShowError(L"Не удалось открыть:\n" + temp);
        return;
    }

    std::wstring full = JoinPath(panel->Path(), e->name);

    // Архив открываем как папку: панель входит внутрь, список — содержимое архива.
    if (IsArchivePath(full)) {
        if (!panel->Navigate(full)) ShowError(panel->NavigateError());
        UpdatePromptLabel();
        return;
    }

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

    // Из архива — распаковка в каталог другой панели; в архив копировать нельзя.
    if (panel->InArchive()) {
        if (other->InArchive()) {
            ShowError(L"Между архивами копирование не поддерживается.");
            return;
        }
        std::vector<std::wstring> inner;
        std::wstring prefix = panel->ArchiveInner().empty() ? L"" : panel->ArchiveInner() + L"\\";
        for (auto& name : panel->SelectedNames()) inner.push_back(prefix + name);
        if (inner.empty()) return;

        std::wstring target = other->Path();
        std::wstring error;
        if (!ExtractFromArchive(panel->ArchiveFile(), inner, target, error)) {
            ShowError(error);
            return;
        }
        panel->ClearMarks();
        other->Reload();
        UpdatePromptLabel();
        return;
    }
    if (other->InArchive()) {
        ShowError(L"Копирование внутрь архива не поддерживается.");
        return;
    }
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

    CopyMoveOptions opts;
    opts.target = target;
    if (!CopyMoveBox(hwnd_, move ? L"Перемещение" : L"Копирование", prompt, opts)) return;

    target = Trim(opts.target);
    if (target.empty()) return;

    // Маска из диалога ограничивает набор копируемых файлов.
    if (!Trim(opts.mask).empty()) {
        const auto masks = SplitMasks(opts.mask);
        std::vector<std::wstring> filtered;
        for (auto& s : sources) {
            if (IsDirectory(s)) continue;                   // каталоги маской не отсекаем
            const std::wstring name = Lower(s.substr(s.find_last_of(L'\\') + 1));
            for (auto& mm : masks) {
                if (MatchMask(name, mm)) {
                    filtered.push_back(s);
                    break;
                }
            }
        }
        if (filtered.empty()) {
            ShowError(L"Под маску «" + opts.mask + L"» не попал ни один файл.");
            return;
        }
        sources = filtered;
    }

    if (!IsDirectory(target)) {
        if (sources.size() == 1 && !target.empty() && target.back() != L'\\') {
            // Новое имя для единственного файла — копия рядом.
            std::wstring parent = ParentPath(target);
            if (!parent.empty() && !IsDirectory(parent)) CreateDirectoryW(parent.c_str(), nullptr);
        } else {
            CreateDirectoryW(target.c_str(), nullptr);
        }
    }

    if (opts.background) {
        QueueCopyMove(sources, target, move, opts.overwriteAll);
    } else {
        const bool ok = ShellCopyMove(hwnd_, sources, target, move, false, opts.overwriteAll);
        LogDebug(L"copy/move immediately ok=" + std::to_wstring(ok ? 1 : 0));
        left_.Reload();
        right_.Reload();
        UpdatePromptLabel();
    }
    panel->ClearMarks();
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

// Плоские кнопки тулбара в стиле TC: значок 16×16, hover-подсветка #CCE8FF.
void App::DrawFlatButton(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    const int id = (int)dis->CtlID;
    const bool isTool = id >= ID_TB_FIRST && id < ID_TB_FIRST + kToolbarCount;
    const int idx = isTool ? id - ID_TB_FIRST : -1;

    if (isTool && kToolbar[idx].separator) {
        // Сепаратор: вертикальная линия по центру, как в TC.
        HBRUSH fill = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(dc, &rc, fill);
        DeleteObject(fill);
        RECT ln = rc;
        ln.left = (rc.left + rc.right) / 2;
        ln.right = ln.left + 1;
        InflateRect(&ln, 0, -Scale(4, dpi_));
        HBRUSH line = CreateSolidBrush(RGB(0xC0, 0xC0, 0xC0));
        FillRect(dc, &ln, line);
        DeleteObject(line);
        return;
    }

    if (isTool) {
        // Кнопка тулбара: hover — заливка #CCE8FF с рамкой #99D1FF, как в TC.
        const bool hover = hoverBtn_ == idx && !pressed && !disabled;
        HBRUSH fill = CreateSolidBrush(pressed ? RGB(0x99, 0xD1, 0xFF)
                                     : hover   ? RGB(0xCC, 0xE8, 0xFF)
                                               : GetSysColor(COLOR_BTNFACE));
        FillRect(dc, &rc, fill);
        DeleteObject(fill);
        if (hover || pressed) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x99, 0xD1, 0xFF));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        if (HICON icon = ToolIcon(kToolbar[idx].command)) {
            const int sz = Scale(16, dpi_);
            const int x = (rc.left + rc.right - sz) / 2;
            const int y = (rc.top + rc.bottom - sz) / 2 + (pressed ? 1 : 0);
            DrawIconEx(dc, x, y, icon, sz, sz, 0, nullptr, DI_NORMAL);
        }
        return;
    }

    // F-кнопки: текст, рамка только у нажатой.
    HBRUSH fill = CreateSolidBrush(pressed ? RGB(198, 195, 189) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    if (pressed) DrawEdge(dc, &rc, BDR_SUNKENOUTER, BF_RECT);

    wchar_t text[160]{};
    GetWindowTextW(dis->hwndItem, text, 160);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(20, 20, 20));
    HFONT normal = font_ ? font_ : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Клавиша жирным, подпись обычным — как в TC («F5» жирное, «Копирование» обычное).
    const std::wstring full(text);
    const size_t sp = full.find(L' ');
    if (sp != std::wstring::npos && sp > 0) {
        if (!boldFont_) {
            LOGFONTW lf{};
            if (GetObjectW(normal, sizeof(lf), &lf) != sizeof(lf)) {
                lf = LOGFONTW{};
                lf.lfHeight = -MulDiv(9, (int)dpi_, 72);
                lf.lfCharSet = DEFAULT_CHARSET;
                lf.lfQuality = CLEARTYPE_QUALITY;
                wcscpy_s(lf.lfFaceName, L"Segoe UI");
            }
            lf.lfWeight = FW_BOLD;
            boldFont_ = CreateFontIndirectW(&lf);
        }
        const std::wstring key = full.substr(0, sp);
        const std::wstring rest = full.substr(sp);

        HGDIOBJ oldFont = SelectObject(dc, boldFont_ ? boldFont_ : normal);
        SIZE keySize{};
        GetTextExtentPoint32W(dc, key.c_str(), (int)key.size(), &keySize);
        SelectObject(dc, normal);
        SIZE restSize{};
        GetTextExtentPoint32W(dc, rest.c_str(), (int)rest.size(), &restSize);

        const int total = keySize.cx + restSize.cx;
        int x = rc.left + ((rc.right - rc.left) - total) / 2;
        const int minX = rc.left + Scale(2, dpi_);
        if (x < minX) x = minX;

        RECT kr{x, rc.top, rc.right, rc.bottom};
        SelectObject(dc, boldFont_ ? boldFont_ : normal);
        DrawTextW(dc, key.c_str(), -1, &kr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT rr{x + keySize.cx, rc.top, rc.right, rc.bottom};
        SelectObject(dc, normal);
        DrawTextW(dc, rest.c_str(), -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return;
    }

    HGDIOBJ oldFont = SelectObject(dc, normal);
    RECT tr = rc;
    InflateRect(&tr, -Scale(3, dpi_), 0);
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// Кнопка «Терминал» у командной строки: плоская, знак «>_» зелёным (#4CE44C), как в TCUP.
void App::DrawTermButton(DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH fill = CreateSolidBrush(pressed ? RGB(0xC8, 0xC6, 0xC0) : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    if (pressed) DrawEdge(dc, &rc, BDR_SUNKENOUTER, BF_RECT);

    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, Scale(2, dpi_), RGB(0x4C, 0xE4, 0x4C));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, cx - Scale(5, dpi_), cy - Scale(4, dpi_), nullptr);
    LineTo(dc, cx - Scale(1, dpi_), cy);
    LineTo(dc, cx - Scale(5, dpi_), cy + Scale(4, dpi_));
    MoveToEx(dc, cx + Scale(1, dpi_), cy + Scale(4, dpi_), nullptr);
    LineTo(dc, cx + Scale(6, dpi_), cy + Scale(4, dpi_));
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Hover кнопок тулбара: TrackMouseEvent, подсветка при наведении.
LRESULT CALLBACK App::TbBtnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                DWORD_PTR refData) {
    App* app = reinterpret_cast<App*>(refData);
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        const int idx = GetDlgCtrlID(hwnd) - ID_TB_FIRST;
        if (app->hoverBtn_ != idx) {
            app->hoverBtn_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        const int idx = GetDlgCtrlID(hwnd) - ID_TB_FIRST;
        if (app->hoverBtn_ == idx) {
            app->hoverBtn_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Цветной значок тулбара по команде: рисуется кодом (MakeToolIcon) и кэшируется.
HICON App::ToolIcon(int command) {
    if (auto it = toolIcons_.find(command); it != toolIcons_.end()) return it->second;
    int kind = -1;
    switch (command) {
        case IDM_NAV_BACK:     kind = TIK_BACK; break;
        case IDM_NAV_FORWARD:  kind = TIK_FWD; break;
        case IDM_NAV_UP:       kind = TIK_UP; break;
        case IDM_TREE:         kind = TIK_TREE; break;
        case IDM_VIEW:         kind = TIK_VIEW; break;
        case IDM_EDIT:         kind = TIK_EDIT; break;
        case IDM_COPY:         kind = TIK_COPY; break;
        case IDM_MOVE:         kind = TIK_MOVE; break;
        case IDM_MKDIR:        kind = TIK_MKDIR; break;
        case IDM_DELETE:       kind = TIK_DEL; break;
        case IDM_PACK:         kind = TIK_PACK; break;
        case IDM_UNPACK:       kind = TIK_UNPACK; break;
        case IDM_BATCH_RENAME: kind = TIK_RENM; break;
        case IDM_SEARCH:       kind = TIK_FIND; break;
        case IDM_SYNC:         kind = TIK_SYNC; break;
        case IDM_REFRESH:      kind = TIK_REFRESH; break;
        case IDM_PROPERTIES:   kind = TIK_PROPS; break;
        default: break;
    }
    HICON icon = nullptr;
    if (kind >= 0) icon = MakeToolIcon(kind, Scale(16, dpi_));
    toolIcons_[command] = icon;
    return icon;
}

// Подписи под ряд F-клавиш меняются при нажатом Shift/Ctrl/Alt.
void App::UpdateFKeyLabels(int kind) {
    if (kind == modKind_) return;
    modKind_ = kind;
    for (int i = 0; i < kFKeyCount; ++i) {
        const FKeyLabel& lab = kFKeyLabels[i];
        const wchar_t* text = (kind == 3) ? lab.alt : (kind == 2 ? lab.ctrl : (kind == 1 ? lab.shift : lab.normal));
        SetWindowTextW(fkeys_[i], text);
        InvalidateRect(fkeys_[i], nullptr, TRUE);
    }
}

// Какая команда соответствует кнопке F-клавиши при текущих модификаторах.
int App::FKeyCommand(int index, int kind) const {
    if (index < 0 || index >= kFKeyCount) return 0;
    if (kind == 3) {
        switch (index) {
            case 1: return IDM_EXIT;
            case 2: return IDM_PACK;
            case 4: return IDM_SEARCH;
            default: break;
        }
    } else if (kind == 2) {
        switch (index) {
            case 0: return IDM_SORT_NAME;
            case 1: return IDM_SORT_EXT;
            case 2: return IDM_SORT_SIZE;
            case 3: return IDM_SORT_DATE;
            case 5: return IDM_TREE;
            default: break;
        }
    } else if (kind == 1) {
        switch (index) {
            case 1: return IDM_NEWFILE;
            case 2: return IDM_COPY;
            case 3: return IDM_RENAME;
            case 6: return IDM_DIRSIZE;
            default: break;
        }
    }
    return kFKeys[index].command;
}

// Ctrl+Q — быстрый просмотр на неактивной панели, как в TC.
void App::ToggleQuickView() {
    if (quickView_) {
        DestroyWindow(quickView_);
        quickView_ = nullptr;
        quickViewSide_ = -1;
        ActivePanel()->FocusList();
        return;
    }
    quickView_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                     ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)2001, g_inst, nullptr);
    if (!quickView_) return;
    SendMessageW(quickView_, WM_SETFONT, (WPARAM)font_, TRUE);
    quickViewSide_ = (activeSide_ == 0) ? 1 : 0;
    Layout();
    RefreshQuickView();
    ActivePanel()->FocusList();
    LogDebug(L"quickview on side " + std::to_wstring(quickViewSide_));
}

void App::RefreshQuickView() {
    if (!quickView_) return;
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e || e->isUp || e->isDir) {
        SetWindowTextW(quickView_, L"");
        return;
    }
    if (panel->InArchive()) {
        SetWindowTextW(quickView_, L"<просмотр доступен только для файлов на диске>");
        return;
    }
    bool binary = false;
    const std::wstring text = LoadTextPreview(panel->FocusedPath(), 128 * 1024, binary);
    SetWindowTextW(quickView_, text.c_str());
    SendMessageW(quickView_, EM_SETSEL, 0, 0);
    SendMessageW(quickView_, EM_SCROLLCARET, 0, 0);
}

// Состояние на диск: окно, панели, вкладки, курсоры. Вызывается до DestroyWindow.
// Заголовок окна в духе TC: имя, дата и время, сеть, память, ЦП.
void App::UpdateTitle() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    static const wchar_t* kWeekday[7] = {L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб"};
    static const wchar_t* kMonth[12] = {L"янв", L"фев", L"мар", L"апр", L"мая", L"июн",
                                        L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек"};

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    int memUsedTenths = 0, memTotalTenths = 0, memPct = 0;
    if (GlobalMemoryStatusEx(&ms)) {
        const double totalGb = ms.ullTotalPhys / (1024.0 * 1024 * 1024);
        const double usedGb = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024 * 1024);
        memTotalTenths = (int)(totalGb * 10 + 0.5);
        memUsedTenths = (int)(usedGb * 10 + 0.5);
        memPct = (int)ms.dwMemoryLoad;
    }

    // ЦП: разница системных времён между обновлениями.
    int cpuPct = 0;
    FILETIME idle{}, kern{}, user{};
    if (GetSystemTimes(&idle, &kern, &user)) {
        auto ticks = [](const FILETIME& ft) {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            return u.QuadPart;
        };
        const ULONGLONG i = ticks(idle), k = ticks(kern), u = ticks(user);
        if (titleCpuKernel_ || titleCpuUser_) {
            const ULONGLONG di = i - titleCpuIdle_;
            const ULONGLONG dk = k - titleCpuKernel_;
            const ULONGLONG du = u - titleCpuUser_;
            const ULONGLONG span = dk + du;
            if (span > 0) cpuPct = (int)((span - di) * 100 / span);
        }
        titleCpuIdle_ = i;
        titleCpuKernel_ = k;
        titleCpuUser_ = u;
    }

    // Сеть: сколько байт прошло за секунду, в Мбит/с. Счётчики 32-битные, разность считаем
    // по модулю 2^32 — так переполнение не даёт всплесков.
    double downMbit = 0.0, upMbit = 0.0;
    {
        DWORD size = 0;
        if (GetIfTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER && size > 0) {
            std::vector<BYTE> raw(size);
            auto* table = reinterpret_cast<MIB_IFTABLE*>(raw.data());
            if (GetIfTable(table, &size, FALSE) == NO_ERROR) {
                std::vector<IfCounter> now;
                DWORD bestDown = 0, bestUp = 0;
                for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                    const MIB_IFROW& row = table->table[i];
                    if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                    if (row.dwOperStatus != MIB_IF_OPER_STATUS_OPERATIONAL) continue;

                    IfCounter counter;
                    counter.index = row.dwIndex;
                    counter.in = row.dwInOctets;
                    counter.out = row.dwOutOctets;
                    now.push_back(counter);

                    for (const auto& prev : netCounters_) {
                        if (prev.index != row.dwIndex) continue;
                        const DWORD down = (DWORD)(row.dwInOctets - prev.in);
                        const DWORD up = (DWORD)(row.dwOutOctets - prev.out);
                        if (down > bestDown) bestDown = down;
                        if (up > bestUp) bestUp = up;
                        break;
                    }
                }
                if (!netCounters_.empty()) {
                    // Берём самый нагруженный интерфейс: виртуальные адаптеры дублируют трафик.
                    downMbit = (double)bestDown * 8.0 / 1000000.0;
                    upMbit = (double)bestUp * 8.0 / 1000000.0;
                }
                netCounters_ = std::move(now);
            }
        }
    }

    wchar_t buf[512]{};
    swprintf(buf, 512,
             L"%ls - Win32   %ls %02d %ls %04d %02d:%02d:%02d   Сеть: %d,%d/%d,%d Мбит/с   "
             L"Память: %d,%d/%d,%d ГБ (%d%%)   ЦП: %d%%",
             kTitle, kWeekday[st.wDayOfWeek], st.wDay, kMonth[st.wMonth - 1], st.wYear, st.wHour,
             st.wMinute, st.wSecond, (int)downMbit, (int)((downMbit - (int)downMbit) * 10), (int)upMbit,
             (int)((upMbit - (int)upMbit) * 10), memUsedTenths / 10, memUsedTenths % 10,
             memTotalTenths / 10, memTotalTenths % 10, memPct, cpuPct);
    SetWindowTextW(hwnd_, buf);
}

// Пересобрать ряды дисков в обеих панелях (после подключения/отключения сети).
void App::RebuildDriveBars() {
    left_.RebuildDriveBar();
    right_.RebuildDriveBar();
}

// Обзор сети средствами оболочки: можно дойти до сервера и его ресурсов.
void App::BrowseNetwork() {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Сеть: выберите папку";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[32768]{};
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok || !path[0]) return;

    LogDebug(L"network browse: '" + std::wstring(path) + L"'");
    if (!ActivePanel()->Navigate(path)) ShowError(ActivePanel()->NavigateError());
    ActivePanel()->FocusList();
}

// Перейти по UNC-пути, введённому вручную (\\сервер\ресурс или \\сервер).
void App::OpenUncPath() {
    std::wstring path = L"\\\\";
    if (!InputBox(hwnd_, L"Сеть", L"UNC-путь (например \\\\сервер\\ресурс):", path, true)) return;
    path = Trim(path);
    if (path.empty()) return;
    if (!ActivePanel()->Navigate(path)) ShowError(ActivePanel()->NavigateError());
    ActivePanel()->FocusList();
}

// Подключить сетевой диск: WNetAddConnection2 с выбранной буквой.
void App::MapNetworkDrive() {
    std::wstring remote = L"\\\\сервер\\ресурс";
    if (!InputBox(hwnd_, L"Подключить сетевой диск", L"Сетевой путь (\\\\сервер\\ресурс):", remote, false)) return;
    remote = Trim(remote);
    if (remote.empty()) return;

    std::wstring letter = L"Z:";
    if (!InputBox(hwnd_, L"Подключить сетевой диск", L"Буква диска (например Z:):", letter, false)) return;
    letter = Upper(Trim(letter));
    if (!letter.empty() && letter.back() != L':') letter += L':';
    if (letter.size() != 2 || letter[1] != L':' || letter[0] < L'A' || letter[0] > L'Z') {
        ShowError(L"Нужна буква диска, например Z:");
        return;
    }

    wchar_t local[4] = {letter[0], L':', 0, 0};
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = local;
    nr.lpRemoteName = const_cast<wchar_t*>(remote.c_str());

    const DWORD rc = WNetAddConnection2W(&nr, nullptr, nullptr, CONNECT_TEMPORARY);
    LogDebug(L"WNetAddConnection2 '" + remote + L"' -> " + letter + L" rc=" + std::to_wstring(rc));
    if (rc != NO_ERROR) {
        ShowError(L"Не удалось подключить «" + remote + L"» как " + letter + L":\n" + ErrText(rc));
        return;
    }
    RebuildDriveBars();
    const std::wstring root = letter + L"\\";
    if (!ActivePanel()->Navigate(root)) LogDebug(L"map: переход в " + root + L" не удался");
    ActivePanel()->FocusList();
}

// Отключить сетевой диск по букве.
void App::UnmapNetworkDrive() {
    std::wstring letter = L"Z:";
    if (!InputBox(hwnd_, L"Отключить сетевой диск", L"Буква диска (например Z:):", letter, false)) return;
    letter = Upper(Trim(letter));
    if (!letter.empty() && letter.back() != L':') letter += L':';
    if (letter.size() != 2) {
        ShowError(L"Нужна буква диска, например Z:");
        return;
    }

    const DWORD rc = WNetCancelConnection2W(letter.c_str(), 0, TRUE);
    LogDebug(L"WNetCancelConnection2 " + letter + L" rc=" + std::to_wstring(rc));
    if (rc != NO_ERROR) {
        ShowError(L"Не удалось отключить " + letter + L"\n" + ErrText(rc));
        return;
    }
    RebuildDriveBars();
    // Если панель стояла на этом диске — уходим на первый доступный.
    for (Panel* panel : {&left_, &right_}) {
        if (!panel->Path().empty() && towupper(panel->Path()[0]) == letter[0]) {
            panel->Navigate(ExeDir());
        }
    }
}

void App::SaveState() {
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
    st.leftTabNames.assign(left_.TabTitles().begin(), left_.TabTitles().end());
    st.rightTabNames.assign(right_.TabTitles().begin(), right_.TabTitles().end());
    st.leftTabLocks.clear();
    for (bool locked : left_.TabLocks()) st.leftTabLocks.push_back(locked ? 1 : 0);
    st.rightTabLocks.clear();
    for (bool locked : right_.TabLocks()) st.rightTabLocks.push_back(locked ? 1 : 0);
    st.activeSide = activeSide_;
    st.leftCursor = left_.CursorIndex();
    st.rightCursor = right_.CursorIndex();
    st.leftMask = left_.Mask();
    st.rightMask = right_.Mask();
    SaveSettings();
}

void App::RunCommandLine() {
    wchar_t buf[4096]{};
    GetWindowTextW(prompt_, buf, 4096);
    std::wstring command = Trim(buf);
    if (command.empty()) return;

    // История командной строки: до 64 строк, без повтора подряд, как в TC.
    if (history_.empty() || history_.back() != command) {
        history_.push_back(command);
        if (history_.size() > 64) history_.erase(history_.begin());
        SendMessageW(prompt_, CB_ADDSTRING, 0, (LPARAM)command.c_str());
        GetSettings().commandHistory = history_;
    }
    histPos_ = -1;
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

// Закладки каталогов: список хранится в настройках.
// Распаковка одного файла из архива во временный каталог (для просмотра и запуска).
std::wstring App::ExtractOneFromArchive(Panel* panel, const std::wstring& innerName) {
    const std::wstring dir = ArchiveTempDir();
    std::wstring rel = innerName;
    for (auto& ch : rel) {
        if (ch == L'/') ch = L'\\';
    }

    std::wstring error;
    if (!ExtractFromArchive(panel->ArchiveFile(), {innerName}, dir, error)) {
        ShowError(error);
        return {};
    }
    return dir + L"\\" + rel;
}

// Какая панель под точкой экрана: 0 — левая, 1 — правая, -1 — мимо.
int App::PanelAtScreen(POINT screen) const {
    for (int side = 0; side < 2; ++side) {
        HWND h = (side == 0) ? left_.Hwnd() : right_.Hwnd();
        RECT rc{};
        if (!h || !GetWindowRect(h, &rc)) continue;
        if (screen.x >= rc.left && screen.x < rc.right && screen.y >= rc.top && screen.y < rc.bottom) return side;
    }
    return -1;
}

// Перетаскивание между панелями: по умолчанию копирование, с Shift — перемещение.
void App::DropOnPanel(int fromSide, int toSide) {
    if (fromSide == toSide) return;

    Panel* src = (fromSide == 0) ? &left_ : &right_;
    Panel* dst = (fromSide == 0) ? &right_ : &left_;

    std::vector<std::wstring> items = src->SelectedPaths();
    LogDebug(L"drop items=" + std::to_wstring(items.size()) + L" target='" + dst->Path() + L"'");
    if (items.empty()) return;

    const bool move = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const std::wstring target = dst->Path();
    if (target.empty()) return;

    if (ShellCopyMove(hwnd_, items, target, move)) {
        src->ClearMarks();
    } else {
        ShowError(move ? L"Перемещение не выполнено или отменено." : L"Копирование не выполнено или отменено.");
    }
    src->Reload();
    dst->Reload();
    UpdatePromptLabel();
}

// Упаковка выделенного в архив: имя и формат задаются в диалоге.
void App::PackSelection() {
    Panel* panel = ActivePanel();
    if (!panel) return;

    if (panel->InArchive()) {
        MessageBoxW(hwnd_, L"Упаковка из архива не поддерживается.", kTitle, MB_ICONINFORMATION);
        return;
    }

    std::vector<std::wstring> items = panel->SelectedPaths();
    LogDebug(L"pack items=" + std::to_wstring(items.size()) + L" panel='" + panel->Path() + L"'");
    for (auto& it : items) LogDebug(L"  item: " + it);
    if (items.empty()) return;

    std::wstring target = JoinPath(panel->Path(), L"архив.zip");
    if (!InputBox(hwnd_, L"Упаковка", L"Имя архива (.zip или .7z):", target, true)) return;
    target = Trim(target);
    if (target.empty()) return;

    std::wstring error;
    if (!CreateArchive(target, items, error)) {
        ShowError(error);
        return;
    }
    panel->ClearMarks();
    panel->Reload();
    UpdatePromptLabel();
}

// ---------- очередь операций ----------

void App::QueueCopyMove(const std::vector<std::wstring>& items, const std::wstring& target, bool move,
                        bool overwrite) {
    if (items.empty() || target.empty()) return;

    CopyJob job;
    job.items = items;
    job.target = target;
    job.move = move;
    job.overwrite = overwrite;
    job.status = L"ожидает";
    jobs_.push_back(std::move(job));

    LogDebug(L"queue: добавлено задание, всего=" + std::to_wstring(jobs_.size()));
    RefreshQueueWindow();

    if (activeJob_ < 0) RunNextJob();
}

void App::RunNextJob() {
    activeJob_ = -1;
    for (size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].status == L"ожидает") {
            activeJob_ = (int)i;
            break;
        }
    }
    if (activeJob_ < 0) {
        RefreshQueueWindow();
        return;
    }

    CopyJob& job = jobs_[activeJob_];
    job.status = L"выполняется";
    RefreshQueueWindow();

    const bool ok = ShellCopyMove(hwnd_, job.items, job.target, job.move, false, job.overwrite);
    job.status = ok ? L"готово" : L"ошибка";

    left_.Reload();
    right_.Reload();
    UpdatePromptLabel();
    RefreshQueueWindow();

    // Следующее задание запускаем из цикла сообщений, а не рекурсией.
    PostMessageW(hwnd_, WM_APP + 401, 0, 0);
}

// Пакетное переименование: {name} — исходное имя без расширения, {n} — номер по порядку.
void App::BatchRename() {
    Panel* panel = ActivePanel();
    if (!panel) return;

    if (panel->InArchive()) {
        MessageBoxW(hwnd_, L"Переименование внутри архива не поддерживается.", kTitle, MB_ICONINFORMATION);
        return;
    }

    std::vector<std::wstring> names = panel->SelectedNames();
    if (names.empty()) return;

    std::wstring pattern = L"{name}_{n}";
    if (!InputBox(hwnd_, L"Пакетное переименование",
                  L"Шаблон: {name} — имя, {n} — номер (с 1):", pattern, false)) {
        return;
    }
    pattern = Trim(pattern);
    if (pattern.empty()) return;

    const int digits = (int)std::to_wstring(names.size()).size();
    int done = 0;
    int failed = 0;
    std::wstring firstError;

    for (size_t i = 0; i < names.size(); ++i) {
        const std::wstring& oldName = names[i];

        std::wstring base = oldName;
        std::wstring ext;
        const size_t dot = oldName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            base = oldName.substr(0, dot);
            ext = oldName.substr(dot);
        }

        std::wstring number = std::to_wstring(i + 1);
        while ((int)number.size() < digits) number.insert(number.begin(), L'0');

        std::wstring newName = pattern;
        auto replaceAll = [&](const std::wstring& what, const std::wstring& with) {
            size_t pos = 0;
            while ((pos = newName.find(what, pos)) != std::wstring::npos) {
                newName.replace(pos, what.size(), with);
                pos += with.size();
            }
        };
        replaceAll(L"{name}", base);
        replaceAll(L"{n}", number);

        if (newName.empty() || newName == oldName) continue;
        newName += ext;

        DWORD err = 0;
        const std::wstring from = JoinPath(panel->Path(), oldName);
        const std::wstring to = JoinPath(panel->Path(), newName);
        if (RenamePath(from, to, err)) {
            ++done;
        } else {
            ++failed;
            if (firstError.empty()) firstError = oldName + L": " + ErrText(err);
        }
    }

    panel->ClearMarks();
    panel->Reload();
    UpdatePromptLabel();

    std::wstring report = L"Переименовано: " + std::to_wstring(done);
    if (failed) report += L"\nНе удалось: " + std::to_wstring(failed) + L"\n" + firstError;
    MessageBoxW(hwnd_, report.c_str(), L"Пакетное переименование", MB_ICONINFORMATION);
}

void App::AddBookmark() {
    const std::wstring path = ActivePanel()->Path();
    if (path.empty()) return;

    Settings& st = GetSettings();
    for (auto& b : st.bookmarks) {
        if (SamePath(b, path)) {
            MessageBoxW(hwnd_, L"Этот каталог уже в закладках.", L"Закладки", MB_ICONINFORMATION);
            return;
        }
    }
    st.bookmarks.push_back(path);
    SaveSettings();
}

void App::ShowBookmarksMenu() {
    Settings& st = GetSettings();
    if (st.bookmarks.empty()) {
        MessageBoxW(hwnd_, L"Закладок пока нет. Нажмите Ctrl+D, чтобы добавить текущий каталог.",
                    L"Закладки", MB_ICONINFORMATION);
        return;
    }

    HMENU popup = CreatePopupMenu();
    for (size_t i = 0; i < st.bookmarks.size() && i < 200; ++i) {
        AppendMenuW(popup, MF_STRING, 2000 + (UINT)i, st.bookmarks[i].c_str());
    }

    POINT pt{};
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(popup);

    if (cmd >= 2000) {
        size_t index = (size_t)(cmd - 2000);
        if (index < st.bookmarks.size()) {
            if (!ActivePanel()->Navigate(st.bookmarks[index])) ShowError(ActivePanel()->NavigateError());
        }
    }
}

LRESULT CALLBACK App::QueueProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {

            auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            switch (msg) {
                case WM_CREATE: {
                    auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    app = reinterpret_cast<App*>(cs->lpCreateParams);
                    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)app);
                    app->queueWnd_ = h;

                    app->queueList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                                      0, 0, 10, 10, h, (HMENU)(INT_PTR)1, g_inst, nullptr);
                    ListView_SetExtendedListViewStyle(app->queueList_,
                                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                    LVCOLUMNW col{};
                    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    col.cx = 70; col.iSubItem = 0; col.pszText = (LPWSTR)L"Операция";
                    ListView_InsertColumn(app->queueList_, 0, &col);
                    col.cx = 120; col.iSubItem = 1; col.pszText = (LPWSTR)L"Что";
                    ListView_InsertColumn(app->queueList_, 1, &col);
                    col.cx = 300; col.iSubItem = 2; col.pszText = (LPWSTR)L"Куда";
                    ListView_InsertColumn(app->queueList_, 2, &col);
                    col.cx = 110; col.iSubItem = 3; col.pszText = (LPWSTR)L"Состояние";
                    ListView_InsertColumn(app->queueList_, 3, &col);

                    app->queueStatus_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                        0, 0, 10, 10, h, (HMENU)(INT_PTR)2, g_inst, nullptr);
                    if (app->font_) {
                        SendMessageW(app->queueList_, WM_SETFONT, (WPARAM)app->font_, TRUE);
                        SendMessageW(app->queueStatus_, WM_SETFONT, (WPARAM)app->font_, TRUE);
                    }

                    SetTimer(h, 1, 400, nullptr);
                    app->RefreshQueueWindow();
                    return 0;
                }
                case WM_SIZE: {
                    if (!app) break;
                    const int w = LOWORD(lp), hgt = HIWORD(lp), pad = 8, statusH = 22;
                    MoveWindow(app->queueList_, pad, pad, w - 2 * pad, hgt - 2 * pad - statusH, TRUE);
                    MoveWindow(app->queueStatus_, pad, hgt - statusH - pad, w - 2 * pad, statusH, TRUE);
                    return 0;
                }
                case WM_TIMER:
                    if (app) app->RefreshQueueWindow();
                    return 0;
                case WM_CLOSE:
                    DestroyWindow(h);
                    return 0;
                case WM_DESTROY:
                    if (app) {
                        app->queueWnd_ = nullptr;
                        app->queueList_ = nullptr;
                        app->queueStatus_ = nullptr;
                    }
                    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                    return 0;
                default:
                    break;
            }
            return DefWindowProcW(h, msg, wp, lp);
}

void App::ShowQueueWindow() {
    if (queueWnd_ && IsWindow(queueWnd_)) {
        SetForegroundWindow(queueWnd_);
        RefreshQueueWindow();
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = QueueProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"POSQueue2";
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        registered = RegisterClassExW(&wc) != 0;
    }

    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    CreateWindowExW(WS_EX_TOOLWINDOW, L"POSQueue2", L"Очередь операций",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, rc.left + 120, rc.top + 120, 700, 340,
                    nullptr, nullptr, g_inst, this);
}

void App::RefreshQueueWindow() {
    if (!queueWnd_ || !IsWindow(queueWnd_)) return;

    if (queueList_) {
        ListView_DeleteAllItems(queueList_);
        int row = 0;
        for (auto& job : jobs_) {
            std::wstring what = job.items.size() == 1
                                    ? job.items[0].substr(job.items[0].find_last_of(L'\\') + 1)
                                    : std::to_wstring(job.items.size()) + L" объектов";

            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = (LPWSTR)(job.move ? L"перемещение" : L"копирование");
            int index = ListView_InsertItem(queueList_, &item);
            if (index >= 0) {
                ListView_SetItemText(queueList_, index, 1, (LPWSTR)what.c_str());
                ListView_SetItemText(queueList_, index, 2, (LPWSTR)job.target.c_str());
                ListView_SetItemText(queueList_, index, 3, (LPWSTR)job.status.c_str());
            }
            ++row;
        }
    }

    if (queueStatus_) {
        size_t waiting = 0, done = 0, failed = 0;
        for (auto& job : jobs_) {
            if (job.status == L"ожидает") ++waiting;
            else if (job.status == L"готово") ++done;
            else if (job.status == L"ошибка") ++failed;
        }
        wchar_t buf[256]{};
        swprintf(buf, 256, L"Всего: %zu   ·   ожидает: %zu   ·   готово: %zu   ·   ошибок: %zu",
                 jobs_.size(), waiting, done, failed);
        SetWindowTextW(queueStatus_, buf);
    }
}

void App::Dispatch(int id) {
    LogDebug(L"Dispatch id=" + std::to_wstring(id));
    Panel* panel = ActivePanel();
    switch (id) {
        case IDM_NET_BROWSE:
            BrowseNetwork();
            break;
        case IDM_NET_UNC:
            OpenUncPath();
            break;
        case IDM_NET_MAP:
            MapNetworkDrive();
            break;
        case IDM_NET_UNMAP:
            UnmapNetworkDrive();
            break;
        case IDM_SEARCH: {
            std::wstring found = SearchDialog(hwnd_, ActivePanel()->Path());
            LogDebug(L"search chosen='" + found + L"'");
            if (!found.empty()) {
                const size_t slash = found.find_last_of(L'\\');
                if (slash != std::wstring::npos) {
                    const std::wstring dir = found.substr(0, slash);
                    const std::wstring name = found.substr(slash + 1);
                    if (ActivePanel()->Navigate(dir, name)) ActivePanel()->FocusList();
                }
            }
            break;
        }
        case IDM_QUICKVIEW:
            ToggleQuickView();
            break;
        case IDM_NEWFILE: {
            if (panel->InArchive()) {
                ShowError(L"В архиве новый файл не создать.");
                break;
            }
            std::wstring name = L"новый файл.txt";
            if (!InputBox(hwnd_, L"Новый файл", L"Имя файла:", name, false)) break;
            name = Trim(name);
            if (name.empty()) break;
            const std::wstring path = JoinPath(panel->Path(), name);
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                ShowError(L"Не удалось создать файл:\n" + path + L"\n\n" + ErrText(GetLastError()));
                break;
            }
            CloseHandle(file);
            LogDebug(L"new file: " + path);
            panel->Navigate(panel->Path(), name);
            ShellExecuteW(hwnd_, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_VIEW: {
            Entry* e = panel->Cursor();
            if (!e || e->isUp || e->isDir) {
                MessageBoxW(hwnd_, L"Просмотр доступен только для файлов.", kTitle, MB_ICONINFORMATION);
                return;
            }
            if (panel->InArchive()) {
                std::wstring inner = panel->ArchiveInner().empty() ? e->name : panel->ArchiveInner() + L"\\" + e->name;
                std::wstring temp = ExtractOneFromArchive(panel, inner);
                if (!temp.empty()) ShowLister(hwnd_, temp);
                break;
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
        case IDM_DIRSIZE: {
            Panel* p = ActivePanel();
            int index = p->CursorIndex();
            if (index >= 0) p->CalculateDirSize(index);
            break;
        }
        case IDM_QUEUE:
            ShowQueueWindow();
            break;
        case IDM_PLUGINS:
            PluginsDialog(hwnd_);
            break;
        case IDM_BATCH_RENAME:
            BatchRename();
            break;
        case IDM_PACK:
            PackSelection();
            break;
        case IDM_UNPACK:
            MessageBoxW(hwnd_, L"Распаковка архивов будет добавлена в следующей вехе.",
                        kTitle, MB_ICONINFORMATION);
            break;
        case IDM_SYNC:
            MessageBoxW(hwnd_, L"Синхронизация каталогов будет добавлена в следующей вехе.",
                        kTitle, MB_ICONINFORMATION);
            break;
        case IDM_TREE:
            panel->ToggleTree();
            break;
        case IDM_BOOKMARK_ADD:
            AddBookmark();
            break;
        case IDM_BOOKMARK_SHOW:
            ShowBookmarksMenu();
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
        case IDM_TAB_NEW:
            panel->AddTab();
            break;
        case IDM_TAB_CLOSE:
            panel->CloseTab();
            break;
        case IDM_TAB_CLOSEALL:
            panel->CloseAllTabs();
            break;
        case IDM_TAB_LOCK:
            panel->ToggleTabLock(panel->ActiveTab());
            break;
        case IDM_TAB_COPY:
            PostMessageW(hwnd_, WM_APP + 302, (WPARAM)panel->Side(), (LPARAM)panel->ActiveTab());
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
    // Пока идёт правка пути, Enter и Esc работают на поле, даже если фокус успел уйти.
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
        if (msg.wParam == VK_ESCAPE) {
            if (left_.PathEditing()) {
                left_.CancelPathEdit();
                return true;
            }
            if (right_.PathEditing()) {
                right_.CancelPathEdit();
                return true;
            }
            if (left_.MaskEditing()) {
                left_.CancelMaskEdit();
                return true;
            }
            if (right_.MaskEditing()) {
                right_.CancelMaskEdit();
                return true;
            }
        }
        if (msg.wParam == VK_RETURN) {
            if (left_.PathEditing()) {
                left_.ConfirmPathEdit();
                return true;
            }
            if (right_.PathEditing()) {
                right_.ConfirmPathEdit();
                return true;
            }
            if (left_.MaskEditing()) {
                left_.ConfirmMaskEdit();
                return true;
            }
            if (right_.MaskEditing()) {
                right_.ConfirmMaskEdit();
                return true;
            }
        }
    }

    // В режиме правки пути или маски клавиши (в том числе Backspace и Delete) идут в поле, а не в панель.
    HWND focusNow = GetFocus();
    if (focusNow && (focusNow == left_.PathEdit() || focusNow == right_.PathEdit() ||
                     focusNow == left_.MaskEdit() || focusNow == right_.MaskEdit())) {
        const bool inMask = focusNow == left_.MaskEdit() || focusNow == right_.MaskEdit();
        const bool editing = inMask ? (focusNow == left_.MaskEdit() ? left_.MaskEditing() : right_.MaskEditing())
                                    : (focusNow == left_.PathEdit() ? left_.PathEditing()
                                                                    : right_.PathEditing());
        if (editing) {
            // Пока идёт правка, клавиши (включая стрелки и Ctrl+A/C/V) обрабатывает само поле.
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN || msg.message == WM_CHAR ||
                msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
                return false;
            }
        }
    }

    // Подписи F-клавиш: Shift/Ctrl/Alt меняют надписи, как в TC.
    if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP || msg.message == WM_SYSKEYDOWN ||
        msg.message == WM_SYSKEYUP) {
        const bool sh = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ct = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool al = (GetKeyState(VK_MENU) & 0x8000) != 0;
        UpdateFKeyLabels(al ? 3 : (ct ? 2 : (sh ? 1 : 0)));
    }
    // Быстрый поиск по буквам: WM_CHAR даёт символ с учётом раскладки.
    if (msg.message == WM_CHAR) {
        HWND focus = GetFocus();
        const bool inList = focus && (focus == left_.List() || focus == right_.List());
        // Пробел — это отметка файла (как в TC), а не быстрый поиск.
        if (inList && msg.wParam > 32 && msg.wParam != 127) {
            const ULONGLONG now = GetTickCount64();
            if (now - quickFilterAt_ > 900) quickFilter_.clear();
            quickFilterAt_ = now;
            quickFilter_.push_back((wchar_t)msg.wParam);

            Panel* target = ActivePanel();
            if (!target->FocusPrefix(quickFilter_) && quickFilter_.size() > 1) {
                quickFilter_.pop_back();
                target->FocusPrefix(quickFilter_);
            }
            return true;
        }
        return false;
    }

    // Alt+клавиша приходит как WM_SYSKEYDOWN, обычная — как WM_KEYDOWN.
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
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
            case VK_UP:                        // история: назад по списку
                if (!history_.empty()) {
                    if (histPos_ < 0) histPos_ = (int)history_.size() - 1;
                    else if (histPos_ > 0) histPos_--;
                    SetWindowTextW(prompt_, history_[histPos_].c_str());
                    SendMessageW(prompt_, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
                }
                return true;
            case VK_DOWN:                      // история: вперёд, дальше — пустая строка
                if (!history_.empty() && histPos_ >= 0) {
                    if (histPos_ + 1 < (int)history_.size()) {
                        histPos_++;
                        SetWindowTextW(prompt_, history_[histPos_].c_str());
                    } else {
                        histPos_ = -1;
                        SetWindowTextW(prompt_, L"");
                    }
                    SendMessageW(prompt_, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
                }
                return true;
            case VK_RETURN:
                RunCommandLine();
                return true;
            case VK_ESCAPE:
                SetWindowTextW(prompt_, L"");
                histPos_ = -1;
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

    if (alt && msg.wParam == VK_LEFT) {
        panel->GoBack();
        panel->FocusList();
        return true;
    }
    if (alt && msg.wParam == VK_RIGHT) {
        panel->GoForward();
        panel->FocusList();
        return true;
    }
    if (alt && msg.wParam == VK_F1) {
        ShowDrivesMenu(0);
        return true;
    }
    if (alt && msg.wParam == VK_F2) {
        ShowDrivesMenu(1);
        return true;
    }
    if (alt && shift && msg.wParam == VK_RETURN) {
        Dispatch(IDM_DIRSIZE);
        return true;
    }
    if (alt && msg.wParam == VK_F5) {
        Dispatch(IDM_PACK);
        return true;
    }
    if (alt && msg.wParam == VK_RETURN) {
        Dispatch(IDM_PROPERTIES);
        return true;
    }
    if (alt && msg.wParam == VK_F7) {
        Dispatch(IDM_SEARCH);
        return true;
    }

    // Прочие Alt+... отдаём системе (Alt+F4 закроет окно, Alt+Tab переключит задачу).
    if (alt) return false;

    // Ctrl-комбинации обрабатываем до обычных клавиш: Ctrl+F8 не должен стать F8-удалением.
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
            case 'D':
                AddBookmark();
                return true;
            case 'M':
                Dispatch(IDM_BATCH_RENAME);
                return true;
            case VK_F8:
                Dispatch(IDM_TREE);
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
            case VK_OEM_5:              // Ctrl+\ — в корень диска
                if (!panel->GoRoot()) ShowError(panel->NavigateError());
                panel->FocusList();
                return true;
            case VK_PRIOR:              // Ctrl+PgUp — на уровень вверх
                GoUpActive();
                panel->FocusList();
                return true;
            case VK_NEXT:               // Ctrl+PgDn — войти в каталог или архив
                OpenCursor();
                panel->FocusList();
                return true;
            case 'Q':                   // Ctrl+Q — быстрый просмотр на другой панели
                Dispatch(IDM_QUICKVIEW);
                return true;
            default:
                break;
        }
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
        case VK_ESCAPE:
            quickFilter_.clear();
            return true;
        case VK_DELETE:
            if (!inPrompt && !inList) break;
            Dispatch(IDM_DELETE);
            return true;
        default:
            break;
    }

    if (shift && msg.wParam == VK_F6) {
        Dispatch(IDM_RENAME);
        return true;
    }

    if (!inPrompt && (msg.wParam == VK_ADD || msg.wParam == VK_SUBTRACT || msg.wParam == VK_MULTIPLY)) {
        const int mode = (msg.wParam == VK_ADD) ? 0 : (msg.wParam == VK_SUBTRACT ? 1 : 2);
        const wchar_t* title = (mode == 0) ? L"Отметить по маске"
                                            : (mode == 1 ? L"Снять отметку по маске" : L"Инвертировать по маске");
        std::wstring mask = L"*.*";
        if (InputBox(hwnd_, title, L"Маска (несколько — через запятую):", mask, false)) {
            const size_t hit = panel->MarkByMask(mask, mode);
            LogDebug(L"mark by mask '" + mask + L"' mode=" + std::to_wstring(mode) + L" hit=" + std::to_wstring(hit));
        }
        panel->FocusList();
        return true;
    }

    if (shift && msg.wParam == VK_F4) {          // Shift+F4 — новый текстовый файл
        Dispatch(IDM_NEWFILE);
        return true;
    }

    if (msg.wParam == VK_F9) {                   // F9 — меню, как в TC
        PostMessageW(hwnd_, WM_SYSKEYDOWN, VK_F10, 1);
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

        case WM_TIMER:
            if (wParam == kTitleTimer) {
                UpdateTitle();
                return 0;
            }
            break;

        case WM_APP + 302: {
            // Вкладка скопирована на другую панель.
            Panel* src = ((int)wParam == 0) ? &left_ : &right_;
            Panel* dst = ((int)wParam == 0) ? &right_ : &left_;
            const int index = (int)lParam;
            std::vector<std::wstring> tabs;
            int active = 0;
            src->CaptureState(tabs, active);
            if (index >= 0 && index < (int)tabs.size()) dst->AddTabFor(tabs[index], L"");
            return 0;
        }

        case WM_APP + 301:
            // Панель сменила курсор: обновить быстрый просмотр, если он на другой панели.
            if (quickView_ && (int)wParam == activeSide_) RefreshQuickView();
            return 0;

        case WM_APP + 303:
            // Панель просит сделать её активной: фокус пришёл в её список (клик мышью)
            // или поле пути. Синхронно снимаем активность с другой панели.
            if ((int)wParam != activeSide_) SetActivePanel((int)wParam);
            return 0;

        case WM_APP + 401:
            RunNextJob();
            return 0;

        case WM_APP + 300:
            // Панель сообщила LVN_BEGINDRAG — ведём перетаскивание сами.
            dragging_ = true;
            dragFromSide_ = (int)wParam;
            LogDebug(L"drag begin side=" + std::to_wstring(dragFromSide_));
            SetCapture(hwnd_);
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

        case WM_DRAWITEM: {
            auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON) {
                const int id = (int)dis->CtlID;
                if ((id >= ID_TB_FIRST && id < ID_TB_FIRST + kToolbarCount) ||
                    (id >= ID_FKEY_FIRST && id < ID_FKEY_FIRST + kFKeyCount)) {
                    DrawFlatButton(dis);
                    return TRUE;
                }
                if (id == ID_TERM) {
                    DrawTermButton(dis);
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

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
                // Сепараторы — не кнопки, команду не диспатчат.
                if (kToolbar[id - ID_TB_FIRST].command) Dispatch(kToolbar[id - ID_TB_FIRST].command);
                return 0;
            }
            if (id >= ID_FKEY_FIRST && id < ID_FKEY_FIRST + kFKeyCount) {
                Dispatch(FKeyCommand(id - ID_FKEY_FIRST, modKind_ < 0 ? 0 : modKind_));
                return 0;
            }
            if (id == ID_TERM) {
                Dispatch(IDM_CONSOLE);
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

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count == 0) {
                DragFinish(drop);
                return 0;
            }

            POINT pt{};
            DragQueryPoint(drop, &pt);
            const int side = PanelAtScreen([&] {
                POINT screen = pt;
                ClientToScreen(hwnd_, &screen);
                return screen;
            }());

            if (side < 0) {
                DragFinish(drop);
                return 0;
            }

            Panel* target = (side == 0) ? &left_ : &right_;
            if (target->InArchive()) {
                DragFinish(drop);
                ShowError(L"Перетаскивание в архив не поддерживается.");
                return 0;
            }

            std::vector<std::wstring> sources;
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 4]{};
                if (DragQueryFileW(drop, i, path, MAX_PATH * 4) > 0) sources.push_back(path);
            }
            DragFinish(drop);

            const bool moveFiles = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!ShellCopyMove(hwnd_, sources, target->Path(), moveFiles)) {
                ShowError(moveFiles ? L"Перемещение не выполнено или отменено."
                                    : L"Копирование не выполнено или отменено.");
            }
            left_.Reload();
            right_.Reload();
            UpdatePromptLabel();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (dragging_) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd_, &pt);
                const int target = PanelAtScreen(pt);
                SetCursor(LoadCursorW(nullptr, (target >= 0 && target != dragFromSide_) ? IDC_HAND : IDC_NO));
                return 0;
            }
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
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd_, &pt);
                const int target = PanelAtScreen(pt);
                LogDebug(L"drag end target=" + std::to_wstring(target) + L" at " + std::to_wstring(pt.x) + L"," + std::to_wstring(pt.y));
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                if (target >= 0) DropOnPanel(dragFromSide_, target);
                return 0;
            }
            if (draggingSplit_) {
                draggingSplit_ = false;
                ReleaseCapture();
                return 0;
            }
            break;

        case WM_CLOSE:
            SaveState();
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY: {
            for (auto& kv : toolIcons_)
                if (kv.second) DestroyIcon(kv.second);
            toolIcons_.clear();
            if (boldFont_) {
                DeleteObject(boldFont_);
                boldFont_ = nullptr;
            }
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

    // Диагностика wcx: --wcx-test <путь к .wcx> <путь к архиву>
    if (cmdLine && wcsstr(cmdLine, L"--wcx-test")) {
        std::wstring args = cmdLine;
        size_t pos = args.find(L"--wcx-test");
        args = args.substr(pos + 10);
        while (!args.empty() && args.front() == L' ') args.erase(args.begin());

        std::vector<std::wstring> parts;
        size_t start = 0;
        while (start < args.size()) {
            while (start < args.size() && args[start] == L' ') ++start;
            if (start >= args.size()) break;
            if (args[start] == L'"') {
                const size_t end = args.find(L'"', start + 1);
                if (end == std::wstring::npos) {
                    parts.push_back(args.substr(start + 1));
                    break;
                }
                parts.push_back(args.substr(start + 1, end - start - 1));
                start = end + 1;
            } else {
                size_t end = args.find(L' ', start);
                if (end == std::wstring::npos) end = args.size();
                parts.push_back(args.substr(start, end - start));
                start = end;
            }
        }
        if (parts.size() >= 2) return WcxSelfTest(parts[0], parts[1]);
        return 2;
    }

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
