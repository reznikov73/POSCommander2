// Диалоги: ввод строки и просмотрщик текста.
#pragma once

#include "common.h"

// Модальный ввод строки. Возвращает false при отмене.
bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              bool isPath);

// Параметры копирования/перемещения (F5/F6), как в TC: путь, маска, «перезаписать», «в фоне».
struct CopyMoveOptions {
    std::wstring target;
    std::wstring mask;          // пусто — все файлы
    bool overwriteAll = false;  // не спрашивать при совпадении имён
    bool background = false;    // поставить в очередь операций
};

bool CopyMoveBox(HWND owner, const std::wstring& title, const std::wstring& prompt, CopyMoveOptions& options);

// Просмотр файла (F3) в отдельном окне.
void ShowLister(HWND owner, const std::wstring& path);
