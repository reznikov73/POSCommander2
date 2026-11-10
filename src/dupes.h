// Поиск дубликатов: файлы с одинаковым содержимым в каталоге и подкаталогах.
#pragma once

#include "common.h"

#include <string>

// Открывает окно поиска дубликатов для каталога root. Модальное для owner.
void FindDuplicates(HWND owner, const std::wstring& root);
