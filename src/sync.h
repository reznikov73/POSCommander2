// Синхронизация каталогов двух панелей: список расхождений и копирование.
#pragma once

#include "common.h"

#include <string>

// Показать окно синхронизации для двух корней. Модальное для owner.
void SyncDirectories(HWND owner, const std::wstring& leftRoot, const std::wstring& rightRoot);
