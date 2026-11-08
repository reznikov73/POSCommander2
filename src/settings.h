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
    int sortColumn = 0;
    bool sortReverse = false;

    std::vector<std::wstring> leftTabs, rightTabs;
    int leftActive = 0, rightActive = 0;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();
std::wstring SettingsFilePath();
