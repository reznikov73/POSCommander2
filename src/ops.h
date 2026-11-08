// Файловые операции: один код и для интерфейса, и для самопроверки.
#pragma once

#include "common.h"

#include <string>
#include <vector>

// Копирование/перемещение через оболочку: системный прогресс, конфликты, отмена.
// silent = true — без диалогов и прогресса (для автотестов).
bool ShellCopyMove(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                   bool move, bool silent = false);

// Переименование одного пути. При неудаче err = код GetLastError, иначе 0.
bool RenamePath(const std::wstring& from, const std::wstring& to, DWORD& err);

// Удаление в корзину через оболочку.
bool DeleteViaShell(HWND owner, const std::vector<std::wstring>& items, bool silent = false);

// Создание одного каталога. true, если каталог создан или уже существует.
bool MakeDir(const std::wstring& path);

// Существует ли путь (файл или каталог), включая длинные пути.
bool PathExists(const std::wstring& path);
