// Пакетное переименование: шаблон, предпросмотр «было → станет», проверка конфликтов.
#pragma once

#include "common.h"

#include <string>
#include <vector>

// Показывает окно пакетного переименования для names в каталоге dir.
// Возвращает число успешно переименованных файлов.
int BatchRenameDialog(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& names);
