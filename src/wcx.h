// Вызов API плагинов Total Commander (wcx): список содержимого архива.
// Плагин грузится в наш процесс, поэтому тяжёлые/рискованные вызовы делаем
// в отдельном запуске (режим --wcx-test).
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct ArchiveEntry;

struct WcxResult {
    bool ok = false;
    std::wstring error;
    int openResult = -1;
    int readResult = -1;
    std::vector<ArchiveEntry> entries;
    std::wstring pluginPath;
};

// Открыть архив через указанный плагин и прочитать список файлов.
WcxResult WcxListArchive(const std::wstring& dllPath, const std::wstring& archivePath);

// Режим диагностики: --wcx-test <dll> <archive>. Отчёт пишется в %TEMP%\posc2-wcx.txt.
int WcxSelfTest(const std::wstring& dllPath, const std::wstring& archivePath);
