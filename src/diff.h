// Сравнение двух файлов: построчный diff и две синхронно прокручиваемые панели.
#pragma once

#include "common.h"

#include <string>

// Открывает окно сравнения. Модальное для owner.
void DiffFiles(HWND owner, const std::wstring& leftPath, const std::wstring& rightPath);
