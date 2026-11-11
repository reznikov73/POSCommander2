// Встроенный движок ZIP: чтение оглавления, распаковка, создание — без 7-Zip.
// Работает на любой Windows (только Win32 + свой inflate), внешних зависимостей нет.
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct ZipItem {
    std::wstring name;                 // путь внутри архива, разделители '\'
    unsigned long long size = 0;       // размер после распаковки
    unsigned long long packed = 0;     // размер сжатых данных
    unsigned long long offset = 0;     // смещение локального заголовка
    unsigned long crc = 0;
    unsigned short method = 0;         // 0 — store, 8 — deflate
    unsigned short flags = 0;
    unsigned short dosTime = 0;
    unsigned short dosDate = 0;
    bool isDir = false;
};

// Оглавление zip (включая zip64). Ошибка — текст в error.
bool ZipReadIndex(const std::wstring& zipPath, std::vector<ZipItem>& items, std::wstring& error);

// Распаковка записей по именам (пусто — все) в каталог target.
bool ZipExtractTo(const std::wstring& zipPath, const std::vector<std::wstring>& names, const std::wstring& target,
                  std::wstring& error);

// Распаковка одной записи в файл targetFile (полный путь).
bool ZipExtractOne(const std::wstring& zipPath, const std::wstring& name, const std::wstring& targetFile,
                   std::wstring& error);

// Что положить в архив: файл на диске и его имя внутри архива.
struct ZipSource {
    std::wstring path;    // полный путь на диске
    std::wstring inner;   // имя внутри архива (с '\')
};

// Создание архива (сжатие store) из списка источников.
bool ZipCreateFrom(const std::wstring& zipPath, const std::vector<ZipSource>& sources, std::wstring& error);

// Быстрая проверка, что файл — zip (по сигнатуре PK\x03\x04 в начале и центральному каталогу).
bool ZipLooksLikeZip(const std::wstring& path);
