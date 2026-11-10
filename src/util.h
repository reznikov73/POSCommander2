// Работа с файловой системой, путями и строками: без окон и без Win32-сообщений.
#pragma once

#include "common.h"

#include <atomic>

// Запись в панели: файл, каталог или строка "..".
struct Entry {
    std::wstring name;
    ULONGLONG size = 0;
    FILETIME ft{};
    DWORD attr = 0;
    bool isDir = false;
    bool isUp = false;
    bool marked = false;
    int icon = -1;          // индекс в системном списке значков, -1 = ещё не взят
    bool dirSizeKnown = false;   // размер каталога посчитан
    bool dirSizeBusy = false;    // размер считается прямо сейчас
    ULONGLONG dirSize = 0;       // суммарный размер содержимого каталога

    bool hidden() const {
        return (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    }
    bool isDirFs() const { return isDir && !isUp; }
    // Расширение вместе с точкой; у каталогов и файлов без расширения пусто.
    std::wstring ext() const;
    // Имя без расширения (так Total Commander показывает колонку "Имя").
    std::wstring stem() const;
};

struct DriveItem {
    wchar_t letter = L'C';
    UINT type = DRIVE_FIXED;
};

// Перечисление каталога. Возвращает false и код ошибки при отказе доступа.
bool ListDir(const std::wstring& dir, bool showHidden, std::vector<Entry>& out, DWORD& error);

// Прижимает прямоугольник окна к рабочей области, чтобы окно не открылось
// за пределами экрана после смены разрешения.
RECT ClampWindowRect(const RECT& r);
// Диагностический лог в %TEMP%\posfiles-debug.log (append, UTF-8).
void LogDebug(const std::wstring& text);
// Суммарный размер содержимого каталога (рекурсивно, без переходов по ссылкам).
ULONGLONG DirectorySize(const std::wstring& dir, const std::atomic<bool>* cancel = nullptr);

// Строка атрибутов в стиле TC: «-a--» (r/h/s/a/d, прочерк — нет атрибута).
std::wstring AttrMask(DWORD attr);

// Текст для быстрого просмотра (Ctrl+Q). Бинарный файл отдаётся hex-дампом,
// binary = true. Читается не больше maxBytes.
std::wstring LoadTextPreview(const std::wstring& path, size_t maxBytes, bool& binary);

std::vector<DriveItem> GetDrives();
std::wstring DriveRootOf(const std::wstring& path);
// "1 253,6 Гб свободно из 3 726,0 Гб" для строки состояния.
std::wstring VolumeSummary(const std::wstring& path);

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring ParentPath(const std::wstring& dir);
bool IsRootPath(const std::wstring& path);
// Приводит путь к абсолютному виду без завершающей обратной черты (кроме корня).
std::wstring NormalizeDir(const std::wstring& path);
std::wstring ExeDir();
std::wstring DrivePrefix(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool SamePath(const std::wstring& a, const std::wstring& b);

std::wstring FmtSize(ULONGLONG bytes);
std::wstring FmtNum(ULONGLONG value);
std::wstring FmtDate(const FILETIME& ft);
std::wstring ErrText(DWORD error);
std::wstring Trim(const std::wstring& text);
std::wstring Lower(std::wstring text);
std::wstring Upper(std::wstring text);

std::vector<std::wstring> SplitMasks(const std::wstring& masks);
bool MatchMask(const std::wstring& name, const std::wstring& mask);

// Системные значки оболочки: один общий список на всё приложение.
void InitShellIcons();
HIMAGELIST ShellSmallImageList();
// dir — каталог панели, нужен для иконок конкретных папок и файлов.
int IconForEntry(const Entry& e, const std::wstring& dir = L"");
// FTP: без обращений к оболочке — только свои значки по расширению (быстро).
int IconForFtpEntry(const Entry& e);

// Значки тулбара: рисуем сами, в стиле Total Commander (см. ui-target.html).
enum ToolIconKind {
    TIK_BACK, TIK_FWD, TIK_UP, TIK_TREE,
    TIK_VIEW, TIK_EDIT, TIK_COPY, TIK_MOVE, TIK_MKDIR, TIK_DEL,
    TIK_PACK, TIK_UNPACK, TIK_RENM,
    TIK_FIND, TIK_SYNC, TIK_REFRESH, TIK_PROPS,
};
HICON MakeToolIcon(int kind, int size);

UINT WindowDpi(HWND hwnd);
// Естественная сортировка: "файл2" раньше "файл10".
int CompareNatural(const std::wstring& a, const std::wstring& b);
