// Просмотр и распаковка архивов через 7z.exe (zip, 7z, rar, tar, gz и т. п.).
#pragma once

#include "common.h"

#include <string>

bool IsArchivePath(const std::wstring& path);

// Окно содержимого архива. defaultTarget — куда распаковывать по умолчанию.
// Возвращает путь каталога, куда что-то распаковали (пустая строка, если ничего).
std::wstring ArchiveDialog(HWND owner, const std::wstring& archivePath, const std::wstring& defaultTarget);
