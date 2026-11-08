// Окно поиска файлов (Alt+F7): маска, каталог, поиск по содержимому, фоновый поток.
#pragma once

#include "common.h"

#include <string>

// Показывает окно поиска. Возвращает путь выбранного файла или пустую строку.
std::wstring SearchDialog(HWND owner, const std::wstring& startDir);
