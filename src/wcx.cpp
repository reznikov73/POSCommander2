#include "wcx.h"

#include "archive.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// ---------- структуры wcx SDK (32-битные) ----------

struct WcxOpenArchiveData {
    int size;              // sizeof(WcxOpenArchiveData)
    char* arcName;
    int openMode;          // 0 = PK_OM_LIST, 1 = PK_OM_EXTRACT
    int openResult;        // 0 = успех
    char* cmtBuf;
    int cmtBufSize;
    int cmtSize;
    int cmtState;
};

struct WcxHeaderData {
    int size;              // sizeof(WcxHeaderData) = 56 в 32-битной сборке
    DWORD fileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD fileSizeHigh;
    DWORD fileSizeLow;
    DWORD reserved0;
    DWORD reserved1;
    char* fileName;        // ANSI/UTF-8 имя внутри архива
    char* arcName;         // путь внутри архива (может быть nullptr)
};

const int kPkOmList = 0;
const int kEendArchive = 10;
const int kEbadArchive = 11;

using OpenArchiveFn = void* (__stdcall*)(WcxOpenArchiveData*);
using ReadHeaderFn = int (__stdcall*)(void*, WcxHeaderData*);
using CloseArchiveFn = int (__stdcall*)(void*);
using GetPackerCapsFn = int (__stdcall*)();
using SetChangeVolProcFn = void (__stdcall*)(void*, void*);
using SetProcessDataProcFn = void (__stdcall*)(void*, void*);
using SetCryptProcFn = void (__stdcall*)(void*, int, void*);

// Обратные вызовы, которые TC всегда предоставляет плагину.
// Без них плагин может вызвать нулевой указатель и упасть.
int __stdcall WcxChangeVol(char* arcName, int mode) {
    (void)arcName;
    (void)mode;
    return 0;      // 0 = том доступен
}

int __stdcall WcxProgress(char* fileName, int size) {
    (void)fileName;
    (void)size;
    return 1;      // 1 = продолжать операцию
}

int __stdcall WcxCrypt(void* arc, int mode, char* password) {
    (void)arc;
    (void)mode;
    (void)password;
    return 0;      // 0 = пароль не нужен
}

std::string ToAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring FromAnsi(const char* text) {
    if (!text || !*text) return {};
    int len = (int)strlen(text);
    int need = MultiByteToWideChar(CP_ACP, 0, text, len, nullptr, 0);
    if (need <= 0) {
        need = MultiByteToWideChar(CP_OEMCP, 0, text, len, nullptr, 0);
        if (need <= 0) return {};
        std::wstring out((size_t)need, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, text, len, out.data(), need);
        return out;
    }
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, len, out.data(), need);
    return out;
}

}  // namespace

WcxResult WcxListArchive(const std::wstring& dllPath, const std::wstring& archivePath) {
    WcxResult result;
    result.pluginPath = dllPath;

    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll) {
        result.error = L"Не удалось загрузить плагин (разрядность или зависимости)";
        return result;
    }

    auto open = reinterpret_cast<OpenArchiveFn>(GetProcAddress(dll, "OpenArchive"));
    auto read = reinterpret_cast<ReadHeaderFn>(GetProcAddress(dll, "ReadHeader"));
    auto close = reinterpret_cast<CloseArchiveFn>(GetProcAddress(dll, "CloseArchive"));

    if (!open || !read || !close) {
        FreeLibrary(dll);
        result.error = L"Плагин не экспортирует OpenArchive/ReadHeader/CloseArchive";
        return result;
    }

    std::string arcName = ToAnsi(archivePath);
    WcxOpenArchiveData data{};
    data.size = sizeof(WcxOpenArchiveData);
    data.arcName = arcName.data();
    data.openMode = kPkOmList;
    data.openResult = 0;

    void* handle = open(&data);
    result.openResult = data.openResult;

    if (!handle || data.openResult != 0) {
        result.error = L"OpenArchive вернул ошибку " + std::to_wstring(data.openResult);
        FreeLibrary(dll);
        return result;
    }

    // Отключаем интерактивные запросы плагина (смена тома, прогресс, пароль).
    if (auto setVol = reinterpret_cast<SetChangeVolProcFn>(GetProcAddress(dll, "SetChangeVolProc"))) {
        setVol(handle, reinterpret_cast<void*>(&WcxChangeVol));
    }
    if (auto setProgress = reinterpret_cast<SetProcessDataProcFn>(GetProcAddress(dll, "SetProcessDataProc"))) {
        setProgress(handle, reinterpret_cast<void*>(&WcxProgress));
    }
    if (auto setCrypt = reinterpret_cast<SetCryptProcFn>(GetProcAddress(dll, "SetCryptProc"))) {
        setCrypt(handle, 0, reinterpret_cast<void*>(&WcxCrypt));
    }

    int guard = 0;
    while (guard++ < 20000) {
        WcxHeaderData header{};
        header.size = sizeof(WcxHeaderData);
        const int rc = read(handle, &header);
        result.readResult = rc;
        if (rc == kEendArchive || rc != 0) break;

        ArchiveEntry entry;
        entry.name = FromAnsi(header.fileName);
        if (entry.name.empty()) continue;
        entry.isDir = (header.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const ULONGLONG size = ((ULONGLONG)header.fileSizeHigh << 32) | header.fileSizeLow;
        entry.size = std::to_wstring(size);
        entry.modified.clear();
        result.entries.push_back(std::move(entry));
    }

    close(handle);
    FreeLibrary(dll);

    result.ok = !result.entries.empty() || result.readResult == kEendArchive;
    if (!result.ok && result.error.empty()) {
        result.error = L"Плагин не отдал ни одной записи (код " + std::to_wstring(result.readResult) + L")";
    }
    return result;
}

int WcxSelfTest(const std::wstring& dllPath, const std::wstring& archivePath) {
    const WcxResult r = WcxListArchive(dllPath, archivePath);

    std::wstring report;
    report += L"Плагин : " + dllPath + L"\r\n";
    report += L"Архив  : " + archivePath + L"\r\n\r\n";
    report += L"OpenArchive: " + std::to_wstring(r.openResult) + L"\r\n";
    report += L"ReadHeader : " + std::to_wstring(r.readResult) + L"\r\n";
    report += L"Записей    : " + std::to_wstring(r.entries.size()) + L"\r\n";
    if (!r.error.empty()) report += L"Ошибка     : " + r.error + L"\r\n";
    report += L"\r\n";
    for (auto& e : r.entries) {
        report += L"  " + e.name + (e.isDir ? L"\\" : L"") + L"   " + e.size + L"\r\n";
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outPath = std::wstring(tmp) + L"posc2-wcx.txt";

    HANDLE file = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        int need = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            std::string utf8((size_t)need, '\0');
            WideCharToMultiByte(CP_UTF8, 0, report.c_str(), (int)report.size(), utf8.data(), need, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
        CloseHandle(file);
    }

    LogDebug(L"wcx selftest: " + report);
    return r.ok ? 0 : 1;
}
