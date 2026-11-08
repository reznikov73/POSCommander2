// Реестр плагинов Total Commander (wcx): поиск DLL и чтение возможностей.
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct PluginInfo {
    std::wstring path;
    std::wstring name;
    int caps = 0;
    std::wstring capsText;
    bool loaded = false;
    std::wstring error;
};

// Сканирует каталоги с плагинами (по умолчанию — установка TC на этой машине).
std::vector<PluginInfo> ScanWcxPlugins(const std::vector<std::wstring>& folders);

// Окно «Плагины» со списком найденного.
void PluginsDialog(HWND owner);
