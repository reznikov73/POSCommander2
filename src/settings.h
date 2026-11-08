// Настройки в INI рядом с exe: окно, панели, вкладки, сортировка.
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct Settings {
    int x = 0, y = 0, w = 1180, h = 760;
    bool hasPosition = false;
    bool maximized = false;

    int splitPercent = 50;
    bool showHidden = true;
    int colWidth[4] = {240, 70, 100, 120};   // Имя, Тип, Размер, Дата
    int sortColumn = 0;
    bool sortReverse = false;

    std::vector<std::wstring> leftTabs, rightTabs;
    std::vector<std::wstring> leftTabNames, rightTabNames;   // пользовательские подписи вкладок
    std::vector<int> leftTabLocks, rightTabLocks;            // 1 — вкладка заблокирована
    std::vector<std::wstring> bookmarks;
    int leftActive = 0, rightActive = 0;
    int activeSide = 0;                                      // какая панель активна при запуске
    int leftCursor = 0, rightCursor = 0;                     // позиция курсора в панели
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();
std::wstring SettingsFilePath();
