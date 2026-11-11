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

// Упаковка файлов в архив (формат по расширению: .zip, .7z, .tar).
bool CreateArchive(const std::wstring& archivePath, const std::vector<std::wstring>& items,
                   std::wstring& error);

// Форматы, которые 7-Zip умеет изменять (zip, 7z, tar): остальные только для чтения.
bool ArchiveCanModify(const std::wstring& archive);

// Добавить в архив файлы и каталоги с диска, разместив их внутри innerDir
// (пусто — в корень архива). Существующие записи с теми же именами заменяются.
bool ArchiveAdd(HWND owner, const std::wstring& archive, const std::wstring& innerDir,
                const std::vector<std::wstring>& sources, std::wstring& error);

// Удалить из архива записи по внутренним путям (каталог — со всем содержимым).
bool ArchiveDelete(HWND owner, const std::wstring& archive, const std::vector<std::wstring>& innerPaths,
                   std::wstring& error);

// Распаковка всего архива в каталог: окно прогресса, кнопка «Отмена».
bool ExtractArchiveWithProgress(HWND owner, const std::wstring& archive,
                                const std::wstring& target, const std::wstring& password,
                                std::wstring& error);

// Временный каталог для просмотра файлов из архива.
std::wstring ArchiveTempDir();

// Окно содержимого архива. Возвращает каталог, куда распаковали (пусто — ничего).
std::wstring ArchiveDialog(HWND owner, const std::wstring& archivePath, const std::wstring& defaultTarget);
