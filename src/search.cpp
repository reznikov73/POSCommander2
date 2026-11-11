#include "search.h"

#include "archive.h"
#include "dialogs.h"

#include "ops.h"
#include "settings.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const wchar_t* kSearchClass = L"POSSearch2";

const int kMaskId = 10;
const int kDirId = 11;
const int kTextId = 12;
const int kCaseId = 13;
const int kSubdirsId = 14;
const int kHiddenId = 15;
const int kMinSizeId = 16;      // фильтр: размер от, Кб
const int kMaxSizeId = 17;      // размер до, Кб
const int kDateFromId = 18;     // дата с, ДД.ММ.ГГГГ
const int kDateToId = 19;       // дата по
const int kTemplatesId = 24;    // кнопка «Шаблоны»
const int kArchivesId = 26;     // искать в архивах
const int kStopId = 20;
const int kBrowseId = 22;
const int kOpenDirId = 23;
const int kListId = 30;
const int kStatusId = 31;
const int kHintId = 32;
const int kDriveIdBase = 200;      // 200..225 — кнопки корней
const int kMenuOpenDir = 1;
const int kMenuCopyPath = 2;

const int kTimerId = 1;
const size_t kMaxResults = 50000;

struct Result {
    std::wstring path;
    std::wstring name;
    std::wstring dir;
    ULONGLONG size = 0;
    FILETIME mtime{};
};

struct SearchState {
    HWND hwnd = nullptr;
    HWND mask = nullptr;
    HWND dir = nullptr;
    HWND text = nullptr;
    HWND caseCheck = nullptr;
    HWND subdirs = nullptr;
    HWND hiddenCheck = nullptr;
    HWND archivesCheck = nullptr;
    bool searchArchives = false;
    HWND list = nullptr;
    HWND status = nullptr;
    HWND hint = nullptr;
    HWND startBtn = nullptr;
    HWND stopBtn = nullptr;
    HWND browseBtn = nullptr;
    HWND openDirBtn = nullptr;
    HWND templatesBtn = nullptr;
    HWND minSize = nullptr;
    HWND maxSize = nullptr;
    HWND dateFrom = nullptr;
    HWND dateTo = nullptr;
    HWND labelFilter = nullptr;
    HWND labelSize = nullptr;
    HWND labelKb = nullptr;
    HWND labelDate = nullptr;
    HWND dash1 = nullptr;
    HWND dash2 = nullptr;

    // Разобранные фильтры: их читает поток поиска.
    bool useMinSize = false;
    bool useMaxSize = false;
    ULONGLONG minSizeBytes = 0;
    ULONGLONG maxSizeBytes = 0;
    bool useDateFrom = false;
    bool useDateTo = false;
    FILETIME dateFromFt{};
    FILETIME dateToFt{};
    HWND labelMask = nullptr;
    HWND labelDir = nullptr;
    HWND labelText = nullptr;
    HWND labelRoots = nullptr;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;

    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int> scanned{0};

    std::mutex mutex;
    std::vector<Result> pending;   // найдено, но ещё не показано
    std::vector<Result> rows;      // всё, что уже в списке
    int sortCol = -1;
    bool sortAsc = true;

    std::wstring chosen;
};

void FillResults(SearchState* st);
void UpdateStatus(SearchState* st);
void LayoutSearch(SearchState* st);

// ---------- поиск ----------

bool FileContainsText(const std::wstring& path, const std::wstring& needle, bool caseSensitive) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG limit = 8LL * 1024 * 1024;      // больше 8 МБ не читаем
    DWORD toRead = (DWORD)(size.QuadPart > limit ? limit : size.QuadPart);
    std::string data(toRead, '\0');
    DWORD read = 0;
    BOOL ok = toRead == 0 ? TRUE : ReadFile(f, data.data(), toRead, &read, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    data.resize(read);
    if (data.empty()) return false;

    std::wstring wide;
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        wide.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
    } else {
        UINT cp = CP_ACP;
        size_t offset = 0;
        if (data.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            cp = CP_UTF8;
            offset = 3;
        } else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0) > 0) {
            cp = CP_UTF8;
        }
        int need = MultiByteToWideChar(cp, 0, data.data() + offset, (int)(data.size() - offset), nullptr, 0);
        if (need <= 0) return false;
        wide.resize((size_t)need);
        MultiByteToWideChar(cp, 0, data.data() + offset, (int)(data.size() - offset), wide.data(), need);
    }

    if (!caseSensitive) {
        wide = Lower(wide);
        return wide.find(needle) != std::wstring::npos;
    }
    return wide.find(needle) != std::wstring::npos;
}

void WalkDirectory(SearchState* st, const std::wstring& dir, const std::vector<std::wstring>& masks,
                   const std::wstring& needle, bool caseSensitive, bool recurse, bool includeHidden,
                   int depth) {
    if (st->stop.load() || depth > 32) return;

    std::wstring pattern = JoinPath(dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    do {
        if (st->stop.load()) break;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;

        const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring name = data.cFileName;
        const std::wstring full = JoinPath(dir, name);

        if (isDir) {
            if (recurse && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) subdirs.push_back(full);
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!includeHidden && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;

        // Фильтры по размеру и дате: до чтения содержимого, чтобы не тратить диск.
        const ULONGLONG fsize = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        if (st->useMinSize && fsize < st->minSizeBytes) continue;
        if (st->useMaxSize && fsize > st->maxSizeBytes) continue;
        if (st->useDateFrom && CompareFileTime(&data.ftLastWriteTime, &st->dateFromFt) < 0) continue;
        if (st->useDateTo && CompareFileTime(&data.ftLastWriteTime, &st->dateToFt) > 0) continue;

        // Архив: ищем по именам внутри, не читая содержимое.
        if (st->searchArchives && IsArchivePath(full)) {
            std::vector<ArchiveEntry> entries;
            if (ListArchive(full, entries)) {
                int added = 0;
                for (const auto& entry : entries) {
                    if (entry.isDir) continue;
                    if (added >= 5000) break;
                    std::wstring inner = entry.name;   // 7z печатает разделитель ОС, но нормализуем на всякий случай
                    for (wchar_t& ch : inner)
                        if (ch == L'/') ch = L'\\';
                    const size_t slash = inner.find_last_of(L"\\/");
                    const std::wstring shortName = slash == std::wstring::npos ? inner : inner.substr(slash + 1);
                    bool match = false;
                    for (auto& m : masks) {
                        if (MatchMask(Lower(shortName), m)) {
                            match = true;
                            break;
                        }
                    }
                    if (!match) continue;

                    std::lock_guard<std::mutex> lock(st->mutex);
                    if (st->rows.size() + st->pending.size() < kMaxResults) {
                        Result r;
                        r.path = full + L"\\" + inner;
                        r.name = inner;
                        r.dir = full;
                        r.size = 0;
                        if (!entry.size.empty()) {           // 7z отдаёт размер строкой
                            wchar_t* end = nullptr;
                            const unsigned long long v = wcstoull(entry.size.c_str(), &end, 10);
                            if (end != entry.size.c_str()) r.size = v;
                        }
                        st->pending.push_back(std::move(r));
                        ++added;
                    }
                }
            }
        }

        bool nameMatches = false;
        for (auto& m : masks) {
            if (MatchMask(Lower(name), m)) {
                nameMatches = true;
                break;
            }
        }
        st->scanned.fetch_add(1);

        if (!nameMatches) continue;
        if (!needle.empty() && !FileContainsText(full, needle, caseSensitive)) continue;

        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->rows.size() + st->pending.size() < kMaxResults) {
                Result r;
                r.path = full;
                r.name = name;
                r.dir = dir;
                r.size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
                r.mtime = data.ftLastWriteTime;
                st->pending.push_back(std::move(r));
            }
        }
    } while (FindNextFileW(find, &data));

    FindClose(find);

    for (auto& sub : subdirs) {
        if (st->stop.load()) break;
        WalkDirectory(st, sub, masks, needle, caseSensitive, recurse, includeHidden, depth + 1);
    }
}

// «512» → 512 Кб. Пустая строка — фильтра нет.
bool ParseSizeKb(const std::wstring& text, ULONGLONG& bytes) {
    const std::wstring t = Trim(text);
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long v = wcstoull(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    bytes = v * 1024ull;
    return true;
}

// «31.12.2026» → FILETIME (локальное время, конец дня для «по»).
bool ParseDate(const std::wstring& text, FILETIME& ft, bool endOfDay) {
    int d = 0, m = 0, y = 0;
    if (swscanf(text.c_str(), L"%d.%d.%d", &d, &m, &y) != 3) return false;
    if (d < 1 || d > 31 || m < 1 || m > 12 || y < 1900) return false;

    SYSTEMTIME st{};
    st.wDay = (WORD)d;
    st.wMonth = (WORD)m;
    st.wYear = (WORD)y;
    st.wHour = endOfDay ? 23 : 0;
    st.wMinute = endOfDay ? 59 : 0;
    st.wSecond = endOfDay ? 59 : 0;

    FILETIME local{};
    if (!SystemTimeToFileTime(&st, &local)) return false;
    return LocalFileTimeToFileTime(&local, &ft) != FALSE;
}

void ReadFilters(SearchState* st) {
    wchar_t buf[64]{};
    GetWindowTextW(st->minSize, buf, 64);
    st->useMinSize = ParseSizeKb(buf, st->minSizeBytes);
    GetWindowTextW(st->maxSize, buf, 64);
    st->useMaxSize = ParseSizeKb(buf, st->maxSizeBytes);
    GetWindowTextW(st->dateFrom, buf, 64);
    st->useDateFrom = ParseDate(buf, st->dateFromFt, false);
    GetWindowTextW(st->dateTo, buf, 64);
    st->useDateTo = ParseDate(buf, st->dateToFt, true);
}

std::wstring TemplateValue(SearchState* st) {
    wchar_t mask[512]{}, dir[MAX_PATH * 4]{}, text[512]{};
    wchar_t minS[64]{}, maxS[64]{}, fromS[64]{}, toS[64]{};
    GetWindowTextW(st->mask, mask, 512);
    GetWindowTextW(st->dir, dir, MAX_PATH * 4);
    GetWindowTextW(st->text, text, 512);
    GetWindowTextW(st->minSize, minS, 64);
    GetWindowTextW(st->maxSize, maxS, 64);
    GetWindowTextW(st->dateFrom, fromS, 64);
    GetWindowTextW(st->dateTo, toS, 64);

    wchar_t flags[8]{};
    swprintf(flags, 8, L"%d%d%d%d",
             SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->hiddenCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->archivesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);

    std::wstring out = mask;
    out += L"|"; out += dir;
    out += L"|"; out += flags;
    out += L"|"; out += text;
    out += L"|"; out += minS;
    out += L"|"; out += maxS;
    out += L"|"; out += fromS;
    out += L"|"; out += toS;
    return out;
}

std::vector<std::wstring> SplitTemplateValue(const std::wstring& value) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : value) {
        if (c == L'|') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

void ApplyTemplate(SearchState* st, const std::wstring& value) {
    const auto p = SplitTemplateValue(value);
    auto at = [&p](size_t i) { return i < p.size() ? p[i] : std::wstring(); };

    SetWindowTextW(st->mask, at(0).c_str());
    SetWindowTextW(st->dir, at(1).c_str());
    const std::wstring flags = at(2);
    SendMessageW(st->subdirs, BM_SETCHECK, flags.size() > 0 && flags[0] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->caseCheck, BM_SETCHECK, flags.size() > 1 && flags[1] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->hiddenCheck, BM_SETCHECK, flags.size() > 2 && flags[2] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->archivesCheck, BM_SETCHECK, flags.size() > 3 && flags[3] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(st->text, at(3).c_str());
    SetWindowTextW(st->minSize, at(4).c_str());
    SetWindowTextW(st->maxSize, at(5).c_str());
    SetWindowTextW(st->dateFrom, at(6).c_str());
    SetWindowTextW(st->dateTo, at(7).c_str());
}

std::vector<std::pair<std::wstring, std::wstring>> LoadTemplates() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const std::wstring ini = SettingsFilePath();
    std::vector<wchar_t> buf(64 * 1024);
    const DWORD n = GetPrivateProfileSectionW(L"SearchTemplates", buf.data(), (DWORD)buf.size(), ini.c_str());
    if (!n) return out;

    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        const std::wstring line = p;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

void SaveTemplateAs(SearchState* st) {
    std::wstring name;
    if (!InputBox(st->hwnd, L"Шаблон поиска", L"Название шаблона:", name, false)) return;
    name = Trim(name);
    if (name.empty()) return;
    const std::wstring ini = SettingsFilePath();
    const std::wstring value = TemplateValue(st);
    WritePrivateProfileStringW(L"SearchTemplates", name.c_str(), value.c_str(), ini.c_str());
}

void DeleteTemplate(SearchState* st) {
    auto items = LoadTemplates();
    if (items.empty()) {
        MessageBoxW(st->hwnd, L"Сохранённых шаблонов нет.", L"Шаблоны", MB_ICONINFORMATION | MB_OK);
        return;
    }
    std::wstring name;
    if (!InputBox(st->hwnd, L"Удалить шаблон", L"Название шаблона:", name, false)) return;
    name = Trim(name);
    if (name.empty()) return;
    const std::wstring ini = SettingsFilePath();
    WritePrivateProfileStringW(L"SearchTemplates", name.c_str(), nullptr, ini.c_str());
}

void ShowTemplatesMenu(SearchState* st) {
    const auto items = LoadTemplates();

    HMENU menu = CreatePopupMenu();
    int cmd = 1;
    for (const auto& kv : items) AppendMenuW(menu, MF_STRING, cmd++, kv.first.c_str());
    if (!items.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 900, L"Сохранить текущий…");
    if (!items.empty()) AppendMenuW(menu, MF_STRING, 901, L"Удалить шаблон…");

    POINT pt{};
    GetCursorPos(&pt);
    const int chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, st->hwnd, nullptr);
    DestroyMenu(menu);

    if (chosen >= 1 && chosen < 900 && chosen - 1 < (int)items.size()) {
        ApplyTemplate(st, items[chosen - 1].second);
    } else if (chosen == 900) {
        SaveTemplateAs(st);
    } else if (chosen == 901) {
        DeleteTemplate(st);
    }
}

void SavePrefs(SearchState* st) {
    const std::wstring ini = SettingsFilePath();
    if (ini.empty()) return;

    wchar_t buf[512]{};
    GetWindowTextW(st->mask, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Mask", buf, ini.c_str());
    GetWindowTextW(st->dir, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Dir", buf, ini.c_str());
    GetWindowTextW(st->text, buf, 512);
    WritePrivateProfileStringW(L"Search", L"Text", buf, ini.c_str());

    wchar_t flags[8]{};
    swprintf(flags, 8, L"%d%d%d%d",
             SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->hiddenCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0,
             SendMessageW(st->archivesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Search", L"Flags", flags, ini.c_str());

    auto put = [&ini](const wchar_t* key, HWND edit) {
        wchar_t value[64]{};
        GetWindowTextW(edit, value, 64);
        WritePrivateProfileStringW(L"Search", key, value, ini.c_str());
    };
    put(L"MinSize", st->minSize);
    put(L"MaxSize", st->maxSize);
    put(L"DateFrom", st->dateFrom);
    put(L"DateTo", st->dateTo);
}

void ReadPrefs(SearchState* st, const std::wstring& startDir) {
    const std::wstring ini = SettingsFilePath();
    wchar_t buf[512]{};

    GetPrivateProfileStringW(L"Search", L"Mask", L"*", buf, 512, ini.c_str());
    SetWindowTextW(st->mask, buf[0] ? buf : L"*");

    GetPrivateProfileStringW(L"Search", L"Dir", L"", buf, 512, ini.c_str());
    SetWindowTextW(st->dir, buf[0] ? buf : startDir.c_str());

    GetPrivateProfileStringW(L"Search", L"Text", L"", buf, 512, ini.c_str());
    SetWindowTextW(st->text, buf);

    auto get = [&ini](const wchar_t* key, HWND edit) {
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"Search", key, L"", value, 64, ini.c_str());
        SetWindowTextW(edit, value);
    };
    get(L"MinSize", st->minSize);
    get(L"MaxSize", st->maxSize);
    get(L"DateFrom", st->dateFrom);
    get(L"DateTo", st->dateTo);

    GetPrivateProfileStringW(L"Search", L"Flags", L"100", buf, 8, ini.c_str());
    const std::wstring flags = buf;
    SendMessageW(st->subdirs, BM_SETCHECK, flags.size() > 0 && flags[0] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->caseCheck, BM_SETCHECK, flags.size() > 1 && flags[1] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->hiddenCheck, BM_SETCHECK, flags.size() > 2 && flags[2] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->archivesCheck, BM_SETCHECK, flags.size() > 3 && flags[3] == L'1' ? BST_CHECKED : BST_UNCHECKED, 0);
}

void StartWorker(SearchState* st) {
    if (st->running.load()) return;

    wchar_t maskBuf[512]{};
    GetWindowTextW(st->mask, maskBuf, 512);
    std::wstring maskText = Trim(maskBuf);
    if (maskText.empty()) maskText = L"*";

    wchar_t dirBuf[MAX_PATH * 4]{};
    GetWindowTextW(st->dir, dirBuf, MAX_PATH * 4);
    std::wstring startDir = Trim(dirBuf);
    if (startDir.empty()) {
        MessageBoxW(st->hwnd, L"Укажите, где искать: выберите корень или папку.", L"Поиск файлов",
                    MB_ICONINFORMATION | MB_OK);
        SetFocus(st->dir);
        return;
    }
    if (GetFileAttributesW(startDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(st->hwnd, (L"Такой папки нет:\n" + startDir).c_str(), L"Поиск файлов",
                    MB_ICONWARNING | MB_OK);
        SetFocus(st->dir);
        return;
    }

    wchar_t textBuf[512]{};
    GetWindowTextW(st->text, textBuf, 512);
    std::wstring needle = Trim(textBuf);
    const bool caseSensitive = SendMessageW(st->caseCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool recurse = SendMessageW(st->subdirs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool hidden = SendMessageW(st->hiddenCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    st->searchArchives = SendMessageW(st->archivesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!caseSensitive && !needle.empty()) needle = Lower(needle);

    auto masks = SplitMasks(maskText);
    if (masks.empty()) masks.push_back(L"*");

    ReadFilters(st);
    SavePrefs(st);

    {
        std::lock_guard<std::mutex> lock(st->mutex);
        st->pending.clear();
        st->rows.clear();
    }
    ListView_DeleteAllItems(st->list);
    st->sortCol = -1;
    st->sortAsc = true;

    st->stop.store(false);
    st->scanned.store(0);
    st->running.store(true);
    EnableWindow(st->startBtn, FALSE);
    EnableWindow(st->stopBtn, TRUE);
    UpdateStatus(st);

    if (st->worker.joinable()) st->worker.join();
    st->worker = std::thread([st, startDir, masks, needle, caseSensitive, recurse, hidden]() {
        WalkDirectory(st, startDir, masks, needle, caseSensitive, recurse, hidden, 0);
        st->running.store(false);
        PostMessageW(st->hwnd, WM_APP + 202, 0, 0);
    });
}

// ---------- интерфейс ----------

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

void LayoutSearch(SearchState* st) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int pad = 10;
    const int labelW = 88;
    const int editX = pad + labelW + 8;
    const int rowH = 24;
    const int right = w - pad;
    const int browseW = 128;

    int y = 12;
    MoveWindow(st->labelMask, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->mask, editX, y, right - editX - 112, rowH, TRUE);
    MoveWindow(st->templatesBtn, right - 104, y, 104, rowH, TRUE);
    MoveWindow(st->hint, editX, y + rowH + 2, right - editX, 16, TRUE);

    y += rowH + 20;
    MoveWindow(st->labelDir, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->dir, editX, y, right - editX - browseW - 8, rowH, TRUE);
    MoveWindow(st->browseBtn, right - browseW, y, browseW, rowH, TRUE);

    y += rowH + 8;
    MoveWindow(st->labelRoots, pad, y + 3, labelW, 18, TRUE);
    int x = editX;
    for (int i = 0; i < 26; ++i) {
        HWND hb = GetDlgItem(st->hwnd, kDriveIdBase + i);
        if (!hb) continue;
        MoveWindow(hb, x, y, 38, 22, TRUE);
        x += 42;
    }

    y += 30;
    MoveWindow(st->labelText, pad, y + 4, labelW, 18, TRUE);
    MoveWindow(st->text, editX, y, right - editX, rowH, TRUE);

    y += rowH + 8;
    // Строка фильтров: размер в Кб и дата в ДД.ММ.ГГГГ, пустое поле — без ограничения.
    MoveWindow(st->labelFilter, pad, y + 3, labelW, 18, TRUE);
    int fx = editX;
    MoveWindow(st->labelSize, fx, y + 3, 52, 18, TRUE);
    fx += 54;
    MoveWindow(st->minSize, fx, y, 64, rowH, TRUE);
    fx += 70;
    MoveWindow(st->dash1, fx, y + 3, 12, 18, TRUE);
    fx += 20;
    MoveWindow(st->maxSize, fx, y, 64, rowH, TRUE);
    fx += 70;
    MoveWindow(st->labelKb, fx, y + 3, 26, 18, TRUE);
    fx += 34;
    MoveWindow(st->labelDate, fx, y + 3, 40, 18, TRUE);
    fx += 46;
    MoveWindow(st->dateFrom, fx, y, 92, rowH, TRUE);
    fx += 100;
    MoveWindow(st->dash2, fx, y + 3, 12, 18, TRUE);
    fx += 20;
    MoveWindow(st->dateTo, fx, y, 92, rowH, TRUE);

    y += rowH + 8;
    MoveWindow(st->subdirs, editX, y, 150, 20, TRUE);
    MoveWindow(st->caseCheck, editX + 160, y, 170, 20, TRUE);
    MoveWindow(st->hiddenCheck, editX + 340, y, 190, 20, TRUE);
    MoveWindow(st->archivesCheck, editX + 540, y, 160, 20, TRUE);

    y += 28;
    const int startW = 160;
    const int stopW = 92;
    MoveWindow(st->startBtn, right - startW, y, startW, rowH, TRUE);
    MoveWindow(st->stopBtn, right - startW - 8 - stopW, y, stopW, rowH, TRUE);
    MoveWindow(st->status, pad, y + 4, right - startW - 8 - stopW - pad - 12, 18, TRUE);

    y += rowH + 8;
    const int bottomY = h - pad - rowH;
    const int listH = (bottomY - 8 - y) > 60 ? (bottomY - 8 - y) : 60;
    MoveWindow(st->list, pad, y, w - 2 * pad, listH, TRUE);

    MoveWindow(st->openDirBtn, pad, bottomY, 170, rowH, TRUE);
    MoveWindow(GetDlgItem(st->hwnd, IDCANCEL), right - 140, bottomY, 140, rowH, TRUE);
}

void UpdateStatus(SearchState* st) {
    size_t found = 0;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        found = st->rows.size();
    }
    wchar_t buf[256]{};
    swprintf(buf, 256, L"Найдено: %zu   ·   просмотрено файлов: %d%s", found, st->scanned.load(),
             st->running.load() ? L"   ·   идёт поиск" : L"");
    SetWindowTextW(st->status, buf);
}

void FillResults(SearchState* st) {
    std::vector<Result> batch;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (!st->pending.empty()) batch.swap(st->pending);
    }
    if (batch.empty()) {
        UpdateStatus(st);
        return;
    }

    size_t base = 0;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        base = st->rows.size();
        st->rows.insert(st->rows.end(), batch.begin(), batch.end());
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        const Result& r = batch[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)(base + i);
        item.iSubItem = 0;
        item.pszText = (LPWSTR)r.name.c_str();
        item.lParam = (LPARAM)(base + i);
        const int index = ListView_InsertItem(st->list, &item);
        if (index < 0) continue;
        ListView_SetItemText(st->list, index, 1, (LPWSTR)r.dir.c_str());
        std::wstring size = FmtSize(r.size);
        ListView_SetItemText(st->list, index, 2, (LPWSTR)size.c_str());
        std::wstring date = FmtDate(r.mtime);
        ListView_SetItemText(st->list, index, 3, (LPWSTR)date.c_str());
    }
    UpdateStatus(st);
}

int CALLBACK CompareResults(LPARAM a, LPARAM b, LPARAM ctx) {
    auto* st = reinterpret_cast<SearchState*>(ctx);
    const size_t ia = (size_t)a;
    const size_t ib = (size_t)b;
    if (ia >= st->rows.size() || ib >= st->rows.size()) return 0;
    const Result& x = st->rows[ia];
    const Result& y = st->rows[ib];

    int cmp = 0;
    switch (st->sortCol) {
        case 0: cmp = CompareNatural(x.name, y.name); break;
        case 1: cmp = CompareNatural(x.dir, y.dir); break;
        case 2: cmp = (x.size < y.size) ? -1 : (x.size > y.size ? 1 : 0); break;
        case 3: cmp = CompareFileTime(&x.mtime, &y.mtime); break;
        default: cmp = 0; break;
    }
    if (cmp == 0) cmp = CompareNatural(x.name, y.name);
    return st->sortAsc ? cmp : -cmp;
}

Result* SelectedResult(SearchState* st) {
    int sel = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    if (sel < 0 && ListView_GetItemCount(st->list) > 0) sel = 0;
    if (sel < 0) return nullptr;

    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    if (!ListView_GetItem(st->list, &item)) return nullptr;
    const size_t index = (size_t)item.lParam;
    if (index >= st->rows.size()) return nullptr;
    return &st->rows[index];
}

void ChooseAndClose(SearchState* st, bool directoryOnly) {
    Result* r = SelectedResult(st);
    if (!r) return;
    st->chosen = directoryOnly ? r->dir + L"\\" : r->path;
    LogDebug(L"search choose '" + st->chosen + L"'");
    DestroyWindow(st->hwnd);
}

void CopySelectedPath(SearchState* st) {
    Result* r = SelectedResult(st);
    if (!r) return;
    if (!OpenClipboard(st->hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (r->path.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, r->path.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

void BrowseForFolder(SearchState* st) {
    BROWSEINFOW bi{};
    bi.hwndOwner = st->hwnd;
    bi.lpszTitle = L"Где искать";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH * 4]{};
    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(st->dir, path);
    CoTaskMemFree(pidl);
}

LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<SearchState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<SearchState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->hwnd = hwnd;

            st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            st->fontSmall = CreateFontW(-Scale(12, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            st->labelMask = MakeChild(hwnd, L"STATIC", L"Искать:", SS_LEFT, 0);
            st->labelDir = MakeChild(hwnd, L"STATIC", L"Место:", SS_LEFT, 0);
            st->labelRoots = MakeChild(hwnd, L"STATIC", L"Корни:", SS_LEFT, 0);
            st->labelText = MakeChild(hwnd, L"STATIC", L"С текстом:", SS_LEFT, 0);

            st->mask = MakeChild(hwnd, L"EDIT", L"*", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kMaskId);
            st->dir = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kDirId);
            st->text = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, kTextId);
            st->hint = MakeChild(hwnd, L"STATIC",
                                 L"Через ; — несколько масок: *.txt;*.md · фильтр: размер в Кб, дата ДД.ММ.ГГГГ",
                                 SS_LEFT, kHintId);

            st->browseBtn = MakeChild(hwnd, L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, kBrowseId);
            st->templatesBtn = MakeChild(hwnd, L"BUTTON", L"Шаблоны", BS_PUSHBUTTON | WS_TABSTOP, kTemplatesId);

            st->labelFilter = MakeChild(hwnd, L"STATIC", L"Фильтр:", SS_LEFT, 0);
            st->labelSize = MakeChild(hwnd, L"STATIC", L"размер", SS_LEFT, 0);
            st->minSize = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, kMinSizeId);
            st->dash1 = MakeChild(hwnd, L"STATIC", L"—", SS_CENTER, 0);
            st->maxSize = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, kMaxSizeId);
            st->labelKb = MakeChild(hwnd, L"STATIC", L"Кб", SS_LEFT, 0);
            st->labelDate = MakeChild(hwnd, L"STATIC", L"дата", SS_LEFT, 0);
            st->dateFrom = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | WS_TABSTOP, kDateFromId);
            st->dash2 = MakeChild(hwnd, L"STATIC", L"—", SS_CENTER, 0);
            st->dateTo = MakeChild(hwnd, L"EDIT", L"", WS_BORDER | WS_TABSTOP, kDateToId);
            st->caseCheck = MakeChild(hwnd, L"BUTTON", L"Учитывать регистр", BS_AUTOCHECKBOX | WS_TABSTOP, kCaseId);
            st->subdirs = MakeChild(hwnd, L"BUTTON", L"В подкаталогах", BS_AUTOCHECKBOX | WS_TABSTOP, kSubdirsId);
            st->hiddenCheck = MakeChild(hwnd, L"BUTTON", L"Скрытые и системные", BS_AUTOCHECKBOX | WS_TABSTOP,
                                        kHiddenId);
            st->archivesCheck = MakeChild(hwnd, L"BUTTON", L"Искать в архивах", BS_AUTOCHECKBOX | WS_TABSTOP,
                                          kArchivesId);
            st->startBtn = MakeChild(hwnd, L"BUTTON", L"Начать поиск", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            st->stopBtn = MakeChild(hwnd, L"BUTTON", L"Стоп", BS_PUSHBUTTON | WS_TABSTOP, kStopId);
            st->openDirBtn = MakeChild(hwnd, L"BUTTON", L"Открыть каталог", BS_PUSHBUTTON | WS_TABSTOP, kOpenDirId);
            MakeChild(hwnd, L"BUTTON", L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                       0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kListId, g_inst, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 300; col.iSubItem = 0; col.pszText = (LPWSTR)L"Файл";
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 380; col.iSubItem = 1; col.pszText = (LPWSTR)L"Каталог";
            ListView_InsertColumn(st->list, 1, &col);
            col.cx = 90; col.iSubItem = 2; col.pszText = (LPWSTR)L"Размер";
            ListView_InsertColumn(st->list, 2, &col);
            col.cx = 130; col.iSubItem = 3; col.pszText = (LPWSTR)L"Дата";
            ListView_InsertColumn(st->list, 3, &col);

            st->status = MakeChild(hwnd, L"STATIC", L"Готов к поиску", SS_LEFT, kStatusId);

            int slot = 0;
            for (const auto& drive : GetDrives()) {
                if (slot >= 26) break;
                std::wstring label;
                label.push_back(drive.letter);
                label += L":";
                CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(kDriveIdBase + slot), g_inst, nullptr);
                ++slot;
            }

            for (HWND h : {st->archivesCheck, st->labelMask, st->labelDir, st->labelRoots, st->labelText, st->mask, st->dir,
                           st->text, st->caseCheck, st->subdirs, st->hiddenCheck, st->list, st->status,
                           st->startBtn, st->stopBtn, st->browseBtn, st->openDirBtn, st->templatesBtn,
                           st->labelFilter, st->labelSize, st->minSize, st->dash1, st->maxSize, st->labelKb,
                           st->labelDate, st->dateFrom, st->dash2, st->dateTo,
                           GetDlgItem(hwnd, IDCANCEL)}) {
                if (h && st->font) SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
            }
            if (st->hint && st->fontSmall) SendMessageW(st->hint, WM_SETFONT, (WPARAM)st->fontSmall, TRUE);
            for (int i = 0; i < 26; ++i) {
                if (HWND h = GetDlgItem(hwnd, kDriveIdBase + i))
                    SendMessageW(h, WM_SETFONT, (WPARAM)st->fontSmall, TRUE);
            }

            EnableWindow(st->stopBtn, FALSE);
            SetTimer(hwnd, kTimerId, 120, nullptr);
            return 0;
        }

        case WM_SIZE:
            if (st) LayoutSearch(st);
            return 0;

        case WM_TIMER:
            if (st && wParam == kTimerId) FillResults(st);
            return 0;

        case WM_APP + 202:
            if (st) {
                FillResults(st);
                EnableWindow(st->startBtn, TRUE);
                EnableWindow(st->stopBtn, FALSE);
                UpdateStatus(st);
            }
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (!st) break;

            if (id == IDOK) {
                if (GetFocus() == st->list) ChooseAndClose(st, false);
                else StartWorker(st);
                return 0;
            }
            if (id == IDCANCEL) {
                st->stop.store(true);
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kStopId) {
                st->stop.store(true);
                EnableWindow(st->stopBtn, FALSE);
                SetWindowTextW(st->status, L"Останавливаю поиск…");
                return 0;
            }
            if (id == kBrowseId) {
                BrowseForFolder(st);
                return 0;
            }
            if (id == kTemplatesId) {
                ShowTemplatesMenu(st);
                return 0;
            }
            if (id == kOpenDirId) {
                ChooseAndClose(st, true);
                return 0;
            }
            if (id >= kDriveIdBase && id < kDriveIdBase + 26) {
                wchar_t root[8]{};
                GetWindowTextW(reinterpret_cast<HWND>(lParam), root, 8);
                if (root[0]) {
                    std::wstring path = root;
                    if (path.back() != L'\\') path += L'\\';
                    SetWindowTextW(st->dir, path.c_str());
                }
                return 0;
            }
            break;
        }

        case WM_NOTIFY: {
            auto* info = reinterpret_cast<NMHDR*>(lParam);
            if (!st || !info || info->idFrom != kListId) break;

            if (info->code == NM_DBLCLK || info->code == NM_RETURN) {
                ChooseAndClose(st, false);
                return 0;
            }
            if (info->code == LVN_KEYDOWN) {
                auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                if (key->wVKey == VK_RETURN) {
                    ChooseAndClose(st, false);
                    return 0;
                }
            }
            if (info->code == LVN_COLUMNCLICK) {
                auto* click = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (st->sortCol == click->iSubItem) {
                    st->sortAsc = !st->sortAsc;
                } else {
                    st->sortCol = click->iSubItem;
                    st->sortAsc = true;
                }
                ListView_SortItems(st->list, CompareResults, (LPARAM)st);
                return 0;
            }
            break;
        }

        case WM_CONTEXTMENU: {
            if (!st || reinterpret_cast<HWND>(wParam) != st->list) break;
            if (!SelectedResult(st)) return 0;

            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuOpenDir, L"Открыть каталог");
            AppendMenuW(menu, MF_STRING, kMenuCopyPath, L"Скопировать путь");
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                GetWindowRect(st->list, &rc);
                pt.x = rc.left + 20;
                pt.y = rc.top + 20;
            }
            const int cmd =
                (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == kMenuOpenDir) ChooseAndClose(st, true);
            if (cmd == kMenuCopyPath) CopySelectedPath(st);
            return 0;
        }

        case WM_CLOSE:
            if (st) st->stop.store(true);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                st->stop.store(true);
                SavePrefs(st);

                RECT wr{};
                if (GetWindowRect(hwnd, &wr)) {
                    const std::wstring iniPath = SettingsFilePath();
                    wchar_t buf[32]{};
                    swprintf(buf, 32, L"%d", (int)wr.left);
                    WritePrivateProfileStringW(L"Search", L"X", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)wr.top);
                    WritePrivateProfileStringW(L"Search", L"Y", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.right - wr.left));
                    WritePrivateProfileStringW(L"Search", L"W", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.bottom - wr.top));
                    WritePrivateProfileStringW(L"Search", L"H", buf, iniPath.c_str());
                }
                if (st->worker.joinable()) st->worker.join();
                KillTimer(hwnd, kTimerId);
                if (st->font) DeleteObject(st->font);
                if (st->fontSmall) DeleteObject(st->fontSmall);
                st->font = nullptr;
                st->fontSmall = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::wstring SearchDialog(HWND owner, const std::wstring& startDir) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SearchProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSearchClass;
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
        RegisterClassExW(&wc);
        registered = true;
    }

    SearchState state;
    state.chosen.clear();

    RECT ownerRect{0, 0, 900, 600};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

    // Позиция и размер запоминаются между вызовами.
    const std::wstring iniPath = SettingsFilePath();
    const int savedX = (int)GetPrivateProfileIntW(L"Search", L"X", ownerRect.left + 60, iniPath.c_str());
    const int savedY = (int)GetPrivateProfileIntW(L"Search", L"Y", ownerRect.top + 40, iniPath.c_str());
    const int savedW = (int)GetPrivateProfileIntW(L"Search", L"W", 940, iniPath.c_str());
    const int savedH = (int)GetPrivateProfileIntW(L"Search", L"H", 640, iniPath.c_str());
    const RECT frame = ClampWindowRect(RECT{savedX, savedY, savedX + savedW, savedY + savedH});
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kSearchClass, L"Поиск файлов",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, frame.left, frame.top,
                                width, height, nullptr, nullptr, g_inst, &state);
    if (!hwnd) return {};

    ReadPrefs(&state, startDir);
    LayoutSearch(&state);

    bool ownerWasEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(state.mask);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (ownerWasEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        SetFocus(owner);
    }
    return state.chosen;
}
