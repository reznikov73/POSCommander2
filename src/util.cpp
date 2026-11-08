// Реализация утилит: пути, форматирование, перечисление каталогов, иконки.
#include "util.h"

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
    std::wstring out = FmtSize(freeBytes.QuadPart) + L" свободно из " + FmtSize(total.QuadPart);
    wchar_t label[MAX_PATH + 1]{};
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && label[0]) {
        out += L" · ";
        out += label;
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

// Красная полоса по левому краю иконки — признак скрытого/системного, как в TC.
void PaintHiddenMark(PixelCanvas& c) {
    const int s = c.size;
    const int top = s * 4 / 16;
    const int bottom = s - s * 3 / 16;
    const DWORD dark = 0xFFC00000;
    const DWORD light = 0xFFE87777;

    c.Fill(1, top, 3, bottom, dark);
    c.VLine(2, top + 1, bottom - 1, light);
}

// Классическая жёлтая папка с язычком — как в Total Commander.
HICON MakeFolderIcon(int size, bool hidden = false) {
    PixelCanvas c(size);
    const int s = size;
    const int bodyTop = s * 5 / 16;
    const int tabTop = s * 3 / 16;
    const int tabRight = s * 8 / 16;

    const DWORD edge = 0xFFB8860B;      // тёмно-янтарный контур
    const DWORD body = 0xFFFFE9A8;      // светлая середина
    const DWORD bodyDark = 0xFFFFD061;  // низ и тень
    const DWORD tab = 0xFFFFD061;

    // язычок
    c.Fill(1, tabTop, tabRight, bodyTop - 1, tab);
    c.HLine(1, tabRight, tabTop - 1, edge);
    c.HLine(1, tabRight, bodyTop - 1, edge);
    c.VLine(1, tabTop, bodyTop - 1, edge);

    // корпус
    c.Fill(1, bodyTop, s - 2, s - 2, body);
    c.Fill(1, s - 4, s - 2, s - 2, bodyDark);

    // контур корпуса
    c.HLine(1, s - 2, bodyTop, edge);
    c.HLine(1, s - 2, s - 2, edge);
    c.VLine(1, bodyTop, s - 2, edge);
    c.VLine(s - 2, bodyTop, s - 2, edge);

    if (hidden) PaintHiddenMark(c);
    return c.ToIcon();
}

// Белая страница с загнутым уголком — для файлов без своего значка.
HICON MakeFileIcon(int size, bool hidden = false) {
    PixelCanvas c(size);
    const int s = size;
    const DWORD edge = 0xFF8A8A8A;
    const DWORD paper = 0xFFFFFFFF;
    const DWORD fold = 0xFFD8D8D8;
    const DWORD line = 0xFF9A9A9A;

    c.Fill(3, 1, s - 4, s - 2, paper);
    c.HLine(3, s - 4, 1, edge);
    c.VLine(3, 1, s - 2, edge);
    c.HLine(3, s - 4, s - 2, edge);
    c.VLine(s - 4, 1, s - 2, edge);

    // загнутый уголок
    c.Fill(s - 7, 1, s - 5, 4, fold);
    c.HLine(s - 7, s - 5, 5, edge);

    // строки текста
    for (int y = 7; y <= s - 4; y += 3) c.HLine(5, s - 6, y, line);

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
        const DWORD solid = 0xFF000000;     // чёрная, непрозрачная

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

// Копия системного значка с красной меткой «скрытый». nullptr, если формат не подходит.
HICON MarkHiddenCopy(HICON src, int size) {
    if (!src) return nullptr;
    ICONINFO ii{};
    if (!GetIconInfo(src, &ii)) return nullptr;

    HICON result = nullptr;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth == size &&
        bm.bmHeight == size && bm.bmBitsPixel == 32) {
        PixelCanvas c(size);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        const int lines = GetDIBits(dc, ii.hbmColor, 0, size, c.px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);

        bool hasAlpha = false;
        for (DWORD p : c.px) {
            if (p >> 24) {
                hasAlpha = true;
                break;
            }
        }
        if (lines == size && hasAlpha) {
            // Как в Проводнике: скрытый элемент — полупрозрачный «призрачный» значок.
            // Цвета не трогаем (альфа непредумноженная), меняем только прозрачность.
            for (DWORD& p : c.px) {
                const DWORD a = (p >> 24) * 50 / 100;
                p = (a << 24) | (p & 0x00FFFFFF);
            }
            result = c.ToIcon();
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

void InitShellIcons() {
    if (g_images) return;

    // Размер малого значка берём из системы: он уже учитывает DPI.
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    g_images = ImageList_Create(size, size, ILC_COLOR32, 64, 256);

    g_iconUp = AddIcon(MakeUpArrowIcon(size));

    // Папка и файл по умолчанию — системные значки, как в Проводнике.
    // Нарисованные вручную остаются запасным вариантом.
    HICON folder = ShellIcon(L"folder", FILE_ATTRIBUTE_DIRECTORY, true);
    HICON file = ShellIcon(L"file", FILE_ATTRIBUTE_NORMAL, true);

    HICON folderHidden = MarkHiddenCopy(folder, size);
    HICON fileHidden = MarkHiddenCopy(file, size);

    g_iconFolder = folder ? AddIcon(folder) : AddIcon(MakeFolderIcon(size));
    g_iconFile = file ? AddIcon(file) : AddIcon(MakeFileIcon(size));
    g_iconFolderHidden = folderHidden ? AddIcon(folderHidden) : AddIcon(MakeFolderIcon(size, true));
    g_iconFileHidden = fileHidden ? AddIcon(fileHidden) : AddIcon(MakeFileIcon(size, true));
}

HIMAGELIST ShellSmallImageList() {
    InitShellIcons();
    return g_images;
}

// Кэш с ограничением: на огромных каталогах не растём бесконечно.
int CachedIcon(const std::wstring& key, const std::wstring& path, DWORD attrs, bool useFileAttrs, UINT extra = 0) {
    auto it = g_iconCache.find(key);
    if (it != g_iconCache.end()) return it->second;

    int index = IconFromPath(path, attrs, useFileAttrs, extra);
    if (index < 0) return -1;

    if (g_iconCache.size() >= kIconCacheLimit) g_iconCache.clear();
    g_iconCache[key] = index;
    return index;
}

int IconForEntry(const Entry& e, const std::wstring& dir) {
    InitShellIcons();

    // «..» — отдельная стрелка, как в TC.
    if (e.isUp) return g_iconUp >= 0 ? g_iconUp : g_iconFolder;

    const std::wstring full = dir.empty() ? e.name : JoinPath(dir, e.name);

    if (e.isDir) {
        // У узнаваемых системных папок берём значок оболочки, у остальных — свою жёлтую папку.
        static const wchar_t* kSpecial[] = {
            L"desktop", L"рабочий стол", L"documents", L"документы", L"downloads", L"загрузки",
            L"pictures", L"изображения", L"music", L"музыка", L"videos", L"видео",
            L"onedrive", L"3d objects", L"объекты 3d", L"favorites", L"избранное",
        };
        const std::wstring lowerName = Lower(e.name);
        bool special = false;
        for (auto n : kSpecial) {
            if (lowerName == n) {
                special = true;
                break;
            }
        }
        if (special) {
            const int byPath = CachedIcon(L"p:" + full, full, FILE_ATTRIBUTE_DIRECTORY, false);
            if (byPath >= 0) return byPath;
        }
        return e.hidden() ? g_iconFolderHidden : g_iconFolder;
    }

    const std::wstring ext = Lower(e.ext());

    // Для exe/lnk/ico/msi значок берём из самого файла, а не по расширению.
    if (ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".msi" || ext == L".com" ||
        ext == L".bat" || ext == L".cmd") {
        const UINT extra = (ext == L".lnk") ? SHGFI_LINKOVERLAY : 0;
        const int byPath = CachedIcon(L"f:" + full, full, FILE_ATTRIBUTE_NORMAL, false, extra);
        if (byPath >= 0) return byPath;
    }

    auto it = g_iconCache.find(L"e:" + ext);
    if (it != g_iconCache.end()) return it->second;

    const int byExt = IconFromPath(L"file" + ext, FILE_ATTRIBUTE_NORMAL, true);
    int index = byExt >= 0 ? byExt : g_iconFile;
    if (e.hidden() && ext.empty()) index = g_iconFileHidden;
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
