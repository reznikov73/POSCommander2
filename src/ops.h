// Файловые операции: один код и для интерфейса, и для самопроверки.
#pragma once

#include "common.h"

#include <string>
#include <vector>

// Копирование/перемещение. Основной путь — IFileOperation (системный прогресс,
// пауза, отмена, конфликты), при недоступности COM — SHFileOperation.
// silent = true — без диалогов и прогресса (для автотестов).
bool ShellCopyMove(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                   bool move, bool silent = false, bool overwriteAll = false);

// Переименование одного пути. При неудаче err = код GetLastError, иначе 0.
bool RenamePath(const std::wstring& from, const std::wstring& to, DWORD& err);

// Удаление в корзину (IFileOperation, при недоступности COM — SHFileOperation).
// permanent = true — удаление мимо корзины (Shift+Del).
bool DeleteViaShell(HWND owner, const std::vector<std::wstring>& items, bool silent = false,
                     bool permanent = false);

// Создание одного каталога. true, если каталог создан или уже существует.
bool MakeDir(const std::wstring& path);

// Существует ли путь (файл или каталог), включая длинные пути.
bool PathExists(const std::wstring& path);
