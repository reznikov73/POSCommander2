// Просмотр и распаковка архивов через 7z.exe (zip, 7z, rar, tar, gz и т. п.).
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct ArchiveEntry {
    std::wstring name;        // путь внутри архива, разделители '\'
    std::wstring size;        // как строка из 7z
    std::wstring modified;
    bool isDir = false;
};

bool IsArchivePath(const std::wstring& path);

// Полный список содержимого архива (все уровни).
bool ListArchive(const std::wstring& archive, std::vector<ArchiveEntry>& out);

// Распаковка указанных путей (пусто = всё) в каталог target.
bool ExtractFromArchive(const std::wstring& archive, const std::vector<std::wstring>& innerPaths,
                        const std::wstring& target, std::wstring& error);

// Временный каталог для просмотра файлов из архива.
std::wstring ArchiveTempDir();

// Окно содержимого архива. Возвращает каталог, куда распаковали (пусто — ничего).
std::wstring ArchiveDialog(HWND owner, const std::wstring& archivePath, const std::wstring& defaultTarget);
