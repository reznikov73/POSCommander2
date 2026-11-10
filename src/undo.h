// Журнал переименований и их отмена (Ctrl+Z).
#pragma once

#include "common.h"

#include <string>
#include <utility>
#include <vector>

// Запомнить одно переименование (from → to).
void UndoPushRename(const std::wstring& from, const std::wstring& to);

// Запомнить пачку переименований как один шаг отмены.
void UndoPushRenames(const std::vector<std::pair<std::wstring, std::wstring>>& pairs);

// Отменить последний шаг. report — что получилось. Возвращает true, если что-то вернули.
bool UndoLastRename(std::wstring& report);

// Сколько шагов помним.
size_t UndoSteps();
