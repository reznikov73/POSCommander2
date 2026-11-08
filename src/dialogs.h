// Диалоги: ввод строки и просмотрщик текста.
#pragma once

#include "common.h"

// Модальный ввод строки. Возвращает false при отмене.
bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              bool isPath);

// Просмотр файла (F3) в отдельном окне.
void ShowLister(HWND owner, const std::wstring& path);
