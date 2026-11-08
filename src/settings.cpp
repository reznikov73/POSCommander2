#include "settings.h"

namespace {

Settings g_settings;

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return (int)GetPrivateProfileIntW(section, key, fallback, SettingsFilePath().c_str());
}

void WriteInt(const wchar_t* section, const wchar_t* key, int value) {
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%d", value);
    WritePrivateProfileStringW(section, key, buf, SettingsFilePath().c_str());
}

std::wstring ReadStr(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buf[2048]{};
    GetPrivateProfileStringW(section, key, fallback, buf, 2048, SettingsFilePath().c_str());
    return buf;
}

void WriteStr(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), SettingsFilePath().c_str());
}

std::vector<std::wstring> ReadTabs(const wchar_t* section, const wchar_t* prefix) {
    std::vector<std::wstring> tabs;
    int count = ReadInt(section, (std::wstring(prefix) + L"Count").c_str(), 0);
    if (count < 0) count = 0;
    if (count > 32) count = 32;
    for (int i = 0; i < count; ++i) {
        std::wstring key = std::wstring(prefix) + std::to_wstring(i);
        std::wstring value = ReadStr(section, key.c_str());
        if (!value.empty()) tabs.push_back(value);
    }
    return tabs;
}

void WriteTabs(const wchar_t* section, const wchar_t* prefix, const std::vector<std::wstring>& tabs) {
    WriteInt(section, (std::wstring(prefix) + L"Count").c_str(), (int)tabs.size());
    for (size_t i = 0; i < tabs.size(); ++i) {
        std::wstring key = std::wstring(prefix) + std::to_wstring(i);
        WriteStr(section, key.c_str(), tabs[i]);
    }
}

}  // namespace

std::wstring SettingsFilePath() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    cached = path + L"POSCommander2.ini";
    return cached;
}

Settings& GetSettings() { return g_settings; }

void LoadSettings() {
    Settings& s = g_settings;

    int px = ReadInt(L"Window", L"X", -1);
    int py = ReadInt(L"Window", L"Y", -1);
    s.w = ReadInt(L"Window", L"W", 1180);
    s.h = ReadInt(L"Window", L"H", 760);
    s.hasPosition = px >= 0 && py >= 0;
    s.x = s.hasPosition ? px : 0;
    s.y = s.hasPosition ? py : 0;
    s.maximized = ReadInt(L"Window", L"Maximized", 0) != 0;
    if (s.w < 400) s.w = 1180;
    if (s.h < 300) s.h = 760;

    s.splitPercent = ReadInt(L"Layout", L"SplitPercent", 50);
    if (s.splitPercent < 15) s.splitPercent = 15;
    if (s.splitPercent > 85) s.splitPercent = 85;

    s.showHidden = ReadInt(L"View", L"ShowHidden", 1) != 0;
    s.sortColumn = ReadInt(L"View", L"SortColumn", 0);
    s.sortReverse = ReadInt(L"View", L"SortReverse", 0) != 0;

    s.leftTabs = ReadTabs(L"Left", L"Tab");
    s.rightTabs = ReadTabs(L"Right", L"Tab");
    s.leftActive = ReadInt(L"Left", L"Active", 0);
    s.rightActive = ReadInt(L"Right", L"Active", 0);

    s.bookmarks = ReadTabs(L"Bookmarks", L"Path");
}

void SaveSettings() {
    Settings& s = g_settings;

    WriteInt(L"Window", L"X", s.x);
    WriteInt(L"Window", L"Y", s.y);
    WriteInt(L"Window", L"W", s.w);
    WriteInt(L"Window", L"H", s.h);
    WriteInt(L"Window", L"Maximized", s.maximized ? 1 : 0);

    WriteInt(L"Layout", L"SplitPercent", s.splitPercent);

    WriteInt(L"View", L"ShowHidden", s.showHidden ? 1 : 0);
    WriteInt(L"View", L"SortColumn", s.sortColumn);
    WriteInt(L"View", L"SortReverse", s.sortReverse ? 1 : 0);

    WriteTabs(L"Left", L"Tab", s.leftTabs);
    WriteInt(L"Left", L"Active", s.leftActive);
    WriteTabs(L"Right", L"Tab", s.rightTabs);
    WriteInt(L"Right", L"Active", s.rightActive);

    WriteInt(L"Bookmarks", L"PathCount", (int)s.bookmarks.size());
    for (size_t i = 0; i < s.bookmarks.size(); ++i) {
        std::wstring key = L"Path" + std::to_wstring(i);
        WriteStr(L"Bookmarks", key.c_str(), s.bookmarks[i]);
    }
}
