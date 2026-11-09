// Реализация утилит: пути, форматирование, перечисление каталогов, иконки.
#include "util.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <cstdio>
#include <cwchar>

// ---------- строки ----------

std::wstring Trim(const std::wstring& text) {
    size_t b = 0, e = text.size();
    while (b < e && iswspace(text[b])) b++;
    while (e > b && iswspace(text[e - 1])) e--;
    return text.substr(b, e - b);
}

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), (DWORD)text.size());
    return text;
}

std::wstring Upper(std::wstring text) {
    if (!text.empty()) CharUpperBuffW(text.data(), (DWORD)text.size());
    return text;
}

static bool SameNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return SameNoCase(a, b); }

std::wstring ErrText(DWORD error) {
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring out;
    if (len && buf) out.assign(buf, len);
    if (buf) LocalFree(buf);
    out = Trim(out);
    if (out.empty()) {
        wchar_t tmp[64];
        swprintf(tmp, 64, L"код ошибки %lu", error);
        out = tmp;
    }
    return out;
}

// ---------- пути ----------

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += name;
    return out;
}

bool IsRootPath(const std::wstring& path) {
    if (path.size() == 3 && path[1] == L':' && path[2] == L'\\') return true;
    if (path.size() == 2 && path[1] == L':') return true;
    if (path == L"\\") return true;
    // UNC: \\сервер\общий — корень, если после общего имени нет ни одной черты.
    if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t first = path.find(L'\\', 2);
        if (first == std::wstring::npos) return true;
        size_t second = path.find(L'\\', first + 1);
        return second == std::wstring::npos ||
               (second + 1 == path.size());
    }
    return false;
}

std::wstring ParentPath(const std::wstring& dir) {
    if (dir.empty() || IsRootPath(dir)) return dir;
    std::wstring base = dir;
    if (base.back() == L'\\') base.pop_back();
    size_t pos = base.find_last_of(L'\\');
    if (pos == std::wstring::npos) return base;
    std::wstring out = base.substr(0, pos);
    if (out.empty()) return L"\\";
    if (out.size() == 2 && out[1] == L':') out.push_back(L'\\');
    return out;
}

std::wstring DrivePrefix(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return path.substr(0, 2);
    if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t first = path.find(L'\\', 2);
        if (first != std::wstring::npos) {
            size_t second = path.find(L'\\', first + 1);
            if (second != std::wstring::npos) return path.substr(0, second);
            return path;
        }
    }
    return {};
}

std::wstring DriveRootOf(const std::wstring& path) {
    std::wstring prefix = DrivePrefix(path);
    if (prefix.empty()) return L"C:\\";
    if (prefix.size() == 2) return prefix + L"\\";
    return prefix;
}

std::wstring NormalizeDir(const std::wstring& path) {
    std::wstring in = Trim(path);
    if (in.size() >= 2 && in.front() == L'"' && in.back() == L'"') in = in.substr(1, in.size() - 2);
    for (auto& ch : in) if (ch == L'/') ch = L'\\';
    if (in.empty()) return in;

    // %TEMP%, %USERPROFILE%, %HOMEDRIVE%%HOMEPATH% и т. п. — как в TC: переменные
    // окружения раскрываются в пути из поля пути, избранного и истории.
    if (in.find(L'%') != std::wstring::npos) {
        wchar_t expanded[32768]{};
        DWORD need = ExpandEnvironmentStringsW(in.c_str(), expanded, 32768);
        if (need > 0 && need <= 32768) in = expanded;
    }

    if (in == L"\\") {
        wchar_t cwd[MAX_PATH]{};
        if (GetCurrentDirectoryW(MAX_PATH, cwd)) return DriveRootOf(cwd);
        return L"C:\\";
    }
    if (in.size() == 2 && in[1] == L':') in.push_back(L'\\');

    wchar_t full[32768]{};
    DWORD n = GetFullPathNameW(in.c_str(), 32768, full, nullptr);
    std::wstring out = (n > 0 && n < 32768) ? std::wstring(full, n) : in;

    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    if (out.size() == 3 && out[1] == L':' && out[2] == L'\\') {
        wchar_t letter = (wchar_t)towupper(out[0]);
        out[0] = letter;
        return out;
    }
    // \\сервер\общий\  ->  \\сервер\общий
    if (out.size() > 2 && out[0] == L'\\' && out[1] == L'\\' && out.back() == L'\\') out.pop_back();
    return out;
}

std::wstring ExeDir() {
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    std::wstring out(path, n);
    size_t pos = out.find_last_of(L'\\');
    return pos == std::wstring::npos ? out : out.substr(0, pos);
}

bool IsDirectory(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ---------- форматирование ----------

std::wstring FmtNum(ULONGLONG value) {
    wchar_t raw[32];
    swprintf(raw, 32, L"%llu", value);
    std::wstring digits(raw), out;
    int count = 0;
    for (int i = (int)digits.size() - 1; i >= 0; --i) {
        out.insert(out.begin(), digits[i]);
        if (++count % 3 == 0 && i > 0) out.insert(out.begin(), L' ');
    }
    return out;
}

// Первые maxBytes файла как текст: UTF-16 по BOM, иначе UTF-8, иначе ANSI.
// Признак бинарного файла — нулевой байт в начале.
std::wstring LoadTextPreview(const std::wstring& path, size_t maxBytes, bool& binary) {
    binary = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"<не удалось открыть файл: " + ErrText(GetLastError()) + L">";

    std::vector<char> buf(maxBytes + 4);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buf.data(), (DWORD)maxBytes, &read, nullptr);
    CloseHandle(file);
    if (!ok) return L"<не удалось прочитать файл>";

    buf.resize(read);
    if (read == 0) return L"<пустой файл>";

    // UTF-16 с BOM
    if (read >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(buf.data() + 2), (read - 2) / 2);
    }

    bool hasNul = false;
    for (size_t i = 0; i < read && i < 4096; ++i) {
        if (buf[i] == 0) {
            hasNul = true;
            break;
        }
    }
    if (hasNul) {
        static const wchar_t* hex = L"0123456789ABCDEF";
        std::wstring out;
        out.reserve(80 * (read / 16 + 1));
        wchar_t line[8];
        for (size_t off = 0; off < read; off += 16) {
            swprintf(line, 8, L"%06zX  ", off);
            out += line;
            for (size_t i = 0; i < 16; ++i) {
                if (off + i < read) {
                    const BYTE b = (BYTE)buf[off + i];
                    out += hex[b >> 4];
                    out += hex[b & 0x0F];
                } else {
                    out += L"  ";
                }
                out += L' ';
                if (i == 7) out += L' ';
            }
            out += L' ';
            for (size_t i = 0; i < 16 && off + i < read; ++i) {
                const BYTE b = (BYTE)buf[off + i];
                out += (b >= 32 && b < 127) ? (wchar_t)b : L'.';
            }
            out += L"\r\n";
        }
        binary = true;
        return out;
    }

    size_t start = 0;
    if (read >= 3 && (BYTE)buf[0] == 0xEF && (BYTE)buf[1] == 0xBB && (BYTE)buf[2] == 0xBF) start = 3;
    UINT cp = CP_ACP;
    if (start == 3) {
        cp = CP_UTF8;
    } else {
        int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data(), (int)read, nullptr, 0);
        if (wide > 0) cp = CP_UTF8;
    }
    const char* src = buf.data() + start;
    const int srcLen = (int)(read - start);
    int need = MultiByteToWideChar(cp, 0, src, srcLen, nullptr, 0);
    if (need <= 0) return L"<не удалось декодировать текст>";
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(cp, 0, src, srcLen, &out[0], need);
    return out;
}

std::wstring AttrMask(DWORD attr) {
    std::wstring out;
    out += (attr & FILE_ATTRIBUTE_READONLY) ? L'r' : L'-';
    out += (attr & FILE_ATTRIBUTE_HIDDEN) ? L'h' : L'-';
    out += (attr & FILE_ATTRIBUTE_SYSTEM) ? L's' : L'-';
    out += (attr & FILE_ATTRIBUTE_ARCHIVE) ? L'a' : L'-';
    out += (attr & FILE_ATTRIBUTE_DIRECTORY) ? L'd' : L'-';
    return out;
}

std::wstring FmtSize(ULONGLONG bytes) {
    if (bytes < 1024) {
        wchar_t b[64];
        swprintf(b, 64, L"%llu байт", bytes);
        return b;
    }
    static const wchar_t* units[] = {L"Кб", L"Мб", L"Гб", L"Тб", L"Пб"};
    double value = (double)bytes;
    int unit = -1;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    wchar_t b[64];
    swprintf(b, 64, L"%.1f %ls", value, units[unit]);
    std::wstring out(b);
    std::replace(out.begin(), out.end(), L'.', L',');
    return out;
}

std::wstring FmtDate(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return {};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return {};
    if (!FileTimeToSystemTime(&local, &st)) return {};
    wchar_t b[64];
    swprintf(b, 64, L"%02u.%02u.%02u %02u:%02u", st.wDay, st.wMonth, st.wYear % 100, st.wHour, st.wMinute);
    return b;
}

std::wstring Entry::ext() const {
    if (isDir) return {};
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return {};
    return name.substr(dot);
}

std::wstring Entry::stem() const {
    if (isDir) return name;
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return name;
    return name.substr(0, dot);
}

// ---------- маски ----------

std::vector<std::wstring> SplitMasks(const std::wstring& masks) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < masks.size()) {
        while (i < masks.size() && (iswspace(masks[i]) || masks[i] == L';' || masks[i] == L',')) i++;
        size_t start = i;
        while (i < masks.size() && !iswspace(masks[i]) && masks[i] != L';' && masks[i] != L',') i++;
        if (i > start) {
            std::wstring item = masks.substr(start, i - start);
            if (item.find_first_of(L"*?") == std::wstring::npos) item = L"*" + item + L"*";
            out.push_back(Lower(item));
        }
    }
    if (out.empty()) out.push_back(L"*");
    return out;
}

bool MatchMask(const std::wstring& name, const std::wstring& mask) {
    // Итеративный поиск с возвратом: * и ? без рекурсии.
    const wchar_t* s = name.c_str();
    const wchar_t* p = mask.c_str();
    const wchar_t* star = nullptr;
    const wchar_t* resume = nullptr;
    while (*s) {
        if (*p == L'?' || towlower(*p) == towlower(*s)) {
            s++; p++;
        } else if (*p == L'*') {
            star = p++;
            resume = s;
        } else if (star) {
            p = star + 1;
            s = ++resume;
        } else {
            return false;
        }
    }
    while (*p == L'*') p++;
    return *p == 0;
}

// ---------- каталоги и диски ----------

bool ListDir(const std::wstring& dir, bool showHidden, std::vector<Entry>& out, DWORD& error) {
    out.clear();
    error = 0;
    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        // Пустой корень пустого устройства — это не ошибка.
        if (err == ERROR_FILE_NOT_FOUND) return true;
        error = err;
        return false;
    }

    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if (!showHidden && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;

        Entry e;
        e.name = data.cFileName;
        e.attr = data.dwFileAttributes;
        e.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = e.isDir ? 0 : ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        e.ft = data.ftLastWriteTime;
        out.push_back(std::move(e));
    } while (FindNextFileW(find, &data));

    FindClose(find);
    return true;
}

// Рекурсивный подсчёт размера каталога. Ссылки и точки повторной обработки пропускаем.
ULONGLONG DirectorySize(const std::wstring& dir, const std::atomic<bool>* cancel) {
    ULONGLONG total = 0;

    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;

    std::vector<std::wstring> subdirs;
    do {
        if (cancel && cancel->load()) break;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            subdirs.push_back(JoinPath(dir, data.cFileName));
        } else {
            total += ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);

    for (auto& sub : subdirs) {
        if (cancel && cancel->load()) break;
        total += DirectorySize(sub, cancel);
    }
    return total;
}

std::vector<DriveItem> GetDrives() {
    std::vector<DriveItem> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        wchar_t root[8] = {letter, L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type == DRIVE_NO_ROOT_DIR) continue;
        out.push_back({letter, type});
    }
    return out;
}

std::wstring VolumeSummary(const std::wstring& path) {
    std::wstring root = DriveRootOf(path);
    ULARGE_INTEGER freeBytes{}, total{}, totalFree{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &total, &totalFree)) return {};
    // Как в макете: «[рабочий] 1 253,4 ГБ из 3 726,0 ГБ свободно».
    std::wstring out = FmtSize(freeBytes.QuadPart) + L" из " + FmtSize(total.QuadPart) + L" свободно";
    wchar_t label[MAX_PATH + 1]{};
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && label[0]) {
        out = std::wstring(L"[") + label + L"] " + out;
    }
    return out;
}

// ---------- иконки оболочки ----------

static HIMAGELIST g_images = nullptr;          // наш список: сюда можно добавлять свои значки
static int g_iconFolder = -1;
static int g_iconFile = -1;
static int g_iconUp = -1;
static int g_iconFolderHidden = -1;
static int g_iconFileHidden = -1;
static int g_iconSize = 16;   // фактический размер малых значков (с учётом DPI)

// Наши иконки типов файлов (макет ui-target.html): лист, текст, код,
// приложение, архив, изображение, ярлык.
enum IconTypeKind {
    kTkFile = 0, kTkText, kTkCode, kTkApp, kTkArchive, kTkImage, kTkLink, kTkCount
};
// Ленивый кэш: kind*2 + (скрытый?) → индекс в g_images; -1 = ещё не создана.
static int g_typeIcons[kTkCount * 2] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1};

static std::map<std::wstring, int> g_iconCache;
static const size_t kIconCacheLimit = 3000;

// Пиксельный холст 32-бит с альфой: рисуем иконки сами, чтобы выглядели как в TC.
struct PixelCanvas {
    int size = 16;
    std::vector<DWORD> px;

    explicit PixelCanvas(int s) : size(s), px((size_t)s * s, 0) {}

    void Set(int x, int y, DWORD argb) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        px[(size_t)y * size + x] = argb;
    }

    void Fill(int x0, int y0, int x1, int y1, DWORD argb) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) Set(x, y, argb);
    }

    void HLine(int x0, int x1, int y, DWORD argb) {
        for (int x = x0; x <= x1; ++x) Set(x, y, argb);
    }

    void VLine(int x, int y0, int y1, DWORD argb) {
        for (int y = y0; y <= y1; ++y) Set(x, y, argb);
    }

    HICON ToIcon() const {
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = size;
        bi.bV5Height = -size;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        HDC screen = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        HICON icon = nullptr;

        if (color && bits && mask) {
            std::memcpy(bits, px.data(), px.size() * sizeof(DWORD));
            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmMask = mask;
            ii.hbmColor = color;
            icon = CreateIconIndirect(&ii);
        }
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        ReleaseDC(nullptr, screen);
        return icon;
    }
};

// ---------- палитра панельных иконок (по макету ui-target.html) ----------
namespace {
const DWORD kColFolderEdge  = 0xFFC89A30;   // контур папки
const DWORD kColFolderBody  = 0xFFFFD061;   // корпус
const DWORD kColFolderFront = 0xFFFFE9A8;   // передняя грань
const DWORD kColFolderGloss = 0xFFFFF3C9;   // блик внизу
const DWORD kColHiddenBand  = 0xFFD03030;   // полоса скрытого/системного
const DWORD kColSheetEdge   = 0xFF7E8794;   // контур листа
const DWORD kColSheetPaper  = 0xFFFFFFFF;
const DWORD kColSheetFold   = 0xFFD9DEE7;   // загнутый уголок
const DWORD kColTextLine    = 0xFF6B7280;
const DWORD kColCodeBlue    = 0xFF1E5FA8;
const DWORD kColCodeGreen   = 0xFF2A7A2A;
const DWORD kColWinFace     = 0xFFF2F3F7;   // окно приложения/ярлыка
const DWORD kColWinEdge     = 0xFF4A5568;
const DWORD kColWinTitle    = 0xFF2A5AAA;
const DWORD kColWinBar      = 0xFFC7D2E4;
const DWORD kColZipFace     = 0xFFE8B95C;   // архив
const DWORD kColZipEdge     = 0xFF9C6B1E;
const DWORD kColZipStrap    = 0xFFB8862F;
const DWORD kColZipBuckle   = 0xFFECECEC;
const DWORD kColZipBuckleE  = 0xFF7A7A7A;
const DWORD kColZipSlot     = 0xFF4D4D4D;
const DWORD kColImgSun      = 0xFFFFD34D;   // изображение
const DWORD kColImgSunEdge  = 0xFFD99E1B;
const DWORD kColImgHill     = 0xFF3E9E4F;
const DWORD kColLnkArrow    = 0xFF1257C1;   // ярлык
}  // namespace

// Красная полоса по левому краю иконки — признак скрытого/системного, как в TC.
void PaintHiddenMark(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    const int x1 = P(4.3), y0 = P(3.0), y1 = P(12.9);
    c.Fill(P(1.2), y0, x1, y1, kColHiddenBand);
    c.Set(x1 + 1, y0 + 1, kColHiddenBand);   // скос верхнего края
    c.Set(x1 + 2, y0 + 2, kColHiddenBand);
}

// Жёлтая папка TC: тёмный язычок сверху, корпус #FFD061, передняя грань
// #FFE9A8, блик #FFF3C9, контур #C89A30 (макет ui-target.html).
HICON MakeFolderIcon(int size, bool hidden = false) {
    PixelCanvas c(size);
    const int s = size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };

    // задняя стенка с язычком (тёмный янтарь)
    c.Fill(P(1.6), P(3.2), P(6.0), P(6.2), kColFolderEdge);
    c.Fill(P(7.7), P(5.1), P(14.5), P(6.2), kColFolderEdge);
    c.Set(P(6.5), P(3.6), kColFolderEdge);   // скос между язычком и стенкой
    c.Set(P(7.2), P(4.5), kColFolderEdge);

    // корпус с передней гранью и бликом
    c.Fill(P(1.6), P(6.2), P(14.5), P(12.2), kColFolderBody);
    c.Fill(P(1.6), P(6.2), P(14.5), P(7.8), kColFolderFront);
    c.Fill(P(2.6), P(10.2), P(13.5), P(11.3), kColFolderGloss);

    // контур корпуса
    c.HLine(P(1.6), P(14.5), P(6.2), kColFolderEdge);
    c.HLine(P(1.6), P(14.5), P(12.8), kColFolderEdge);
    c.VLine(P(1.6), P(6.2), P(12.8), kColFolderEdge);
    c.VLine(P(14.5), P(6.2), P(12.8), kColFolderEdge);

    if (hidden) PaintHiddenMark(c);
    return c.ToIcon();
}

// Общий базовый «лист с загнутым уголком» для файловых иконок.
void PaintSheet(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    const int xl = P(3.2), xr = P(12.8), yt = P(1.5), yb = P(14.2);
    const int xf = P(9.6), yf = P(4.7);

    c.Fill(xl, yt, xr, yb, kColSheetPaper);

    // срез страницы над диагональю + сам загнутый уголок
    for (int x = xf; x <= xr; ++x) {
        const double t = double(x - xf) / double(xr - xf);
        const int yEdge = yt + (int)lround(t * (yf - yt));
        for (int y = yt; y < yEdge; ++y) c.Set(x, y, 0);              // прозрачный срез
        for (int y = yEdge; y <= yf; ++y) c.Set(x, y, kColSheetFold);  // сгиб
    }

    // контуры
    c.HLine(xl, xf, yt, kColSheetEdge);
    c.VLine(xl, yt, yb, kColSheetEdge);
    c.HLine(xl, xr, yb, kColSheetEdge);
    c.VLine(xr, yf, yb, kColSheetEdge);
    c.VLine(xf, yt + 1, yf, kColSheetEdge);
    c.HLine(xf, xr, yf, kColSheetEdge);
}

// Текст (.txt .log .ini): серые строки.
void PaintTextDeco(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    c.HLine(P(5.0), P(11.0), P(7.0), kColTextLine);
    c.HLine(P(5.0), P(11.0), P(9.2), kColTextLine);
    c.HLine(P(5.0), P(9.6), P(11.4), kColTextLine);
}

// Код (.cpp .h .c .py): маркеры синего, зелёного и серого.
void PaintCodeDeco(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    c.HLine(P(5.0), P(11.2), P(7.0), kColCodeBlue);
    c.HLine(P(5.0), P(10.2), P(9.4), kColCodeGreen);
    c.HLine(P(5.0), P(11.2), P(11.8), kColTextLine);
}

// Изображение (.jpg .png .bmp): солнце и горы на листе.
void PaintImageDeco(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };

    // солнце с ободком
    const int cx = P(6.1), cy = P(7.0);
    c.Set(cx, cy - 1, kColImgSun);
    c.Set(cx - 1, cy, kColImgSun);
    c.Set(cx, cy, kColImgSun);
    c.Set(cx + 1, cy, kColImgSun);
    c.Set(cx, cy + 1, kColImgSun);
    c.Set(cx - 1, cy - 1, kColImgSunEdge);
    c.Set(cx + 1, cy - 1, kColImgSunEdge);
    c.Set(cx - 1, cy + 1, kColImgSunEdge);
    c.Set(cx + 1, cy + 1, kColImgSunEdge);

    // горы: основание и два холма
    const int hy = P(12.6);
    c.Fill(P(4.0), P(11.4), P(12.4), hy, kColImgHill);
    for (int i = 1; i <= 3; ++i) {
        c.Set(P(7.0) - i, P(9.4) + i, kColImgHill);
        c.Set(P(7.0) + i, P(10.0) + i, kColImgHill);
    }
    for (int i = 1; i <= 2; ++i) {
        c.Set(P(10.6) - i, P(10.4) + i, kColImgHill);
        if (P(10.6) + i <= P(12.4)) c.Set(P(10.6) + i, P(10.8) + i, kColImgHill);  // не вылезаем на кромку
    }
}

// Окно приложения (.exe .dll): рамка, синий заголовок с точками, серые строки.
void PaintAppWindow(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    const int xl = P(2.2), xr = P(13.8), yt = P(2.8), yb = P(13.2);

    c.Fill(xl, yt, xr, yb, kColWinFace);
    c.Fill(xl, yt, xr, P(5.2), kColWinTitle);
    c.Set(P(4.0), P(4.2), 0xFFFFFFFF);
    c.Set(P(5.7), P(4.2), 0xFFFFFFFF);
    c.HLine(P(3.5), P(12.7), P(7.0), kColWinBar);
    c.HLine(P(3.5), P(9.7), P(9.1), kColWinBar);
    c.HLine(P(3.5), P(11.3), P(11.2), kColWinBar);

    c.HLine(xl, xr, yt, kColWinEdge);
    c.HLine(xl, xr, yb, kColWinEdge);
    c.VLine(xl, yt, yb, kColWinEdge);
    c.VLine(xr, yt, yb, kColWinEdge);
    c.HLine(xl, xr, P(5.2), kColWinEdge);
}

// Архив (.zip .rar .7z): жёлтая коробка с ремнём и пряжкой.
void PaintArchive(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };
    const int xl = P(2.6), xr = P(13.4), yt = P(3.4), yb = P(13.4);

    c.Fill(xl, yt, xr, yb, kColZipFace);
    c.Fill(P(6.4), yt, P(9.6), yb, kColZipStrap);

    // пряжка с прорезью
    const int bx0 = P(5.2), bx1 = P(10.5), by0 = P(6.4), by1 = P(9.6);
    c.Fill(bx0, by0, bx1, by1, kColZipBuckle);
    c.Fill(P(7.1), P(7.2), P(8.9), P(8.8), kColZipSlot);
    c.HLine(bx0, bx1, by0, kColZipBuckleE);
    c.HLine(bx0, bx1, by1, kColZipBuckleE);
    c.VLine(bx0, by0, by1, kColZipBuckleE);
    c.VLine(bx1, by0, by1, kColZipBuckleE);

    c.HLine(xl, xr, yt, kColZipEdge);
    c.HLine(xl, xr, yb, kColZipEdge);
    c.VLine(xl, yt, yb, kColZipEdge);
    c.VLine(xr, yt, yb, kColZipEdge);
}

// Ярлык (.lnk): окно + страница со синей стрелкой внизу слева.
void PaintLink(PixelCanvas& c) {
    const int s = c.size;
    auto P = [&](double v) { return (int)lround(v * s / 16.0); };

    // окно справа
    const int wxl = P(3.6), wxr = P(14.2), wyt = P(1.6), wyb = P(14.4);
    c.Fill(wxl, wyt, wxr, wyb, kColWinFace);
    c.Fill(wxl, wyt, wxr, P(4.2), kColWinTitle);
    c.HLine(P(6.0), P(12.6), P(6.8), kColWinBar);
    c.HLine(P(6.0), P(10.0), P(8.6), kColWinBar);
    c.HLine(wxl, wxr, wyt, kColWinEdge);
    c.HLine(wxl, wxr, wyb, kColWinEdge);
    c.VLine(wxl, wyt, wyb, kColWinEdge);
    c.VLine(wxr, wyt, wyb, kColWinEdge);
    c.HLine(wxl, wxr, P(4.2), kColWinEdge);

    // страница внизу слева
    const int pxl = P(0.8), pxr = P(6.8), pyt = P(9.2), pyb = P(15.2);
    c.Fill(pxl, pyt, pxr, pyb, kColSheetPaper);
    c.HLine(pxl, pxr, pyt, kColSheetEdge);
    c.HLine(pxl, pxr, pyb, kColSheetEdge);
    c.VLine(pxl, pyt, pyb, kColSheetEdge);
    c.VLine(pxr, pyt, pyb, kColSheetEdge);

    // стрелка из нижнего левого угла страницы
    for (int i = 0; i < 4; ++i) {
        c.Set(P(2.1) + i, P(13.8) - i, kColLnkArrow);
        c.Set(P(2.1) + 1 + i, P(13.8) - i, kColLnkArrow);
    }
    c.HLine(P(3.5), P(5.6), P(10.2), kColLnkArrow);
    c.VLine(P(5.6), P(10.2), P(12.3), kColLnkArrow);
}

// Иконка типа файла: базовый лист/окно + оформление по типу (макет).
HICON MakeTypeIcon(int kind, int size, bool hidden = false) {
    PixelCanvas c(size);
    switch (kind) {
        case kTkText:    PaintSheet(c); PaintTextDeco(c); break;
        case kTkCode:    PaintSheet(c); PaintCodeDeco(c); break;
        case kTkImage:   PaintSheet(c); PaintImageDeco(c); break;
        case kTkApp:     PaintAppWindow(c); break;
        case kTkArchive: PaintArchive(c); break;
        case kTkLink:    PaintLink(c); break;
        default:         PaintSheet(c); break;   // обычный файл
    }
    if (hidden) PaintHiddenMark(c);
    return c.ToIcon();
}

// Стрелка «вверх» для строки «..» — рисуем сами, как в Total Commander.
HICON MakeUpArrowIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;                 // сверху вниз
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HICON icon = nullptr;

    if (color && bits && mask) {
        DWORD* px = reinterpret_cast<DWORD*>(bits);
        std::memset(px, 0, (size_t)size * size * 4);

        // Простая стрелка: древко внизу по центру и треугольник сверху.
        const int cx = size / 2;
        const int headH = size / 2;
        const DWORD solid = 0xFF141414;     // почти чёрная, как в макете

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                bool fill = false;

                // треугольник-наконечник
                if (y < headH) {
                    const int spread = (y * cx) / (headH > 0 ? headH : 1);
                    if (x >= cx - spread && x <= cx + spread) fill = true;
                }
                // древко
                const int shaftW = size >= 16 ? 2 : 1;
                if (y >= headH && x >= cx - shaftW && x <= cx + shaftW) fill = true;

                if (fill) px[(size_t)y * size + x] = solid;
            }
        }

        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        icon = CreateIconIndirect(&ii);
    }

    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    ReleaseDC(nullptr, screen);
    return icon;
}

// ─── значки тулбара 16×16: пиксель-арт в стиле Total Commander ───────
// Рисуется в координатах 16×16; при другом размере масштабируется
// ближайшим соседом (dpi 125% -> 20, 150% -> 24).
// Пиксель-арт проверен по макету ui-target.html (см. scripts/icon_preview.cpp).
void ToolIconPixels(int kind, PixelCanvas& c) {
    const unsigned kGreen   = 0xFF2A7A2A;
    const unsigned kTree     = 0xFF2E7D32;
    const unsigned kBlue     = 0xFF1E5FA8;
    const unsigned kPupil    = 0xFF0E2A50;
    const unsigned kAmber    = 0xFFF5A623;
    const unsigned kPencilEdge = 0xFF8A5A12;
    const unsigned kWood     = 0xFFC8A050;
    const unsigned kLead     = 0xFF3A3A3A;
    const unsigned kEraser   = 0xFFD06060;
    const unsigned kGray     = 0xFF7E8794;
    const unsigned kPaper    = 0xFFFFFFFF;
    const unsigned kPaper2   = 0xFFE6E6E6;
    const unsigned kEdge     = 0xFF8A8A8A;
    const unsigned kText     = 0xFF9A9A9A;
    const unsigned kRed      = 0xFFD03030;
    const unsigned kBox      = 0xFFE8B95C;
    const unsigned kBoxEdge  = 0xFFB08234;
    const unsigned kBoxLid   = 0xFFF0DCA8;
    const unsigned kBinoc    = 0xFF39506E;
    const unsigned kGear     = 0xFF6B7280;
    const unsigned kSync     = 0xFF2A5AAA;
    const unsigned kRefresh  = 0xFF1E6FC0;
    const unsigned kFEdge    = 0xFFB8860B;
    const unsigned kFBody    = 0xFFFFE9A8;
    const unsigned kFDark    = 0xFFFFD061;

    switch (kind) {
        case TIK_BACK:                                  // назад: зелёная стрелка влево
            for (int k = 0; k < 5; ++k) c.VLine(3 + k, 7 - k, 8 + k, kGreen);
            c.Fill(8, 7, 13, 8, kGreen);
            break;
        case TIK_FWD:                                   // вперёд
            for (int k = 0; k < 5; ++k) c.VLine(12 - k, 7 - k, 8 + k, kGreen);
            c.Fill(2, 7, 7, 8, kGreen);
            break;
        case TIK_UP:                                    // вверх
            for (int k = 0; k < 5; ++k) c.HLine(7 - k, 8 + k, 3 + k, kGreen);
            c.Fill(7, 8, 8, 13, kGreen);
            break;
        case TIK_TREE: {                                  // дерево каталогов: ствол + ветви + узлы
            auto node = [&c](int x0, int y0) {
                c.HLine(x0, x0 + 4, y0, kTree);
                c.HLine(x0, x0 + 4, y0 + 4, kTree);
                c.VLine(x0, y0, y0 + 4, kTree);
                c.VLine(x0 + 4, y0, y0 + 4, kTree);
            };
            c.Fill(2, 2, 3, 13, kTree);                   // ствол 2px
            c.Fill(4, 4, 7, 5, kTree);                    // ветвь 1
            c.Fill(4, 9, 7, 10, kTree);                   // ветвь 2
            node(8, 2);
            node(8, 9);
            break;
        }
        case TIK_VIEW: {                                // просмотр: глаз
            c.HLine(5, 10, 3, kBlue);
            c.Set(3, 4, kBlue); c.Set(4, 4, kBlue); c.Set(11, 4, kBlue); c.Set(12, 4, kBlue);
            c.Set(2, 5, kBlue); c.Set(13, 5, kBlue);
            c.Set(1, 6, kBlue); c.Set(14, 6, kBlue);
            c.Set(1, 7, kBlue); c.Set(14, 7, kBlue);
            c.Set(2, 8, kBlue); c.Set(13, 8, kBlue);
            c.Set(3, 9, kBlue); c.Set(4, 9, kBlue); c.Set(11, 9, kBlue); c.Set(12, 9, kBlue);
            c.HLine(5, 10, 10, kBlue);
            c.Fill(6, 5, 9, 8, kPupil);
            c.Set(6, 5, kPaper); c.Set(9, 5, kPaper);      // белые уголки зрачка
            c.Set(6, 8, kPaper); c.Set(9, 8, kPaper);
            break;
        }
        case TIK_EDIT: {                                // правка: толстый карандаш по диагонали
            c.Fill(11, 0, 13, 1, kEraser);                // ластик
            c.HLine(11, 13, 2, kPencilEdge);
            for (int k = 0; k <= 6; ++k) c.Fill(10 - k, 3 + k, 12 - k, 3 + k, kAmber);
            c.Fill(3, 10, 5, 10, kWood);                  // деревянное остриё
            c.Fill(2, 11, 3, 11, kWood);
            c.Fill(1, 12, 2, 12, kLead);                  // грифель
            break;
        }
        case TIK_COPY: {                                // копирование: два листа
            auto sheet = [&c](int x0, int y0, unsigned paper, unsigned edge) {
                c.Fill(x0, y0, x0 + 7, y0 + 10, paper);
                c.HLine(x0, x0 + 7, y0, edge);
                c.HLine(x0, x0 + 7, y0 + 10, edge);
                c.VLine(x0, y0, y0 + 10, edge);
                c.VLine(x0 + 7, y0, y0 + 10, edge);
                for (int y = y0 + 3; y <= y0 + 8; y += 3) c.HLine(x0 + 2, x0 + 5, y, 0xFF9A9A9A);
            };
            sheet(4, 4, kPaper2, kEdge);
            sheet(1, 1, kPaper, kGray);
            break;
        }
        case TIK_MOVE: {                                // перемещение: лист + стрелка
            c.Fill(1, 1, 7, 10, kPaper);
            c.HLine(1, 7, 1, kEdge); c.HLine(1, 7, 10, kEdge);
            c.VLine(1, 1, 10, kEdge); c.VLine(7, 1, 10, kEdge);
            c.HLine(3, 5, 4, kText); c.HLine(3, 5, 7, kText);
            for (int k = 0; k < 4; ++k) c.VLine(13 - k, 10 - k, 11 + k, kGreen);
            c.Fill(8, 10, 12, 11, kGreen);
            break;
        }
        case TIK_MKDIR: {                               // новый каталог: папка + плюс
            c.Fill(4, 0, 5, 5, kGreen);                   // вертикаль плюса
            c.Fill(1, 2, 8, 3, kGreen);                   // горизонталь плюса
            c.HLine(1, 6, 5, kFEdge);                     // верх язычка
            c.Fill(1, 6, 6, 7, kFDark);                   // язычок
            c.VLine(1, 5, 7, kFEdge);
            c.HLine(1, 6, 7, kFEdge);
            c.HLine(1, 14, 8, kFEdge);                    // верх корпуса
            c.Fill(1, 9, 14, 11, kFBody);                 // корпус
            c.Fill(1, 12, 14, 13, kFDark);                // низ темнее
            c.HLine(1, 14, 14, kFEdge);
            c.VLine(1, 8, 14, kFEdge);
            c.VLine(14, 8, 14, kFEdge);
            break;
        }
        case TIK_DEL:                                   // удаление: красный крест
            for (int i = 0; i <= 9; ++i) {
                c.Set(3 + i, 3 + i, kRed); c.Set(4 + i, 3 + i, kRed);
                c.Set(12 - i, 3 + i, kRed); c.Set(13 - i, 3 + i, kRed);
            }
            break;
        case TIK_PACK: {                                 // упаковать: посылка, лента крест-накрест
            c.Fill(2, 5, 13, 14, kBox);                   // корпус
            c.HLine(2, 13, 5, kBoxEdge); c.HLine(2, 13, 14, kBoxEdge);
            c.VLine(2, 5, 14, kBoxEdge); c.VLine(13, 5, 14, kBoxEdge);
            c.Fill(1, 2, 14, 4, kBoxLid);                 // крышка шире корпуса
            c.HLine(1, 14, 2, kBoxEdge);
            c.VLine(1, 2, 4, kBoxEdge); c.VLine(14, 2, 4, kBoxEdge);
            c.HLine(1, 14, 4, kBoxEdge);
            c.Fill(7, 2, 8, 14, kPaper);                  // лента: вертикаль
            c.Fill(2, 9, 13, 10, kPaper);                 // лента: горизонталь
            break;
        }
        case TIK_UNPACK: {                              // распаковать: открытая коробка + стрелка
            for (int k = 0; k < 3; ++k) c.HLine(7 - k, 8 + k, k, kGreen);   // головка
            c.Fill(7, 3, 8, 6, kGreen);                                        // вал
            c.HLine(1, 4, 6, kBoxLid);                    // отогнутые клапаны
            c.HLine(0, 3, 7, kBoxEdge);
            c.HLine(11, 14, 6, kBoxLid);
            c.HLine(12, 15, 7, kBoxEdge);
            c.Fill(2, 8, 13, 14, kBox);                   // корпус
            c.HLine(2, 13, 8, kBoxEdge); c.HLine(2, 13, 14, kBoxEdge);
            c.VLine(2, 8, 14, kBoxEdge); c.VLine(13, 8, 14, kBoxEdge);
            c.Fill(7, 8, 8, 14, kPaper);                  // лента
            break;
        }
        case TIK_RENM: {                                // мультипереименование: лист + карандаш
            c.Fill(1, 1, 6, 9, kPaper);
            c.HLine(1, 6, 1, kEdge); c.HLine(1, 6, 9, kEdge);
            c.VLine(1, 1, 9, kEdge); c.VLine(6, 1, 9, kEdge);
            c.HLine(3, 4, 4, kText); c.HLine(3, 4, 7, kText);
            for (int k = 0; k <= 6; ++k) c.Fill(10 - k, 5 + k, 12 - k, 5 + k, kAmber);
            c.Fill(3, 12, 5, 12, kWood);
            c.Fill(2, 13, 3, 13, kLead);
            break;
        }
        case TIK_FIND: {                                // поиск: бинокль
            auto lens = [&c](int cx, int cy) {
                c.HLine(cx - 2, cx + 2, cy - 3, kBinoc);
                for (int dy = -2; dy <= 2; ++dy) c.HLine(cx - 3, cx + 3, cy + dy, kBinoc);
                c.HLine(cx - 2, cx + 2, cy + 3, kBinoc);
            };
            c.HLine(2, 13, 4, kBinoc);                    // верхняя планка
            c.VLine(4, 4, 7, kBinoc); c.VLine(12, 4, 7, kBinoc);
            c.HLine(7, 9, 10, kBinoc);                    // перемычка
            lens(4, 10); lens(12, 10);
            break;
        }
        case TIK_SYNC:                                  // синхронизация: встречные стрелки
            for (int k = 0; k < 3; ++k) c.VLine(11 - k, 4 - k, 5 + k, kSync);
            c.Fill(3, 4, 10, 5, kSync);
            for (int k = 0; k < 3; ++k) c.VLine(4 + k, 10 - k, 11 + k, kSync);
            c.Fill(5, 10, 12, 11, kSync);
            break;
        case TIK_REFRESH: {                             // обновить: круговая стрелка ↻
            for (int i = 0; i <= 50; ++i) {
                const double rad = (75.0 - i * 300.0 / 50.0) * 3.14159265358979 / 180.0;
                const int x5 = 8 + (int)(5.0 * cos(rad) + 0.5);
                const int y5 = 8 - (int)(5.0 * sin(rad) + 0.5);
                const int x4 = 8 + (int)(4.0 * cos(rad) + 0.5);
                const int y4 = 8 - (int)(4.0 * sin(rad) + 0.5);
                c.Set(x5, y5, kRefresh);
                c.Set(x4, y4, kRefresh);
            }
            c.HLine(9, 11, 2, kRefresh);                 // наконечник у начала дуги
            c.HLine(9, 12, 3, kRefresh);
            c.HLine(9, 12, 4, kRefresh);
            c.HLine(9, 11, 5, kRefresh);
            break;
        }
        case TIK_PROPS: {                               // свойства: шестерёнка с зубцами
            c.HLine(6, 9, 5, kGear);
            c.HLine(6, 9, 11, kGear);
            c.VLine(5, 6, 9, kGear); c.VLine(10, 6, 9, kGear);
            c.Fill(7, 2, 8, 4, kGear);                                  // зуб N
            c.Fill(7, 12, 8, 14, kGear);                                 // зуб S
            c.Fill(2, 7, 4, 8, kGear);                                   // зуб W
            c.Fill(11, 7, 13, 8, kGear);                                 // зуб E
            c.Fill(11, 3, 12, 4, kGear);                                 // зуб NE
            c.Fill(11, 11, 12, 12, kGear);                                // зуб SE
            c.Fill(3, 11, 4, 12, kGear);                                 // зуб SW
            c.Fill(3, 3, 4, 4, kGear);                                   // зуб NW
            break;
        }
        default:
            break;
    }
}

// Значок тулбара по виду: 16×16, при большем размере — масштабирование.
HICON MakeToolIcon(int kind, int size) {
    PixelCanvas small(16);
    ToolIconPixels(kind, small);
    if (size <= 16) return small.ToIcon();
    PixelCanvas big(size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            big.Set(x, y, small.px[(size_t)(y * 16 / size) * 16 + (x * 16 / size)]);
    return big.ToIcon();
}

// Добавляет значок в наш список и возвращает его индекс.
int AddIcon(HICON icon) {
    if (!g_images || !icon) return 0;
    const int index = ImageList_AddIcon(g_images, icon);
    DestroyIcon(icon);
    return index >= 0 ? index : 0;
}

// Системный значок (HICON) по пути; вызывающий отвечает за DestroyIcon.
HICON ShellIcon(const std::wstring& path, DWORD attrs, bool useFileAttrs, UINT extraFlags = 0) {
    SHFILEINFOW fi{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON | extraFlags;
    if (useFileAttrs) flags |= SHGFI_USEFILEATTRIBUTES;
    if (!SHGetFileInfoW(path.c_str(), attrs, &fi, sizeof(fi), flags)) return nullptr;
    return fi.hIcon;
}

// Значок по пути: SHGFI_ICON даёт «живую» иконку, включая спецпапки и exe.
int IconFromPath(const std::wstring& path, DWORD attrs, bool useFileAttrs, UINT extraFlags = 0) {
    HICON icon = ShellIcon(path, attrs, useFileAttrs, extraFlags);
    if (!icon) return -1;
    return AddIcon(icon);
}

// Индекс нашей иконки типа в общем списке. Создаётся лениво, по одному
// экземпляру на тип и признак скрытости; утечек нет — список владеет значками.
int TypeIconIndex(int kind, bool hidden) {
    InitShellIcons();
    const int slot = kind * 2 + (hidden ? 1 : 0);
    if (slot < 0 || slot >= kTkCount * 2) return g_iconFile;
    if (g_typeIcons[slot] < 0) {
        HICON icon = MakeTypeIcon(kind, g_iconSize, hidden);
        g_typeIcons[slot] = icon ? AddIcon(icon) : 0;
    }
    return g_typeIcons[slot];
}

void InitShellIcons() {
    if (g_images) return;

    // Размер малого значка берём из системы: он уже учитывает DPI.
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    g_iconSize = size;
    g_images = ImageList_Create(size, size, ILC_COLOR32, 64, 256);

    for (int& i : g_typeIcons) i = -1;

    g_iconUp = AddIcon(MakeUpArrowIcon(size));

    // Панельные значки рисуем сами — как в Total Commander (макет):
    // жёлтые папки, типы файлов по расширению, у скрытых красная полоса.
    g_iconFolder = AddIcon(MakeFolderIcon(size, false));
    g_iconFolderHidden = AddIcon(MakeFolderIcon(size, true));
    g_iconFile = TypeIconIndex(kTkFile, false);
    g_iconFileHidden = TypeIconIndex(kTkFile, true);
}

HIMAGELIST ShellSmallImageList() {
    InitShellIcons();
    return g_images;
}

int IconForEntry(const Entry& e, const std::wstring& dir) {
    InitShellIcons();
    (void)dir;   // значки типов не зависят от пути

    // «..» — отдельная стрелка, как в TC.
    if (e.isUp) return g_iconUp >= 0 ? g_iconUp : g_iconFolder;

    // Каталоги — единая жёлтая папка TC, у скрытых красная полоса.
    if (e.isDir) return e.hidden() ? g_iconFolderHidden : g_iconFolder;

    const std::wstring ext = Lower(e.ext());

    // Наши иконки типов — по спецификации макета ui-target.html.
    int kind = -1;
    if (ext == L".txt" || ext == L".log" || ext == L".ini") kind = kTkText;
    else if (ext == L".cpp" || ext == L".h" || ext == L".c" || ext == L".py") kind = kTkCode;
    else if (ext == L".exe" || ext == L".dll") kind = kTkApp;
    else if (ext == L".zip" || ext == L".rar" || ext == L".7z") kind = kTkArchive;
    else if (ext == L".jpg" || ext == L".png" || ext == L".bmp") kind = kTkImage;
    else if (ext == L".lnk") kind = kTkLink;
    if (kind >= 0) return TypeIconIndex(kind, e.hidden());

    // Скрытый файл неизвестного типа — белый лист с красной полосой.
    if (e.hidden()) return g_iconFileHidden;

    // Прочие расширения — системный значок (pdf, docx, mp3…), как в Проводнике.
    auto it = g_iconCache.find(L"e:" + ext);
    if (it != g_iconCache.end()) return it->second;

    const int byExt = IconFromPath(L"file" + ext, FILE_ATTRIBUTE_NORMAL, true);
    int index = byExt >= 0 ? byExt : g_iconFile;
    if (g_iconCache.size() >= kIconCacheLimit) g_iconCache.clear();
    g_iconCache[L"e:" + ext] = index;
    return index;
}

UINT WindowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = nullptr;
    static bool probed = false;
    if (!probed) {
        probed = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (Fn)GetProcAddress(user32, "GetDpiForWindow");
    }
    UINT dpi = fn ? fn(hwnd) : 0;
    if (!dpi) {
        HDC dc = GetDC(nullptr);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    return dpi ? dpi : 96;
}

// Быстрый natural-compare: без вызовов оболочки и без locale-функций на ASCII.
// StrCmpLogicalW на 100 000 имён давал ~430 мс; этот вариант — единицы-десятки мс.
namespace {

inline bool IsDigitFast(wchar_t c) { return c >= L'0' && c <= L'9'; }

inline wchar_t LowerFast(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c + (L'a' - L'A'));
    if (c < 128) return c;
    return (wchar_t)towlower(c);
}

}  // namespace

int CompareNatural(const std::wstring& a, const std::wstring& b) {
    const size_t na = a.size(), nb = b.size();
    const wchar_t* pa = a.data();
    const wchar_t* pb = b.data();
    size_t i = 0, j = 0;

    while (i < na && j < nb) {
        wchar_t ca = pa[i], cb = pb[j];
        if (ca == cb) {          // самый частый случай: префикс совпадает
            ++i;
            ++j;
            continue;
        }

        if (IsDigitFast(ca) && IsDigitFast(cb)) {
            size_t ia = i, jb = j;
            while (ia < na && pa[ia] == L'0') ++ia;
            while (jb < nb && pb[jb] == L'0') ++jb;
            size_t ea = ia, eb = jb;
            while (ea < na && IsDigitFast(pa[ea])) ++ea;
            while (eb < nb && IsDigitFast(pb[eb])) ++eb;
            const size_t la = ea - ia, lb = eb - jb;
            if (la != lb) return la < lb ? -1 : 1;
            for (size_t k = 0; k < la; ++k) {
                if (pa[ia + k] != pb[jb + k]) return pa[ia + k] < pb[jb + k] ? -1 : 1;
            }
            i = ea;
            j = eb;
            continue;
        }

        wchar_t la = LowerFast(ca), lb = LowerFast(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++i;
        ++j;
    }

    if (i < na) return 1;
    if (j < nb) return -1;
    if (a == b) return 0;
    return CompareStringOrdinal(pa, (int)na, pb, (int)nb, TRUE) - CSTR_EQUAL;
}
// ---------- диагностика ----------

void LogDebug(const std::wstring& text) {
    // Диагностика включается только по переменной окружения POSC2_DEBUG=1,
    // иначе в %TEMP% ничего не пишется (как в TC: лишних файлов не создаём).
    static const bool enabled = [] {
        wchar_t v[8]{};
        const DWORD n = GetEnvironmentVariableW(L"POSC2_DEBUG", v, 8);
        return n > 0 && v[0] != L'0';
    }();
    if (!enabled) return;

    wchar_t tmp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring path = std::wstring(tmp) + L"posc2-debug.log";

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[64]{};
    swprintf(ts, 64, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = std::wstring(ts) + text + L"\r\n";

    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string narrow((size_t)need, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), narrow.data(), need, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(f, narrow.data(), (DWORD)narrow.size(), &written, nullptr);
    }
    CloseHandle(f);
}
