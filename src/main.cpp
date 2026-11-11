// Главное окно PosFiles: две панели, меню, командная строка, F-клавиши.
#include "common.h"
#include "dialogs.h"
#include "panel.h"
#include "util.h"
#include "ops.h"
#include "selftest.h"
#include "settings.h"
#include "version.h"
#include "search.h"
#include "dupes.h"
#include "rename.h"
#include "undo.h"
#include "diff.h"
#include "prefs.h"
#include "archive.h"
#include "sync.h"
#include "ftp.h"
#include "plugins.h"
#include "wcx.h"

#include <windowsx.h>
#include <winnetwk.h>
#include <ole2.h>

#include <commctrl.h>

#include <iphlpapi.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

HINSTANCE g_inst = nullptr;

namespace {

const wchar_t* kMainClass = L"PosFilesMain";
const wchar_t* kTitle = L"PosFiles";

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
    IDM_OPEN_NEW_TAB = 206,
    IDM_COPY_PATH = 207,
    IDM_DUPES = 208,
    IDM_UNDO_RENAME = 209,
    IDM_DIFF = 210,
    IDM_PREFS = 211,
    IDM_DELETE_PERM = 212,   // Shift+Del — мимо корзины
    IDM_BRANCH = 213,        // Ctrl+B — плоский список поддерева
    IDM_TAB_LIST = 214,      // Ctrl+Shift+A — список вкладок
    IDM_CMDHIST = 215,       // Alt+F8 — история команд
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
    IDM_FTP_CONNECT = 199,
    IDM_FTP_DISCONNECT = 200,

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
    IDM_SEVENZIP,
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
// Каталог команд, которые можно вынести на тулбар: имя для INI, подпись, команда.
struct TbNamed { const wchar_t* name; const wchar_t* text; int command; bool separator; };

const TbNamed kToolbarCatalog[] = {
    {L"back", L"Назад", IDM_NAV_BACK, false},
    {L"forward", L"Вперёд", IDM_NAV_FORWARD, false},
    {L"up", L"Вверх", IDM_NAV_UP, false},
    {L"tree", L"Дерево каталогов", IDM_TREE, false},
    {L"view", L"Просмотр (F3)", IDM_VIEW, false},
    {L"edit", L"Правка (F4)", IDM_EDIT, false},
    {L"copy", L"Копирование (F5)", IDM_COPY, false},
    {L"move", L"Перемещение (F6)", IDM_MOVE, false},
    {L"mkdir", L"Новый каталог (F7)", IDM_MKDIR, false},
    {L"delete", L"Удаление (F8)", IDM_DELETE, false},
    {L"pack", L"Упаковать (Alt+F5)", IDM_PACK, false},
    {L"unpack", L"Распаковать", IDM_UNPACK, false},
    {L"rename", L"Пакетное переименование (Ctrl+M)", IDM_BATCH_RENAME, false},
    {L"find", L"Поиск файлов", IDM_SEARCH, false},
    {L"sync", L"Синхронизация каталогов", IDM_SYNC, false},
    {L"refresh", L"Обновить (Ctrl+R)", IDM_REFRESH, false},
    {L"props", L"Свойства (Alt+Enter)", IDM_PROPERTIES, false},
    {L"dupes", L"Поиск дубликатов", IDM_DUPES, false},
    {L"diff", L"Сравнить файлы", IDM_DIFF, false},
    {L"undo", L"Отменить переименование", IDM_UNDO_RENAME, false},
    {L"prefs", L"Параметры", IDM_PREFS, false},
    {L"newtab", L"Открыть в новой вкладке", IDM_OPEN_NEW_TAB, false},
    {L"copypath", L"Копировать путь", IDM_COPY_PATH, false},
    {L"sep", L"", 0, true},
};

// Набор по умолчанию — как в Total Commander.
const wchar_t* kToolbarDefault[] = {L"back",  L"forward", L"up",     L"tree", L"sep", L"view",   L"edit",
                                    L"copy",  L"move",    L"mkdir",  L"delete", L"sep", L"pack", L"unpack",
                                    L"rename", L"sep",    L"find",   L"sync", L"refresh", L"sep", L"props"};

const int kMaxToolbar = 64;

class App;
class PanelDropTarget;


// ---------- OLE drag & drop: источник CF_HDROP ----------

class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* stg) override {
        if (!fmt || !stg) return E_INVALIDARG;
        if (fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;

        SIZE_T chars = 1;
        for (const auto& path : paths_) chars += path.size() + 1;
        const SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!mem) return E_OUTOFMEMORY;

        auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
        if (!df) {
            GlobalFree(mem);
            return E_OUTOFMEMORY;
        }
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
        for (const auto& path : paths_) {
            wcscpy(dst, path.c_str());
            dst += path.size() + 1;
        }
        *dst = 0;
        GlobalUnlock(mem);

        stg->tymed = TYMED_HGLOBAL;
        stg->hGlobal = mem;
        stg->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* fmt) override {
        if (fmt && fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &fmt, out);
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~FileDataObject() = default;
    LONG ref_ = 1;
    std::vector<std::wstring> paths_;
};

class FileDropSource final : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keyState) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ~FileDropSource() = default;
    LONG ref_ = 1;
};
// ---------- FTP: общие помощники копирования ----------

struct FtpCopyCtx {
    HWND owner = nullptr;
    FtpConnection* src = nullptr;       // null — локальный источник
    FtpConnection* dst = nullptr;       // null — локальный приёмник
    bool move = false;
    bool overwriteAll = false;
    bool skipExisting = false;
    std::vector<std::wstring> masks;
    std::wstring error;
    bool aborted = false;
    int files = 0;
    ULONGLONG bytesDone = 0;
};

bool ConnectFtpForUrl(const std::wstring& url, std::unique_ptr<FtpConnection>& conn, FtpSite& site,
                      std::wstring& remote, std::wstring& error) {
    remote.clear();
    if (!FtpParseUrl(url, site, remote)) {
        error = L"Некорректный FTP-адрес: " + url;
        return false;
    }
    if (!site.password.empty()) FtpRememberPassword(url, site.password);
    if (!site.user.empty() && !FtpPasswordFor(url, site.password)) {
        error = L"Нет пароля для " + site.user + L"@" + site.host + L".\n"
                L"Подключитесь через меню «Сеть» → «FTP: подключиться…».";
        return false;
    }
    return FtpConnect(site, conn, error);
}

bool RemotePathOfUrl(const std::wstring& url, std::wstring& remote) {
    FtpSite site;
    return FtpParseUrl(url, site, remote);
}

bool EnsureRemoteDir(FtpConnection& conn, const std::wstring& url, std::wstring& error) {
    std::wstring remote;
    if (!RemotePathOfUrl(url, remote)) {
        error = L"Некорректный FTP-адрес: " + url;
        return false;
    }
    std::wstring mkError;
    if (FtpCreateDir(conn, remote, mkError)) return true;
    std::vector<FtpEntryInfo> probe;
    std::wstring listError;
    if (FtpList(conn, remote, probe, listError)) return true;
    error = L"Не удалось создать каталог на FTP:\n" + url + L"\n\n" + mkError;
    return false;
}

bool EnsureLocalDir(const std::wstring& path, std::wstring& error) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS && IsDirectory(path)) return true;
    error = L"Не удалось создать каталог:\n" + path + L"\n\n" + ErrText(err);
    return false;
}

bool CopyFtpItem(FtpCopyCtx& c, const std::wstring& srcPath, const std::wstring& dstPath,
                 const std::wstring& name, bool isDir, bool topLevel) {
    if (c.aborted) return false;

    // Маска F5 ограничивает только выбранные файлы верхнего уровня; каталоги — целиком.
    if (!isDir && topLevel && !c.masks.empty()) {
        bool match = false;
        const std::wstring lowName = Lower(name);
        for (auto& mask : c.masks) {
            if (MatchMask(lowName, Lower(mask))) {
                match = true;
                break;
            }
        }
        if (!match) return true;
    }

    if (isDir) {
        if (c.dst) {
            if (!EnsureRemoteDir(*c.dst, dstPath, c.error)) return false;
        } else if (!EnsureLocalDir(dstPath, c.error)) {
            return false;
        }

        if (c.src) {
            std::wstring remote;
            if (!RemotePathOfUrl(srcPath, remote)) {
                c.error = L"Некорректный FTP-адрес: " + srcPath;
                return false;
            }
            std::vector<FtpEntryInfo> list;
            if (!FtpList(*c.src, remote, list, c.error)) return false;
            for (auto& e : list) {
                const std::wstring childSrc = c.src ? FtpJoinUrl(srcPath, e.name) : JoinPath(srcPath, e.name);
                const std::wstring childDst = c.dst ? FtpJoinUrl(dstPath, e.name) : JoinPath(dstPath, e.name);
                if (!CopyFtpItem(c, childSrc, childDst, e.name, e.isDir, false)) return false;
            }
        } else {
            std::vector<Entry> list;
            DWORD listError = 0;
            if (!ListDir(srcPath, true, list, listError)) {
                c.error = L"Не удалось прочитать каталог:\n" + srcPath + L"\n\n" + ErrText(listError);
                return false;
            }
            for (auto& e : list) {
                if (e.isUp) continue;
                if (!CopyFtpItem(c, JoinPath(srcPath, e.name), JoinPath(dstPath, e.name), e.name, e.isDir, false)) {
                    return false;
                }
            }
        }

        if (c.move) {
            if (c.src) {
                std::wstring remote;
                RemotePathOfUrl(srcPath, remote);
                std::wstring ignore;
                FtpRemoveDir(*c.src, remote, ignore);
            } else {
                RemoveDirectoryW(srcPath.c_str());
            }
        }
        return true;
    }

    // Файл: проверка перезаписи.
    bool exists = false;
    if (c.dst) {
        std::wstring remote;
        if (!RemotePathOfUrl(dstPath, remote)) {
            c.error = L"Некорректный FTP-адрес: " + dstPath;
            return false;
        }
        ULONGLONG size = 0;
        std::wstring ignore;
        exists = FtpFileSize(*c.dst, remote, size, ignore);
    } else {
        exists = GetFileAttributesW(dstPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    if (exists && !c.overwriteAll) {
        if (c.skipExisting) return true;
        const int answer = MessageBoxW(c.owner, (L"Перезаписать «" + name + L"»?").c_str(), L"FTP",
                                       MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) {
            c.aborted = true;
            c.error = L"Отменено";
            return false;
        }
        if (answer == IDNO) return true;
    }

    ULONGLONG fileTotal = 0;
    auto progress = [&name, &fileTotal, &c](ULONGLONG done, ULONGLONG total) {
        fileTotal = total;
        return FtpProgressUpdate(name, done, total, c.files, c.bytesDone);
    };

    if (c.src && c.dst) {
        // FTP → FTP: через временный файл.
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"posfiles-ftp-xfer";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring temp = dir + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(c.files) + L".tmp";
        std::wstring srcRemote, dstRemote;
        RemotePathOfUrl(srcPath, srcRemote);
        RemotePathOfUrl(dstPath, dstRemote);
        if (!FtpDownloadFile(*c.src, srcRemote, temp, progress, c.error)) return false;
        const bool uploaded = FtpUploadFile(*c.dst, temp, dstRemote, progress, c.error);
        DeleteFileW(temp.c_str());
        if (!uploaded) return false;
    } else if (c.src) {
        std::wstring srcRemote;
        RemotePathOfUrl(srcPath, srcRemote);
        if (!FtpDownloadFile(*c.src, srcRemote, dstPath, progress, c.error)) return false;
    } else if (c.dst) {
        std::wstring dstRemote;
        RemotePathOfUrl(dstPath, dstRemote);
        if (!FtpUploadFile(*c.dst, srcPath, dstRemote, progress, c.error)) return false;
    }

    ++c.files;
    c.bytesDone += fileTotal;
    if (c.move) {
        if (c.src) {
            std::wstring remote;
            RemotePathOfUrl(srcPath, remote);
            std::wstring ignore;
            FtpDeleteFile(*c.src, remote, ignore);
        } else {
            DeleteFileW(srcPath.c_str());
        }
    }
    return true;
}

void DeleteFtpTree(FtpConnection& conn, const std::wstring& url, const std::wstring& name, std::wstring& error,
                   bool& cancelled) {
    if (cancelled) return;
    FtpSite site;
    std::wstring remote;
    if (!FtpParseUrl(url, site, remote)) {
        error = L"Некорректный FTP-адрес: " + url;
        return;
    }
    if (!FtpProgressUpdate(name, 0, 0)) {
        cancelled = true;
        error = L"Отменено";
        return;
    }

    // Пробуем как файл; если сервер ответил «not a file», идём как по каталогу.
    std::wstring fileError;
    if (FtpDeleteFile(conn, remote, fileError)) return;

    std::vector<FtpEntryInfo> list;
    std::wstring listError;
    if (!FtpList(conn, remote, list, listError)) {
        error = L"Не удалось удалить «" + name + L"»:\n" + fileError;
        return;
    }
    for (auto& e : list) {
        const std::wstring child = FtpJoinUrl(url, e.name);
        DeleteFtpTree(conn, child, e.name, error, cancelled);
        if (cancelled || !error.empty()) return;
    }
    std::wstring rmError;
    if (!FtpRemoveDir(conn, remote, rmError)) error = rmError;
}

class App {
public:
    bool Create(HINSTANCE inst);
    int Run();
    DWORD OleDragEffect(POINTL pt, DWORD keyState) const;
    void OleDropFiles(POINTL pt, const std::vector<std::wstring>& files, bool move);

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
    void BuildToolbar();
    void OpenCursor();
    void ApplyUiSettings();
    void OpenCursorInNewTab();
    void CopyCursorPaths();
    void GoUpActive();
    void DoCopyMove(bool move);
    void DoFtpCopyMove(bool move);
    void DoRename();
    void DoMkdir(Panel* target = nullptr, const std::wstring& initialName = L"");
    void DoCopyInPlace();       // Shift+F5 — копия рядом под новым именем
    void DoDelete(bool permanent = false);
    void DoProperties();
    void DoSort(int column);
    void ShowDrivesMenu(int side);
    void AddBookmark();
    void PackSelection();
    void DoUnpack();
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
    void ShowCommandHistory();   // Alt+F8
    void ShowTabList();          // Ctrl+Shift+A
    void RunCommandLine();
    void SaveState();
    void UpdateTitle();
    void BrowseNetwork();
    void MapNetworkDrive();
    void UnmapNetworkDrive();
    void FtpConnectDialog();
    void FtpDisconnectActive();
    std::wstring DownloadFtpToTemp(const std::wstring& url, const std::wstring& name);
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
    PanelDropTarget* dropTarget_ = nullptr;
    HMENU menu_ = nullptr;
    HWND label_ = nullptr;
    HWND prompt_ = nullptr;
    HWND ver_ = nullptr;                // версия в правом верхнем углу, малозаметным шрифтом
    HFONT verFont_ = nullptr;           // мелкий шрифт для версии
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
    HWND toolbar_[kMaxToolbar]{};
    std::vector<TbDef> toolbarDefs_;   // состав тулбара из настроек
    int toolbarCount_ = 0;
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
    bool shiftHeld_ = false;            // Shift по сообщениям окна

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


// ---------- OLE drag & drop: приёмник на главном окне и списках ----------

class PanelDropTarget final : public IDropTarget {
public:
    explicit PanelDropTarget(App* app) : app_(app) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP DragEnter(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override {
        if (!effect) return E_POINTER;
        *effect = app_->OleDragEffect(pt, keyState);
        if (*effect != DROPEFFECT_NONE && data) {
            FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            if (data->QueryGetData(&fmt) != S_OK) *effect = DROPEFFECT_NONE;
        }
        accepted_ = (*effect != DROPEFFECT_NONE);
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD keyState, POINTL pt, DWORD* effect) override {
        if (!effect) return E_POINTER;
        *effect = accepted_ ? app_->OleDragEffect(pt, keyState) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        accepted_ = false;
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override {
        if (!effect) return E_POINTER;
        accepted_ = false;
        const DWORD targetEffect = app_->OleDragEffect(pt, keyState);
        std::vector<std::wstring> files;
        if (targetEffect == DROPEFFECT_NONE || !ReadFiles(data, files)) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        app_->OleDropFiles(pt, files, (targetEffect & DROPEFFECT_MOVE) != 0);
        *effect = targetEffect;
        return S_OK;
    }

private:
    ~PanelDropTarget() = default;

    static bool ReadFiles(IDataObject* data, std::vector<std::wstring>& files) {
        if (!data) return false;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (data->GetData(&fmt, &stg) != S_OK) return false;
        if (!stg.hGlobal) return false;
        auto drop = static_cast<HDROP>(stg.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (!len) continue;
            std::wstring path((size_t)len + 1, L'\0');
            if (DragQueryFileW(drop, i, path.data(), len + 1) == len) {
                path.resize(len);
                files.push_back(path);
            }
        }
        ReleaseStgMedium(&stg);
        return !files.empty();
    }

    LONG ref_ = 1;
    App* app_ = nullptr;
    bool accepted_ = false;
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

    // OLE drag & drop: источник — панель, приёмник — окно и оба списка.
    dropTarget_ = new PanelDropTarget(this);
    RegisterDragDrop(hwnd_, dropTarget_);
    if (left_.List()) RegisterDragDrop(left_.List(), dropTarget_);
    if (right_.List()) RegisterDragDrop(right_.List(), dropTarget_);

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
    AppendMenuW(file, MF_STRING, IDM_DELETE_PERM, L"Удалить без &корзины\tShift+Del");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PROPERTIES, L"С&войства\tAlt+Enter");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_OPEN_NEW_TAB, L"Открыть в новой в&кладке\tCtrl+Enter");
    AppendMenuW(file, MF_STRING, IDM_COPY_PATH, L"Копировать &путь\tCtrl+Shift+C");
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
    AppendMenuW(nav, MF_STRING, IDM_BRANCH, L"&Ветка (плоский список)\tCtrl+B");
    AppendMenuW(nav, MF_STRING, IDM_TAB_LIST, L"Список &вкладок…\tCtrl+Shift+A");
    AppendMenuW(nav, MF_STRING, IDM_CMDHIST, L"История &команд…\tAlt+F8");
    AppendMenuW(nav, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(nav, MF_STRING, IDM_DRIVE_LEFT, L"Диск &левой панели\tAlt+F1");
    AppendMenuW(nav, MF_STRING, IDM_DRIVE_RIGHT, L"Диск &правой панели\tAlt+F2");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)nav, L"&Навигация");

    HMENU net = CreatePopupMenu();
    AppendMenuW(net, MF_STRING, IDM_FTP_CONNECT, L"FTP: &подключиться…\tCtrl+F");
    AppendMenuW(net, MF_STRING, IDM_FTP_DISCONNECT, L"FTP: о&тключиться\tCtrl+Shift+F");
    AppendMenuW(net, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(net, MF_STRING, IDM_NET_BROWSE, L"&Обзор сети…");
    AppendMenuW(net, MF_STRING, IDM_NET_UNC, L"Перейти по &UNC-пути…");
    AppendMenuW(net, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(net, MF_STRING, IDM_NET_MAP, L"&Подключить сетевой диск…");
    AppendMenuW(net, MF_STRING, IDM_NET_UNMAP, L"&Отключить сетевой диск…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)net, L"&Сеть");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_SORT_NAME, L"По &имени\tCtrl+F3");
    AppendMenuW(view, MF_STRING, IDM_SORT_EXT, L"По &типу\tCtrl+F4");
    AppendMenuW(view, MF_STRING, IDM_SORT_SIZE, L"По &размеру\tCtrl+F6");
    AppendMenuW(view, MF_STRING, IDM_SORT_DATE, L"По &дате\tCtrl+F5");
    AppendMenuW(view, MF_STRING, IDM_REVERSE, L"Обратный &порядок");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_HIDDEN, L"Скрытые &файлы\tCtrl+H");
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Обновить\tCtrl+R");
    AppendMenuW(view, MF_STRING, IDM_QUICKVIEW, L"&Быстрый просмотр\tCtrl+Q");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_BOOKMARK_SHOW, L"&Избранное (закладки)\tCtrl+D");
    AppendMenuW(view, MF_STRING, IDM_BOOKMARK_ADD, L"Добавить &закладку");
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
    AppendMenuW(cfg, MF_STRING, IDM_PREFS, L"&Параметры…");
    AppendMenuW(cfg, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(cfg, MF_STRING, IDM_PLUGINS, L"&Плагины Total Commander…");
    AppendMenuW(cfg, MF_STRING, IDM_QUEUE, L"&Очередь операций…");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)cfg, L"&Конфигурация");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_SEARCH, L"&Поиск файлов\tAlt+F7");
    AppendMenuW(tools, MF_STRING, IDM_SYNC, L"&Синхронизация каталогов");
    AppendMenuW(tools, MF_STRING, IDM_DUPES, L"Поиск &дубликатов…");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_UNDO_RENAME, L"Отменить переименование\tCtrl+Z");
    AppendMenuW(tools, MF_STRING, IDM_DIFF, L"Сравнить &файлы…");
    AppendMenuW(tools, MF_STRING, IDM_BATCH_RENAME, L"&Пакетное переименование…\tCtrl+M");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_SWAP, L"С&менить панели\tCtrl+U");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)tools, L"&Инструменты");

    HMENU startMenu = CreatePopupMenu();
    AppendMenuW(startMenu, MF_STRING, IDM_EXPLORER, L"&Проводник здесь");
    AppendMenuW(startMenu, MF_STRING, IDM_CONSOLE, L"&Командная строка здесь");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)startMenu, L"&Запуск");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_SEVENZIP, L"&Проверить 7-Zip…");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&О программе");
    AppendMenuW(menu_, MF_POPUP, (UINT_PTR)help, L"&Помощь");
}

// Пересоздаёт шрифт интерфейса и раздаёт его панелям и статусным полям.
void App::ApplyUiSettings() {
    HFONT fresh = MakeUiFont();
    if (!fresh) return;

    if (font_) DeleteObject(font_);
    font_ = fresh;

    // Мелкий шрифт версии: на 2 пункта меньше интерфейсного, но не мельче 7.
    LOGFONTW vlf{};
    int vpt = GetSettings().uiFontPt - 2;
    if (vpt < 7) vpt = 7;
    vlf.lfHeight = -MulDiv(vpt, (int)dpi_, 72);
    vlf.lfCharSet = DEFAULT_CHARSET;
    vlf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(vlf.lfFaceName, L"Segoe UI");
    HFONT freshVer = CreateFontIndirectW(&vlf);
    if (freshVer) {
        if (verFont_) DeleteObject(verFont_);
        verFont_ = freshVer;
    }

    left_.SetFont(font_, dpi_);
    right_.SetFont(font_, dpi_);
    if (prompt_) SendMessageW(prompt_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (label_) SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (ver_) SendMessageW(ver_, WM_SETFONT, (WPARAM)(verFont_ ? verFont_ : font_), TRUE);

    left_.SetShowHidden(showHidden_);
    right_.SetShowHidden(showHidden_);
    Layout();
    left_.Reload();
    right_.Reload();
}

// Состав тулбара: по умолчанию набор как в TC, иначе — список из [Toolbar] Items.
void App::BuildToolbar() {
    toolbarDefs_.clear();

    auto add = [this](const std::wstring& name) {
        const std::wstring key = Lower(Trim(name));
        if (key.empty()) return;
        for (const auto& c : kToolbarCatalog) {
            if (key == c.name) {
                if ((int)toolbarDefs_.size() < kMaxToolbar) {
                    toolbarDefs_.push_back(TbDef{c.text, c.command, c.separator});
                }
                return;
            }
        }
        LogDebug(L"toolbar: неизвестная команда '" + key + L"'");
    };

    const auto& custom = GetSettings().toolbarItems;
    if (custom.empty()) {
        for (const wchar_t* name : kToolbarDefault) add(name);
    } else {
        for (const auto& name : custom) add(name);
    }
    if (toolbarDefs_.empty()) {                 // ничего не распознали — откат к набору по умолчанию
        for (const wchar_t* name : kToolbarDefault) add(name);
    }
    toolbarCount_ = (int)toolbarDefs_.size();
    LogDebug(L"toolbar: кнопок " + std::to_wstring(toolbarCount_));
}

HFONT App::MakeUiFont() {
    LOGFONTW lf{};
    int pt = GetSettings().uiFontPt;
    if (pt < 8) pt = 8;
    if (pt > 14) pt = 14;
    lf.lfHeight = -MulDiv(pt, (int)dpi_, 72);
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

    // Версия на главном окне: правый край ряда тулбара, мелким серым шрифтом.
    ver_ = CreateWindowExW(0, L"STATIC", POSFILES_VERSION_FULL, WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                           0, 0, 10, 10, hwnd_, nullptr, g_inst, nullptr);

    prompt_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                              0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)ID_PROMPT, g_inst, nullptr);

    BuildToolbar();
    for (int i = 0; i < toolbarCount_; i++) {
        toolbar_[i] = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                      hwnd_, (HMENU)(INT_PTR)(ID_TB_FIRST + i), g_inst, nullptr);
        // Кнопкам (не сепараторам) — subclass для hover-подсветки, как в TC.
        if (!toolbarDefs_[i].separator) SetWindowSubclass(toolbar_[i], TbBtnProc, 5, (DWORD_PTR)this);
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
        for (int i = 0; i < toolbarCount_; i++) {
            if (toolbarDefs_[i].separator) continue;   // сепараторам подсказки не нужны
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd_;
            ti.uId = (UINT_PTR)toolbar_[i];
            ti.lpszText = (LPWSTR)toolbarDefs_[i].text;
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

    // Сохранённый FTP-каталог мог стать недоступен (нет пароля/сервера) —
    // панель не должна остаться пустой.
    if (left_.Path().empty()) left_.Navigate(cwd);
    if (right_.Path().empty()) right_.Navigate(cwd);

    // Возвращаем курсор и активную панель.
    left_.SetCursorIndex(st.leftCursor);
    right_.SetCursorIndex(st.rightCursor);
    if (st.activeSide == 1) SetActivePanel(1);
    ActivePanel()->FocusList();
    UpdatePromptLabel();

    // Временная диагностика этапа 4 (рамка курсора неактивной панели) — в %TEMP%\posfiles-debug.log.
    wchar_t stamp[32]{};
    swprintf(stamp, 32, L"%hs %hs", __DATE__, __TIME__);
    LogDebug(L"7z: '" + Find7zPath() + L"'");
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

    HDWP dwp = BeginDeferWindowPos(12 + toolbarCount_);
    int tbEnd = 0;                                    // правый край кнопок тулбара
    if (dwp) {
        int x = 0;
        const int bh = Scale(24, dpi_);              // кнопка 24×24, как в TC
        const int by = (tbH - bh) / 2;
        for (int i = 0; i < toolbarCount_; i++) {
            const int w = toolbarDefs_[i].separator ? Scale(7, dpi_) : bh;
            dwp = DeferWindowPos(dwp, toolbar_[i], nullptr, x, by, w, bh, SWP_NOZORDER);
            x += w + Scale(2, dpi_);
        }
        tbEnd = x;
    }

    // Версия в правом краю ряда тулбара: место есть — показываем, нет — прячем.
    if (ver_) {
        const int verW = Scale(104, dpi_);
        const int verX = width - verW - gap;
        const bool room = verX > tbEnd + Scale(12, dpi_);
        ShowWindow(ver_, room ? SW_SHOW : SW_HIDE);
        if (room && dwp) dwp = DeferWindowPos(dwp, ver_, nullptr, verX, 0, verW, tbH, SWP_NOZORDER);
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
    const wchar_t sep = IsFtpUrl(text) ? L'/' : L'\\';
    if (!text.empty() && text.back() != sep && text.back() != L'\\' && text.back() != L'/') text.push_back(sep);
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

// Ctrl+Enter / Ctrl+Shift+Enter: имя или полный путь — в командную строку (как в TC).
void App::AppendToPrompt(const std::wstring& text, bool quoted) {
    if (!prompt_ || text.empty()) return;
    wchar_t buf[4096]{};
    GetWindowTextW(prompt_, buf, 4096);
    std::wstring line = buf;
    std::wstring add = text;
    if (quoted && add.find(L' ') != std::wstring::npos) add = L"\"" + add + L"\"";
    if (!line.empty() && line.back() != L' ') line += L' ';
    line += add;
    LogDebug(L"prompt: +'" + add + L"'");
    SetWindowTextW(prompt_, line.c_str());
    SendMessageW(prompt_, CB_SETEDITSEL, 0, MAKELPARAM(line.size(), line.size()));
    SetFocus(prompt_);
}

// Ctrl+Enter: элемент открывается в новой вкладке, текущая остаётся на месте.
void App::OpenCursorInNewTab() {
    LogDebug(L"new tab from cursor");
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e || e->isUp) return;
    if (panel->InFtp() || panel->InArchive()) {      // для FTP/архива новая вкладка не нужна
        OpenCursor();
        return;
    }
    if (e->isDir) {
        panel->AddTabFor(JoinPath(panel->Path(), e->name), L"");
    } else {
        const std::wstring name = e->name;
        panel->AddTabFor(panel->Path(), name);
        panel->Navigate(panel->Path(), name);
    }
    panel->FocusList();
}

// Ctrl+Shift+C: пути отмеченных файлов (или текущего) в буфер обмена.
void App::CopyCursorPaths() {
    Panel* panel = ActivePanel();
    if (!panel) return;
    const std::vector<std::wstring> names = panel->SelectedNames();
    if (names.empty()) return;

    std::wstring text;
    for (const auto& name : names) {
        if (!text.empty()) text += L"\r\n";
        text += JoinPath(panel->Path(), name);
    }
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
    LogDebug(L"copy path: " + std::to_wstring(names.size()) + L" шт.");
}

void App::OpenCursor() {
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e) return;

    // FTP: каталог — переход, файл — скачиваем во временный и открываем.
    if (panel->InFtp()) {
        if (e->isUp) {
            panel->GoUp();
            return;
        }
        if (e->isDir) {
            const std::wstring target = FtpJoinUrl(panel->Path(), e->name);
            if (!panel->Navigate(target)) ShowError(panel->NavigateError());
            return;
        }
        const std::wstring temp = DownloadFtpToTemp(panel->FocusedPath(), e->name);
        if (temp.empty()) return;
        HINSTANCE hr = ShellExecuteW(hwnd_, L"open", temp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) ShowError(L"Не удалось открыть:\n" + temp);
        return;
    }

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

    // Любая сторона на FTP — отдельный путь: оболочка с URL не работает.
    if (panel->InFtp() || other->InFtp()) {
        DoFtpCopyMove(move);
        return;
    }

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
        // Добавление в архив: источники — выделенные записи этой панели на диске.
        std::vector<std::wstring> addSources = panel->SelectedPaths();
        if (addSources.empty()) return;
        LogDebug(L"archive add: '" + other->ArchiveFile() + L"' inner='" + other->ArchiveInner() +
                 L"' sources=" + std::to_wstring(addSources.size()) + L" move=" + std::to_wstring(move ? 1 : 0));

        std::wstring addError;
        if (!ArchiveAdd(hwnd_, other->ArchiveFile(), other->ArchiveInner(), addSources, addError)) {
            ShowError(addError);
            return;
        }
        if (move) DeleteViaShell(hwnd_, addSources);   // перемещение: исходники убираем
        panel->ClearMarks();
        other->Reload();
        if (move) panel->Reload();
        UpdatePromptLabel();
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

void App::DoFtpCopyMove(bool move) {
    Panel* panel = ActivePanel();
    Panel* other = OtherPanel();

    if (panel->InArchive() || other->InArchive()) {
        ShowError(L"Копирование между архивом и FTP пока не поддерживается.");
        return;
    }

    std::vector<std::wstring> names = panel->SelectedNames();
    if (names.empty()) return;

    CopyMoveOptions opts;
    opts.target = other->Path();
    if (!CopyMoveBox(hwnd_, move ? L"Перемещение по FTP" : L"Копирование по FTP",
                     L"Копировать в каталог:", opts)) {
        return;
    }
    const std::wstring target = Trim(opts.target);
    if (target.empty()) return;
    if (opts.background) {
        MessageBoxW(hwnd_, L"Фоновая очередь для FTP пока не поддерживается:\nоперация выполняется сразу.",
                    L"FTP", MB_ICONINFORMATION | MB_OK);
    }

    const bool targetRemote = IsFtpUrl(target);
    FtpSite srcSite, dstSite;
    std::wstring srcRemote, dstRemote, error;
    std::unique_ptr<FtpConnection> srcConn, dstConn;

    if (panel->InFtp()) {
        if (!ConnectFtpForUrl(panel->Path(), srcConn, srcSite, srcRemote, error)) {
            ShowError(error);
            return;
        }
    }
    if (targetRemote) {
        if (!ConnectFtpForUrl(target, dstConn, dstSite, dstRemote, error)) {
            ShowError(error);
            return;
        }
        if (!EnsureRemoteDir(*dstConn, target, error)) {
            ShowError(error);
            return;
        }
    } else if (!IsDirectory(target)) {
        if (!EnsureLocalDir(target, error)) {
            ShowError(error);
            return;
        }
    }

    // Один вопрос про перезапись вместо вопроса на каждый файл.
    bool overwriteAll = opts.overwriteAll;
    bool skipExisting = false;
    bool targetHasEntries = false;
    if (targetRemote) {
        std::vector<FtpEntryInfo> probe;
        std::wstring probeError;
        if (FtpList(*dstConn, dstRemote, probe, probeError)) targetHasEntries = !probe.empty();
    } else {
        std::vector<Entry> probe;
        DWORD probeError = 0;
        if (ListDir(target, true, probe, probeError)) targetHasEntries = !probe.empty();
    }
    if (targetHasEntries && !overwriteAll) {
        const int answer = MessageBoxW(
            hwnd_,
            L"В каталоге назначения уже есть файлы.\n\n"
            L"«Да» — перезаписать все\n"
            L"«Нет» — пропустить существующие\n"
            L"«Отмена» — прервать",
            move ? L"FTP: перемещение" : L"FTP: копирование", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return;
        if (answer == IDYES) overwriteAll = true;
        if (answer == IDNO) skipExisting = true;
    }

    FtpCopyCtx ctx;
    ctx.owner = hwnd_;
    ctx.src = srcConn.get();
    ctx.dst = dstConn.get();
    ctx.move = move;
    ctx.overwriteAll = overwriteAll;
    ctx.skipExisting = skipExisting;
    if (!Trim(opts.mask).empty()) ctx.masks = SplitMasks(opts.mask);

    FtpProgressOpen(hwnd_, move ? L"FTP: перемещение" : L"FTP: копирование", target);
    for (auto& name : names) {
        const Entry* entry = nullptr;
        for (auto& e : panel->Entries()) {
            if (e.name == name) {
                entry = &e;
                break;
            }
        }
        if (!entry || entry->isUp) continue;

        const std::wstring srcPath = panel->InFtp() ? FtpJoinUrl(panel->Path(), name)
                                                    : JoinPath(panel->Path(), name);
        const std::wstring dstPath = targetRemote ? FtpJoinUrl(target, name) : JoinPath(target, name);
        if (!CopyFtpItem(ctx, srcPath, dstPath, name, entry->isDir, true)) break;
    }
    FtpProgressClose();

    if (!ctx.error.empty() && ctx.error != L"Отменено") ShowError(ctx.error);
    panel->ClearMarks();
    panel->Reload();
    other->Reload();
    UpdatePromptLabel();
    LogDebug(L"FTP copy/move files=" + std::to_wstring(ctx.files) + L" move=" + std::to_wstring(move ? 1 : 0) +
             L" error='" + ctx.error + L"'");
}

void App::DoRename() {
    Panel* panel = ActivePanel();
    Entry* e = panel->Cursor();
    if (!e || e->isUp) return;

    // Локально — правка имени прямо в строке, как в TC. Для FTP оставляем диалог.
    if (!panel->InFtp() && !panel->InArchive()) {
        // Начинаем после возврата из Dispatch: иначе возврат фокуса в список
        // сразу закроет только что созданный EDIT.
        PostMessageW(hwnd_, WM_APP + 305, (WPARAM)panel->Side(), 0);
        return;
    }
    if (panel->InArchive()) {
        MessageBoxW(hwnd_, L"Переименование внутри архива не поддерживается.", L"Переименование",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::wstring name = e->name;
    if (!InputBox(hwnd_, L"Переименование", L"Новое имя:", name, false)) return;
    name = Trim(name);
    if (name.empty() || SamePath(name, e->name)) return;

    if (panel->InFtp()) {
        FtpSite site;
        std::wstring remoteDir;
        std::unique_ptr<FtpConnection> conn;
        std::wstring error;
        if (!ConnectFtpForUrl(panel->Path(), conn, site, remoteDir, error)) {
            ShowError(error);
            return;
        }
        const std::wstring base = (remoteDir == L"/") ? L"" : remoteDir;
        if (!FtpRename(*conn, base + L"/" + e->name, base + L"/" + name, error)) {
            ShowError(L"Не удалось переименовать:\n" + error);
            return;
        }
        panel->Reload();
        return;
    }

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
    UndoPushRename(from, to);
    panel->Reload();
    panel->SetCursorIndex(panel->CursorIndex());
    UpdatePromptLabel();
}

// Shift+F5: копия рядом с исходным файлом под новым именем (как в TC).
void App::DoCopyInPlace() {
    Panel* panel = ActivePanel();
    if (panel->InFtp() || panel->InArchive()) {
        ShowError(L"Копия в этом же каталоге доступна только для файлов на диске.");
        return;
    }
    Entry* e = panel->Cursor();
    if (!e || e->isUp) return;
    if (e->isDir) {
        ShowError(L"Каталог в этом же месте: используйте F5 и укажите новое имя в пути назначения.");
        return;
    }

    const std::wstring name = e->name;
    std::wstring base = name, ext;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot);
    }
    std::wstring newName = base + L" — копия" + ext;
    if (!InputBox(hwnd_, L"Копия в этом каталоге", L"Новое имя:", newName, false)) return;
    newName = Trim(newName);
    if (newName.empty() || SamePath(newName, name)) return;

    const std::wstring src = JoinPath(panel->Path(), name);
    const std::wstring dst = JoinPath(panel->Path(), newName);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        ShowError(L"Не удалось скопировать:\n" + ErrText(GetLastError()));
        return;
    }
    LogDebug(L"copy in place: '" + name + L"' -> '" + newName + L"'");
    panel->Reload();
    UpdatePromptLabel();
}
void App::DoMkdir(Panel* target, const std::wstring& initialName) {
    Panel* panel = target ? target : ActivePanel();

    if (panel->InFtp()) {
        std::wstring name;
        if (!InputBox(hwnd_, L"Создать каталог", L"Имя каталога:", name, false)) return;
        name = Trim(name);
        if (name.empty()) return;

        FtpSite site;
        std::wstring remoteDir;
        std::unique_ptr<FtpConnection> conn;
        std::wstring error;
        if (!ConnectFtpForUrl(panel->Path(), conn, site, remoteDir, error)) {
            ShowError(error);
            return;
        }
        const std::wstring base = (remoteDir == L"/") ? L"" : remoteDir;
        if (!FtpCreateDir(*conn, base + L"/" + name, error)) {
            ShowError(L"Не удалось создать каталог:\n" + error);
            return;
        }
        panel->Reload();
        return;
    }

    std::wstring name = initialName;
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

void App::DoDelete(bool permanent) {
    LogDebug(L"DoDelete enter");
    Panel* panel = ActivePanel();

    if (panel->InFtp()) {
        std::vector<std::wstring> names = panel->SelectedNames();
        if (names.empty()) return;
        std::wstring question;
        if (names.size() == 1) {
            question = L"Удалить «" + names[0] + L"» на FTP без корзины?";
        } else {
            question = L"Удалить на FTP без корзины объектов: " + std::to_wstring(names.size()) + L"?";
        }
        if (MessageBoxW(hwnd_, question.c_str(), L"Удаление по FTP", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

        FtpSite site;
        std::wstring remoteDir;
        std::unique_ptr<FtpConnection> conn;
        std::wstring error;
        if (!ConnectFtpForUrl(panel->Path(), conn, site, remoteDir, error)) {
            ShowError(error);
            return;
        }

        FtpProgressOpen(hwnd_, L"FTP: удаление", panel->Path());
        bool cancelled = false;
        for (auto& name : names) {
            DeleteFtpTree(*conn, FtpJoinUrl(panel->Path(), name), name, error, cancelled);
            if (cancelled || !error.empty()) break;
        }
        FtpProgressClose();

        if (!error.empty() && error != L"Отменено") ShowError(error);
        panel->ClearMarks();
        panel->Reload();
        return;
    }

    if (panel->InArchive()) {
        std::vector<std::wstring> names = panel->SelectedNames();
        if (names.empty()) return;
        const std::wstring prefix = panel->ArchiveInner().empty() ? L"" : panel->ArchiveInner() + L"\\";
        std::vector<std::wstring> inner;
        for (auto& name : names) inner.push_back(prefix + name);

        std::wstring question;
        if (names.size() == 1) {
            question = L"Удалить «" + names[0] + L"» из архива?\nВосстановить будет нельзя.";
        } else {
            question = L"Удалить из архива объектов: " + std::to_wstring(names.size()) +
                       L"?\nВосстановить будет нельзя.";
        }
        if (MessageBoxW(hwnd_, question.c_str(), L"Удаление из архива", MB_ICONQUESTION | MB_YESNO) != IDYES)
            return;

        std::wstring delError;
        if (!ArchiveDelete(hwnd_, panel->ArchiveFile(), inner, delError)) ShowError(delError);
        panel->ClearMarks();
        panel->Reload();
        return;
    }

    std::vector<std::wstring> targets = panel->SelectedPaths();
    if (targets.empty()) return;

    std::wstring question;
    if (targets.size() == 1) {
        std::wstring name = targets[0].substr(targets[0].find_last_of(L'\\') + 1);
        question = permanent ? (L"Удалить «" + name + L"» безвозвратно?")
                            : (L"Удалить «" + name + L"» в корзину?");
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, permanent ? L"Удалить объектов: %zu безвозвратно?" : L"Удалить объектов: %zu в корзину?",
                 targets.size());
        question = buf;
    }
    if (MessageBoxW(hwnd_, question.c_str(), L"Удаление", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    if (!DeleteViaShell(hwnd_, targets, false, permanent)) ShowError(L"Удаление не выполнено или отменено.");

    panel->ClearMarks();
    panel->Reload();
}

void App::DoProperties() {
    Panel* panel = ActivePanel();
    if (panel->InFtp()) {
        Entry* e = panel->Cursor();
        if (!e || e->isUp) {
            MessageBoxW(hwnd_, panel->Path().c_str(), L"FTP", MB_ICONINFORMATION | MB_OK);
            return;
        }
        std::wstring text = panel->FullPathOf(*e) + L"\n\n";
        text += e->isDir ? L"каталог" : (L"размер: " + FmtSize(e->size) + L"\nизменён: " + FmtDate(e->ft));
        MessageBoxW(hwnd_, text.c_str(), L"FTP: свойства", MB_ICONINFORMATION | MB_OK);
        return;
    }
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
    const bool isTool = id >= ID_TB_FIRST && id < ID_TB_FIRST + toolbarCount_;
    const int idx = isTool ? id - ID_TB_FIRST : -1;

    if (isTool && toolbarDefs_[idx].separator) {
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

        if (HICON icon = ToolIcon(toolbarDefs_[idx].command)) {
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
    if (ActivePanel()->InFtp() || OtherPanel()->InFtp()) {
        MessageBoxW(hwnd_, L"Быстрый просмотр по FTP не поддерживается.", L"FTP", MB_ICONINFORMATION | MB_OK);
        return;
    }
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

DWORD App::OleDragEffect(POINTL pt, DWORD keyState) const {
    POINT p{pt.x, pt.y};
    const int side = PanelAtScreen(p);
    if (side < 0) return DROPEFFECT_NONE;
    const Panel* target = (side == 0) ? &left_ : &right_;
    if (target->InFtp() || target->InArchive()) return DROPEFFECT_NONE;
    return (keyState & MK_SHIFT) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
}

void App::OleDropFiles(POINTL pt, const std::vector<std::wstring>& files, bool move) {
    POINT p{pt.x, pt.y};
    const int side = PanelAtScreen(p);
    if (side < 0 || files.empty()) return;
    Panel* target = (side == 0) ? &left_ : &right_;
    if (target->InFtp() || target->InArchive()) {
        MessageBoxW(hwnd_, L"Перетаскивание в FTP или архив не поддерживается. Используйте F5/F6.",
                    L"Перетаскивание", MB_ICONINFORMATION | MB_OK);
        return;
    }
    LogDebug(L"OLE drop files=" + std::to_wstring(files.size()) + L" move=" + std::to_wstring(move ? 1 : 0) +
             L" target='" + target->Path() + L"'");
    if (!ShellCopyMove(hwnd_, files, target->Path(), move)) {
        ShowError(move ? L"Перемещение не выполнено или отменено." : L"Копирование не выполнено или отменено.");
    }
    left_.ClearMarks();
    right_.ClearMarks();
    left_.Reload();
    right_.Reload();
    UpdatePromptLabel();
}

// Перетаскивание между панелями: по умолчанию копирование, с Shift — перемещение.
void App::DropOnPanel(int fromSide, int toSide) {
    if (fromSide == toSide) return;

    Panel* src = (fromSide == 0) ? &left_ : &right_;
    Panel* dst = (fromSide == 0) ? &right_ : &left_;

    if (src->InFtp() || dst->InFtp()) {
        MessageBoxW(hwnd_, L"Перетаскивание с FTP пока не поддерживается. Используйте F5/F6.", L"FTP",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

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
// Распаковка архивов в каталог другой панели (или указанный): с прогрессом.
void App::DoUnpack() {
    Panel* panel = ActivePanel();
    Panel* other = OtherPanel();

    if (panel->InFtp() || other->InFtp()) {
        ShowError(L"Распаковка с FTP или на FTP пока не поддерживается.");
        return;
    }

    if (panel->InArchive()) {
        ShowError(L"Открыт архив. Выйдите в каталог: распаковка из просмотра — по F5.");
        return;
    }

    // Сначала отмеченные архивы, иначе — тот, на котором стоит курсор.
    std::vector<std::wstring> archives;
    for (auto& e : panel->Entries()) {
        if (e.marked && !e.isDir && !e.isUp && IsArchivePath(e.name)) {
            archives.push_back(JoinPath(panel->Path(), e.name));
        }
    }
    if (archives.empty()) {
        Entry* cur = panel->Cursor();
        if (cur && !cur->isDir && !cur->isUp && IsArchivePath(cur->name)) {
            archives.push_back(JoinPath(panel->Path(), cur->name));
        }
    }
    if (archives.empty()) {
        ShowError(L"Курсор не на архиве.\n\nВстаньте на .zip/.7z/.rar/.tar или отметьте архивы.");
        return;
    }

    std::wstring target = other->Path();
    std::wstring password;
    std::wstring archiveName = archives[0].substr(archives[0].find_last_of(L'\\') + 1);
    if (archives.size() > 1) {
        wchar_t many[128]{};
        swprintf(many, 128, L"архивов: %zu", archives.size());
        archiveName = many;
    }
    if (!UnpackDialog(hwnd_, archiveName, target, password)) return;
    target = Trim(target);
    if (target.empty()) return;

    int done = 0;
    for (auto& archive : archives) {
        std::wstring error;
        if (!ExtractArchiveWithProgress(hwnd_, archive, target, password, error)) {
            ShowError(error);
            break;
        }
        done++;
    }

    if (done > 0) {
        panel->Reload();
        other->Reload();
        UpdatePromptLabel();
        LogDebug(L"unpack: архивов " + std::to_wstring(done) + L" -> " + target);
    }
}

void App::PackSelection() {
    Panel* panel = ActivePanel();
    if (!panel) return;

    if (panel->InFtp()) {
        MessageBoxW(hwnd_, L"Упаковка файлов на FTP не поддерживается: сначала скачайте их (F5).", L"FTP",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

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

    if (panel->InFtp()) {
        MessageBoxW(hwnd_, L"Пакетное переименование по FTP не поддерживается: переименуйте по одному (F2).",
                    L"FTP", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (panel->InArchive()) {
        MessageBoxW(hwnd_, L"Переименование внутри архива не поддерживается.", kTitle, MB_ICONINFORMATION);
        return;
    }

    std::vector<std::wstring> names = panel->SelectedNames();
    if (names.empty()) return;

    const int renamed = BatchRenameDialog(hwnd_, panel->Path(), names);
    if (renamed > 0) {
        panel->ClearMarks();
        panel->Reload();
        UpdatePromptLabel();
    }
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

    // Как в TC: Ctrl+D — меню избранного; добавление текущего — пунктом внутри.
    HMENU popup = CreatePopupMenu();
    for (size_t i = 0; i < st.bookmarks.size() && i < 200; ++i) {
        AppendMenuW(popup, MF_STRING, 2000 + (UINT)i, st.bookmarks[i].c_str());
    }
    if (!st.bookmarks.empty()) AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, 2001, L"Добавить текущий каталог");

    POINT pt{};
    GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(popup);

    if (cmd == 2001) {
        AddBookmark();
        return;
    }
    if (cmd >= 2000) {
        size_t index = (size_t)(cmd - 2000);
        if (index < st.bookmarks.size()) {
            if (!ActivePanel()->Navigate(st.bookmarks[index])) ShowError(ActivePanel()->NavigateError());
        }
    }
}

// Alt+F8: выпадающий список истории команд (как в TC).
void App::ShowCommandHistory() {
    if (!prompt_ || history_.empty()) return;
    SendMessageW(prompt_, CB_RESETCONTENT, 0, 0);
    for (const auto& line : history_) {
        SendMessageW(prompt_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    SetFocus(prompt_);
    SendMessageW(prompt_, CB_SHOWDROPDOWN, TRUE, 0);
}

// Ctrl+Shift+A: список вкладок активной панели.
void App::ShowTabList() {
    Panel* panel = ActivePanel();
    const int count = panel ? panel->TabCount() : 0;
    if (count <= 0) return;

    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < count && i < 64; ++i) {
        std::wstring title = panel->TabTitle(i);
        if (title.empty()) title = L"Вкладка " + std::to_wstring(i + 1);
        const UINT flags = MF_STRING | (i == panel->ActiveTab() ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, 3000 + i, title.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3099, L"Закрыть текущую вкладку\tCtrl+W");

    POINT pt{};
    GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == 3099) {
        panel->CloseTab();
        panel->FocusList();
        return;
    }
    if (cmd >= 3000 && cmd < 3000 + count) {
        panel->ActivateTab(cmd - 3000);
        panel->FocusList();
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
        case IDM_FTP_CONNECT:
            FtpConnectDialog();
            break;
        case IDM_FTP_DISCONNECT:
            FtpDisconnectActive();
            break;
        case IDM_DUPES: {
            if (ActivePanel()->InFtp() || ActivePanel()->InArchive()) {
                MessageBoxW(hwnd_, L"Поиск дубликатов работает только с локальными каталогами.", L"Дубликаты",
                            MB_ICONINFORMATION | MB_OK);
                break;
            }
            FindDuplicates(hwnd_, ActivePanel()->Path());
            break;
        }
        case IDM_PREFS: {
            if (ShowPreferences(hwnd_)) {
                ApplyUiSettings();
            }
            break;
        }
        case IDM_DIFF: {
            if (left_.InFtp() || right_.InFtp() || left_.InArchive() || right_.InArchive()) {
                MessageBoxW(hwnd_, L"Сравнение работает только с локальными файлами.", L"Сравнение файлов",
                            MB_ICONINFORMATION | MB_OK);
                break;
            }
            Entry* le = left_.Cursor();
            Entry* re = right_.Cursor();
            if (!le || !re || le->isUp || re->isUp || le->isDir || re->isDir) {
                MessageBoxW(hwnd_, L"Поставьте курсор на файл в каждой панели.", L"Сравнение файлов",
                            MB_ICONINFORMATION | MB_OK);
                break;
            }
            DiffFiles(hwnd_, JoinPath(left_.Path(), le->name), JoinPath(right_.Path(), re->name));
            break;
        }
        case IDM_UNDO_RENAME: {
            std::wstring report;
            const bool any = UndoLastRename(report);
            left_.Reload();
            right_.Reload();
            MessageBoxW(hwnd_, report.c_str(), L"Отмена переименования",
                        (any ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
            break;
        }
        case IDM_OPEN_NEW_TAB:
            OpenCursorInNewTab();
            break;
        case IDM_COPY_PATH:
            CopyCursorPaths();
            break;
        case IDM_SEARCH: {
            if (ActivePanel()->InFtp()) {
                MessageBoxW(hwnd_, L"Поиск по FTP не поддерживается.", L"FTP", MB_ICONINFORMATION | MB_OK);
                break;
            }
            std::wstring found = SearchDialog(hwnd_, ActivePanel()->Path());
            LogDebug(L"search chosen='" + found + L"'");
            if (!found.empty()) {
                const size_t slash = found.find_last_of(L'\\');
                if (slash != std::wstring::npos) {
                    const std::wstring dir = found.substr(0, slash);
                    const std::wstring name = found.substr(slash + 1);

                    // Путь может быть внутри архива: ищем существующий архивный
                    // файл-префикс и открываем архив с нужным каталогом внутри.
                    if (!PathExists(dir)) {
                        std::wstring root;
                        for (size_t pos = found.find(L'\\'); pos != std::wstring::npos;
                             pos = found.find(L'\\', pos + 1)) {
                            const std::wstring candidate = found.substr(0, pos);
                            if (IsArchivePath(candidate) && PathExists(candidate)) {
                                root = candidate;
                                break;
                            }
                        }
                        if (!root.empty()) {
                            std::wstring inner = found.substr(root.size() + 1);
                            for (wchar_t& ch : inner)
                                if (ch == L'/') ch = L'\\';
                            const size_t lastSlash = inner.find_last_of(L'\\');
                            const std::wstring innerDir =
                                lastSlash == std::wstring::npos ? std::wstring{} : inner.substr(0, lastSlash);
                            const std::wstring fileName =
                                lastSlash == std::wstring::npos ? inner : inner.substr(lastSlash + 1);
                            const bool first = ActivePanel()->Navigate(root);
                            const bool second =
                                first && ActivePanel()->Navigate(
                                              innerDir.empty() ? root : root + L"\\" + innerDir, fileName);
                            LogDebug(L"search: архив root='" + root + L"' inner='" + innerDir +
                                     L"' focus='" + fileName + L"' enter=" + std::to_wstring(first ? 1 : 0) +
                                     L"/" + std::to_wstring(second ? 1 : 0) + L" err='" +
                                     ActivePanel()->NavigateError() + L"'");
                            if (second) ActivePanel()->FocusList();
                            break;
                        }
                    }

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
            if (panel->InFtp()) {
                MessageBoxW(hwnd_, L"Создание файлов по FTP пока не поддерживается.\n"
                                    L"Создайте его локально и загрузите через F5.",
                            L"FTP", MB_ICONINFORMATION | MB_OK);
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
            LogDebug(L"view: cursor='" + (e ? e->name : L"<null>") + L"' isDir=" +
                     std::to_wstring(e && e->isDir ? 1 : 0) + L" path='" + panel->FocusedPath() + L"'");
            if (!e || e->isUp || e->isDir) {
                MessageBoxW(hwnd_, L"Просмотр доступен только для файлов.", kTitle, MB_ICONINFORMATION);
                return;
            }
            if (panel->InFtp()) {
                const std::wstring temp = DownloadFtpToTemp(panel->FocusedPath(), e->name);
                if (!temp.empty()) ShowLister(hwnd_, temp);
                break;
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
            if (panel->InFtp()) {
                MessageBoxW(hwnd_, L"Правка файлов по FTP пока не поддерживается.\n"
                                    L"Откройте файл через F3 (просмотр).",
                            L"FTP", MB_ICONINFORMATION | MB_OK);
                break;
            }
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
            DoDelete(false);
            break;
        case IDM_DELETE_PERM:
            DoDelete(true);
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
            DoUnpack();
            break;
        case IDM_SYNC:
            if (left_.InFtp() || right_.InFtp()) {
                MessageBoxW(hwnd_, L"Синхронизация каталогов по FTP не поддерживается.", L"FTP",
                            MB_ICONINFORMATION | MB_OK);
                break;
            }
            SyncDirectories(hwnd_, left_.Path(), right_.Path());
            break;
        case IDM_BRANCH:
            panel->ToggleBranch();
            break;
        case IDM_TAB_LIST:
            ShowTabList();
            break;
        case IDM_CMDHIST:
            ShowCommandHistory();
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
        case IDM_SEVENZIP: {
            ResetSevenZipCache();          // 7-Zip могли установить только что
            const std::wstring sevenZip = Find7zPath();
            if (sevenZip.empty()) {
                const int answer = MessageBoxW(
                    hwnd_,
                    L"7z.exe не найден.\n\n"
                    L"Архивы (просмотр, упаковка, распаковка, поиск и правка внутри) работают через 7-Zip.\n"
                    L"Установите 7-Zip или положите 7z.exe рядом с PosFiles.exe.\n\n"
                    L"Открыть сайт 7-Zip?",
                    L"7-Zip не найден", MB_ICONWARNING | MB_YESNO);
                if (answer == IDYES)
                    ShellExecuteW(hwnd_, L"open", L"https://www.7-zip.org/", nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                MessageBoxW(hwnd_, (L"7-Zip найден:\n" + sevenZip).c_str(), L"7-Zip",
                            MB_ICONINFORMATION | MB_OK);
            }
            break;
        }
        case IDM_ABOUT:
            ShowAbout(hwnd_);
            break;
        default:
            break;
    }
    // Возвращаем фокус в список — как в Total Commander после команды.
    // Но не выдёргиваем его из inline-переименования (F2).
    if (!ActivePanel()->IsInlineRenaming() && GetFocus() != prompt_) ActivePanel()->FocusList();
    UpdatePromptLabel();
}

// ---------- клавиатура ----------

bool App::PreTranslate(const MSG& msg) {
    // Держим Shift по сообщениям окна: GetKeyState в редких случаях не видит
    // модификатор к моменту прихода стрелки (например, при синтетическом вводе).
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
        if (msg.wParam == VK_SHIFT || msg.wParam == VK_LSHIFT || msg.wParam == VK_RSHIFT) shiftHeld_ = true;
    } else if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
        if (msg.wParam == VK_SHIFT || msg.wParam == VK_LSHIFT || msg.wParam == VK_RSHIFT) shiftHeld_ = false;
    }

    // Правка имени в строке панели: весь ввод принадлежит EDIT. Иначе Enter
    // ниже уходит в OpenCursor (файл открывается до применения имени), а WM_CHAR
    // съедает быстрый поиск по буквам. Alt оставляем системе (Alt+F4).
    if (left_.IsInlineRenaming() || right_.IsInlineRenaming()) {
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (!alt && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN ||
                     msg.message == WM_CHAR || msg.message == WM_SYSCHAR))
            return false;
    }

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
    bool shift = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) || shiftHeld_;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    Panel* panel = ActivePanel();
    // Shift+стрелки — отметка диапазона от якоря (как в TC), а не просто курсор.
    if (inList && shift && !ctrl && !alt) {
        const int count = (int)panel->Entries().size();
        if (count > 0) {
            const int cur = panel->CursorIndex();
            int page = ListView_GetCountPerPage(panel->List());
            if (page < 1) page = 1;
            int next = cur;
            switch (msg.wParam) {
                case VK_UP: next = cur - 1; break;
                case VK_DOWN: next = cur + 1; break;
                case VK_HOME: next = 0; break;
                case VK_END: next = count - 1; break;
                case VK_PRIOR: next = cur - page; break;
                case VK_NEXT: next = cur + page; break;
                default: break;
            }
            if (next != cur) {
                if (next < 0) next = 0;
                if (next >= count) next = count - 1;
                panel->ShiftMarkTo(next);
                panel->FocusList();
                return true;
            }
        }
    }

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
    if (alt && msg.wParam == VK_F8) {           // Alt+F8 — выпадающая история команд
        ShowCommandHistory();
        return true;
    }

    // Прочие Alt+... отдаём системе (Alt+F4 закроет окно, Alt+Tab переключит задачу).
    if (alt) return false;

    // Ctrl+Enter / Ctrl+Shift+Enter — имя файла (полный путь) в командную строку, как в TC.
    if (!inPrompt && ctrl && msg.wParam == VK_RETURN) {
        Entry* cur = panel->Cursor();
        if (cur && !cur->isUp) AppendToPrompt(shift ? panel->FocusedPath() : cur->name, true);
        return true;
    }
    if (!inPrompt && ctrl && shift && msg.wParam == 'C') {
        CopyCursorPaths();
        return true;
    }

    // Ctrl-комбинации обрабатываем до обычных клавиш: Ctrl+F8 не должен стать F8-удалением.
    if (ctrl) {
        switch (msg.wParam) {
            case 'A':
                if (shift) {            // Ctrl+Shift+A — список вкладок
                    ShowTabList();
                } else {
                    panel->MarkAll(true);
                }
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
                ShowBookmarksMenu();        // как в TC: меню избранного, добавление — внутри
                return true;
            case 'J': {                 // Ctrl+J / Ctrl+Shift+J — имя/путь в командную строку
                Entry* cur = panel->Cursor();
                if (cur && !cur->isUp) AppendToPrompt(shift ? panel->FocusedPath() : cur->name, true);
                return true;
            }
            case 'L':                   // Ctrl+L — пересчитать размеры отмеченного
                panel->RecalcMarkedSizes();
                return true;
            case 'S':                   // Ctrl+S — быстрый фильтр по подстроке
                if (shift) panel->ClearQuickFilter();
                else panel->BeginQuickFilter();
                return true;
            case 'E': {                 // Ctrl+E — предыдущая команда в строку
                if (history_.empty()) return true;
                if (histPos_ < 0) histPos_ = (int)history_.size() - 1;
                else if (histPos_ > 0) --histPos_;
                SetWindowTextW(prompt_, history_[histPos_].c_str());
                SetFocus(prompt_);
                return true;
            }
            case VK_DOWN:               // Ctrl+↓ — в строку с последней командой
                if (!history_.empty()) {
                    histPos_ = (int)history_.size() - 1;
                    SetWindowTextW(prompt_, history_[histPos_].c_str());
                }
                SetFocus(prompt_);
                return true;
            case 'B':                   // Ctrl+B — ветка (плоский список поддерева)
                Dispatch(IDM_BRANCH);
                return true;
            case 'F':                   // Ctrl+F — FTP: подключиться, Ctrl+Shift+F — отключиться
                Dispatch(shift ? IDM_FTP_DISCONNECT : IDM_FTP_CONNECT);
                return true;
            case 'N':                   // Ctrl+N — новое FTP-соединение (тот же диалог)
                Dispatch(IDM_FTP_CONNECT);
                return true;
            case 'M':
                Dispatch(IDM_BATCH_RENAME);
                return true;
            case VK_F8:
                Dispatch(IDM_TREE);
                return true;
            case 'Z':
                Dispatch(IDM_UNDO_RENAME);
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
                Dispatch(IDM_SORT_DATE);
                return true;
            case VK_F6:
                Dispatch(IDM_SORT_SIZE);
                return true;
            case VK_OEM_5:              // Ctrl+\ — в корень диска
                if (!panel->GoRoot()) ShowError(panel->NavigateError());
                panel->FocusList();
                return true;
            case VK_PRIOR:              // Ctrl+PgUp — на уровень вверх
                GoUpActive();
                panel->FocusList();
                return true;
            case VK_UP:                 // Ctrl+↑ — открыть в новой вкладке (как в TC)
                OpenCursorInNewTab();
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
            Dispatch(shift ? IDM_NEWFILE : IDM_EDIT);   // Shift+F4 — новый текстовый файл
            return true;
        case VK_F5:
            if (shift) {                                // Shift+F5 — копия в этом же каталоге
                DoCopyInPlace();
                return true;
            }
            Dispatch(IDM_COPY);
            return true;
        case VK_F6:
            Dispatch(shift ? IDM_RENAME : IDM_MOVE);    // Shift+F6 — переименование
            return true;
        case VK_F7:
            if (shift) {                                // Shift+F7 — каталог в неактивной панели
                Panel* other = OtherPanel();
                Entry* cur = panel->Cursor();
                DoMkdir(other, (cur && !cur->isUp) ? cur->name : L"");
                return true;
            }
            Dispatch(IDM_MKDIR);
            return true;
        case VK_F8:
            Dispatch(shift ? IDM_DELETE_PERM : IDM_DELETE);
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
                    if (shift) ActivePanel()->PrevTab();
                    else ActivePanel()->NextTab();
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
            Dispatch(shift ? IDM_DELETE_PERM : IDM_DELETE);   // Shift+Del — мимо корзины
            return true;
        default:
            break;
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

    if (msg.wParam == VK_F9 || msg.wParam == VK_F10) {   // F9/F10 — меню, как в TC
        HMENU first = menu_ ? GetSubMenu(menu_, 0) : nullptr;
        if (first) {
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            const int cmd = (int)TrackPopupMenu(first, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                rc.left + 4, rc.top + 4, 0, hwnd_, nullptr);
            if (cmd) Dispatch(cmd);
        }
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

        case WM_APP + 305: {
            // Начать inline-переименование после завершения Dispatch (иначе
            // возврат фокуса в список закроет EDIT).
            Panel* p = ((int)wParam == 0) ? &left_ : &right_;
            p->BeginInlineRename();
            return 0;
        }

        case WM_APP + 401:
            RunNextJob();
            return 0;

        case WM_APP + 300: {
            // Панель сообщила LVN_BEGINDRAG — ведём настоящий OLE-драг,
            // чтобы файлы можно было бросать на рабочий стол и в Проводник.
            Panel* panel = ((int)wParam == 0) ? &left_ : &right_;
            if (panel->InFtp() || panel->InArchive()) {
                MessageBoxW(hwnd_, L"Перетаскивание с FTP или из архива пока не поддерживается.\n"
                                    L"Используйте F5/F6.",
                            L"Перетаскивание", MB_ICONINFORMATION | MB_OK);
                return 0;
            }
            std::vector<std::wstring> paths = panel->SelectedPaths();
            if (paths.empty()) return 0;
            auto* data = new FileDataObject(std::move(paths));
            auto* source = new FileDropSource();
            DWORD effect = 0;
            DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
            data->Release();
            source->Release();
            if (effect != DROPEFFECT_NONE) {
                left_.Reload();
                right_.Reload();
                UpdatePromptLabel();
            }
            return 0;
        }

        case WM_SIZE:
            Layout();
            return 0;

        case WM_CTLCOLORSTATIC: {
            // Версия на главном окне — серым, без своей рамки-фона.
            if (reinterpret_cast<HWND>(lParam) == ver_) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            auto mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scale(600, dpi_);
            mmi->ptMinTrackSize.y = Scale(360, dpi_);
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                shiftHeld_ = false;
            } else if (ActivePanel()) {
                // Вернулись в окно — перечитываем каталог, как TC: снаружи могли
                // появиться или исчезнуть файлы, пока мы были не в фокусе.
                ActivePanel()->Reload();
            }
            break;

        case WM_SETFOCUS:
            if (ActivePanel()) ActivePanel()->FocusList();
            return 0;

        case WM_DRAWITEM: {
            auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON) {
                const int id = (int)dis->CtlID;
                if ((id >= ID_TB_FIRST && id < ID_TB_FIRST + toolbarCount_) ||
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
            if (id >= ID_TB_FIRST && id < ID_TB_FIRST + toolbarCount_) {
                // Сепараторы — не кнопки, команду не диспатчат.
                if (toolbarDefs_[id - ID_TB_FIRST].command) Dispatch(toolbarDefs_[id - ID_TB_FIRST].command);
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
            if (dropTarget_) {
                if (hwnd_) RevokeDragDrop(hwnd_);
                if (left_.List()) RevokeDragDrop(left_.List());
                if (right_.List()) RevokeDragDrop(right_.List());
                dropTarget_->Release();
                dropTarget_ = nullptr;
            }
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

// ---------- FTP ----------

void App::FtpConnectDialog() {
    FtpConnectOptions opts;
    Panel* panel = ActivePanel();

    // Если панель уже на FTP — предлагаем её профиль; иначе первый сохранённый.
    if (panel->InFtp()) {
        FtpSite site;
        std::wstring remote;
        if (FtpParseUrl(panel->Path(), site, remote)) {
            opts.site = site;
            opts.site.root = remote.empty() ? L"/" : remote;
            FtpPasswordFor(panel->Path(), opts.site.password);
        }
    } else if (!GetSettings().ftpSites.empty()) {
        opts.site = GetSettings().ftpSites[0];
    }

    if (!FtpConnectBox(hwnd_, opts, GetSettings().ftpSites)) return;

    // Проверяем вход до сохранения и перехода: иначе ошибка всплывёт уже в панели.
    std::unique_ptr<FtpConnection> probe;
    std::wstring error;
    if (!FtpConnect(opts.site, probe, error)) {
        ShowError(L"Не удалось подключиться к " + opts.site.host + L":\n" + error);
        return;
    }
    FtpDisconnect(probe);

    const std::wstring url = FtpMakeUrl(opts.site, opts.site.root);
    FtpRememberPassword(url, opts.site.password);

    if (opts.saveSite) {
        auto& sites = GetSettings().ftpSites;
        bool updated = false;
        for (auto& saved : sites) {
            if (SamePath(saved.host, opts.site.host) && saved.port == opts.site.port &&
                SamePath(saved.user, opts.site.user)) {
                const std::wstring oldName = saved.name;
                saved = opts.site;
                if (saved.name.empty()) {
                    saved.name = oldName.empty() ? (saved.user.empty() ? saved.host : (saved.user + L"@" + saved.host))
                                                 : oldName;
                }
                updated = true;
                break;
            }
        }
        if (!updated) {
            FtpSite saved = opts.site;
            if (saved.name.empty()) saved.name = saved.user.empty() ? saved.host : (saved.user + L"@" + saved.host);
            sites.push_back(saved);
        }
    }

    LogDebug(L"FTP connect: " + url);
    if (!panel->Navigate(url)) {
        ShowError(panel->NavigateError());
        return;
    }
    panel->FocusList();
    UpdatePromptLabel();
}

void App::FtpDisconnectActive() {
    Panel* panel = ActivePanel();
    if (!panel->InFtp()) {
        MessageBoxW(hwnd_, L"Активная панель не подключена к FTP.", L"FTP", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!panel->Navigate(ExeDir())) ShowError(panel->NavigateError());
    UpdatePromptLabel();
}

// Скачать файл с FTP во временный каталог: просмотр, открытие, сравнение.
std::wstring App::DownloadFtpToTemp(const std::wstring& url, const std::wstring& name) {
    FtpSite site;
    std::wstring remote;
    if (!FtpParseUrl(url, site, remote)) {
        ShowError(L"Некорректный FTP-адрес:\n" + url);
        return {};
    }
    if (!site.user.empty() && !FtpPasswordFor(url, site.password)) {
        ShowError(L"Нет пароля для " + site.user + L"@" + site.host + L".\n"
                  L"Подключитесь через меню «Сеть» → «FTP: подключиться…».");
        return {};
    }

    std::unique_ptr<FtpConnection> conn;
    std::wstring error;
    if (!FtpConnect(site, conn, error)) {
        ShowError(L"Не удалось подключиться к " + site.host + L":\n" + error);
        return {};
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"posfiles-ftp";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring safe = name.empty() ? L"file" : name;
    for (auto& ch : safe) {
        if (ch == L'\\' || ch == L'/' || ch == L':') ch = L'_';
    }
    const std::wstring local = dir + L"\\" + safe;

    const bool ok = FtpDownloadFile(*conn, remote, local, nullptr, error);
    FtpDisconnect(conn);
    if (!ok) {
        ShowError(L"Не удалось скачать файл:\n" + remote + L"\n\n" + error);
        return {};
    }
    return local;
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

    // Сквозная проверка FTP: --ftp-test host:port:user:pass
    if (cmdLine && wcsstr(cmdLine, L"--ftp-test")) {
        std::wstring args = cmdLine;
        const size_t pos = args.find(L"--ftp-test");
        args = Trim(args.substr(pos + 10));
        return RunFtpTest(args);
    }

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

    HRESULT hrOle = OleInitialize(nullptr);

    App app;
    int code = 0;
    if (app.Create(inst)) {
        code = app.Run();
    } else {
        MessageBoxW(nullptr, L"Не удалось создать главное окно.", L"PosFiles", MB_ICONERROR);
        code = 1;
    }

    if (SUCCEEDED(hrOle)) OleUninitialize();
    return code;
}
