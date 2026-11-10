// Сравнение файлов: LCS-дифф, две панели с подсветкой и синхронной прокруткой.
#include "diff.h"

#include "util.h"

#include <commctrl.h>
#include <richedit.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const wchar_t* kDiffClass = L"POSDiff2";
const int kLeftId = 700;
const int kRightId = 701;
const int kStatusId = 702;
const int kCloseId = 703;
const UINT kSyncMsg = WM_APP + 421;

// Ограничение на LCS-таблицу: 2000x2000 строк — это уже мегабайты.
const size_t kMaxDiffCells = 4u * 1000u * 1000u;

struct Line {
    std::wstring text;
    int kind = 0;          // 0 равно, 1 удалено (слева), 2 добавлено (справа)
};

struct DiffState {
    HWND hwnd = nullptr;
    HWND left = nullptr;
    HWND right = nullptr;
    HWND status = nullptr;
    HFONT font = nullptr;
    std::wstring leftName;
    std::wstring rightName;
    int added = 0;
    int removed = 0;
    int equal = 0;
    bool done = false;
};

DiffState g_diff;

std::wstring DecodeBytes(const std::vector<unsigned char>& raw) {
    if (raw.empty()) return {};

    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
    }

    UINT cp = CP_ACP;
    size_t offset = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        cp = CP_UTF8;
        offset = 3;
    } else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)raw.data(), (int)raw.size(), nullptr, 0) > 0) {
        cp = CP_UTF8;
    }

    const int need = MultiByteToWideChar(cp, 0, (LPCCH)raw.data() + offset, (int)(raw.size() - offset), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(cp, 0, (LPCCH)raw.data() + offset, (int)(raw.size() - offset), out.data(), need);
    return out;
}

std::vector<std::wstring> LoadLines(const std::wstring& path, bool& ok) {
    ok = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG limit = 8LL * 1024 * 1024;
    const DWORD toRead = (DWORD)(size.QuadPart > limit ? limit : size.QuadPart);

    std::vector<unsigned char> raw(toRead);
    DWORD read = 0;
    const BOOL got = toRead == 0 ? TRUE : ReadFile(f, raw.data(), toRead, &read, nullptr);
    CloseHandle(f);
    if (!got) return {};
    raw.resize(read);

    std::wstring text = DecodeBytes(raw);
    ok = true;

    std::vector<std::wstring> lines;
    std::wstring cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            lines.push_back(cur);
            cur.clear();
        } else if (c == L'\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Классический LCS-дифф: таблица длин, затем обратный проход.
bool BuildDiff(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b,
               std::vector<Line>& left, std::vector<Line>& right, int& equalCount, int& addedCount,
               int& removedCount) {
    const size_t n = a.size();
    const size_t m = b.size();

    std::vector<size_t> lens;
    if (n == 0 || m == 0) {
        for (const auto& s : a) {
            left.push_back({s, 1});
            right.push_back({L"", 1});
            ++removedCount;
        }
        for (const auto& s : b) {
            left.push_back({L"", 2});
            right.push_back({s, 2});
            ++addedCount;
        }
        return true;
    }

    // Таблица LCS (n+1)x(m+1) из uint16 не влезает при больших файлах — проверяем заранее.
    if ((n + 1) * (m + 1) > kMaxDiffCells) return false;

    std::vector<uint16_t> dp((n + 1) * (m + 1), 0);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (a[i - 1] == b[j - 1]) {
                dp[i * (m + 1) + j] = (uint16_t)(dp[(i - 1) * (m + 1) + (j - 1)] + 1);
            } else {
                const uint16_t up = dp[(i - 1) * (m + 1) + j];
                const uint16_t lf = dp[i * (m + 1) + (j - 1)];
                dp[i * (m + 1) + j] = up > lf ? up : lf;
            }
        }
    }

    std::vector<Line> revLeft, revRight;
    size_t i = n, j = m;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            revLeft.push_back({a[i - 1], 0});
            revRight.push_back({b[j - 1], 0});
            ++equalCount;
            --i;
            --j;
        } else if (dp[(i - 1) * (m + 1) + j] >= dp[i * (m + 1) + (j - 1)]) {
            revLeft.push_back({a[i - 1], 1});
            revRight.push_back({L"", 1});
            ++removedCount;
            --i;
        } else {
            revLeft.push_back({L"", 2});
            revRight.push_back({b[j - 1], 2});
            ++addedCount;
            --j;
        }
    }
    while (i > 0) {
        revLeft.push_back({a[i - 1], 1});
        revRight.push_back({L"", 1});
        ++removedCount;
        --i;
    }
    while (j > 0) {
        revLeft.push_back({L"", 2});
        revRight.push_back({b[j - 1], 2});
        ++addedCount;
        --j;
    }

    left.assign(revLeft.rbegin(), revLeft.rend());
    right.assign(revRight.rbegin(), revRight.rend());

    // Между совпадающими строками собираем блок изменений целиком: удалённые и
    // добавленные строки встают парами в одну строку, иначе панели разъезжаются.
    std::vector<Line> mergedLeft, mergedRight;
    mergedLeft.reserve(left.size());
    mergedRight.reserve(right.size());

    size_t pos = 0;
    while (pos < left.size()) {
        if (left[pos].kind == 0) {
            mergedLeft.push_back(left[pos]);
            mergedRight.push_back(right[pos]);
            ++pos;
            continue;
        }

        std::vector<std::wstring> dels, ins;
        while (pos < left.size() && left[pos].kind != 0) {
            if (left[pos].kind == 1) {
                dels.push_back(left[pos].text);
            } else {
                ins.push_back(right[pos].text);
            }
            ++pos;
        }

        const size_t rows = dels.size() > ins.size() ? dels.size() : ins.size();
        for (size_t k = 0; k < rows; ++k) {
            const bool hasDel = k < dels.size();
            const bool hasIns = k < ins.size();
            mergedLeft.push_back({hasDel ? dels[k] : L"", hasDel ? 1 : 0});
            mergedRight.push_back({hasIns ? ins[k] : L"", hasIns ? 2 : 0});
        }
    }

    left.swap(mergedLeft);
    right.swap(mergedRight);
    return true;
}

void PaintSide(HWND edit, const std::vector<Line>& lines) {
    std::wstring text;
    std::vector<CHARRANGE> ranges;
    ranges.reserve(lines.size());

    for (const auto& line : lines) {
        const LONG start = (LONG)text.size();
        text += line.text;
        const LONG end = (LONG)text.size();
        ranges.push_back(CHARRANGE{start, end});
        text += L"\r\n";
    }

    SetWindowTextW(edit, text.c_str());

    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BACKCOLOR;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == 0) continue;
        cf.crBackColor = lines[i].kind == 1 ? RGB(255, 214, 214) : RGB(214, 242, 214);
        SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&ranges[i]);
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }

    SendMessageW(edit, EM_SETSEL, 0, 0);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void SyncScroll(HWND from, HWND to) {
    POINT pt{};
    SendMessageW(from, EM_GETSCROLLPOS, 0, (LPARAM)&pt);
    SendMessageW(to, EM_SETSCROLLPOS, 0, (LPARAM)&pt);
}

void UpdateStatus() {
    wchar_t buf[256]{};
    swprintf(buf, 256, L"Добавлено строк: %d · удалено: %d · совпадает: %d", g_diff.added, g_diff.removed,
             g_diff.equal);
    SetWindowTextW(g_diff.status, buf);
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 8;
    const int statusH = 20;
    const int gap = 6;
    const int width = (rc.right - 2 * pad - gap) / 2;

    MoveWindow(g_diff.left, pad, pad, width, rc.bottom - 2 * pad - statusH - 4, TRUE);
    MoveWindow(g_diff.right, pad + width + gap, pad, width, rc.bottom - 2 * pad - statusH - 4, TRUE);
    MoveWindow(g_diff.status, pad, rc.bottom - pad - statusH + 2, rc.right - 2 * pad - 110, statusH, TRUE);
    MoveWindow(GetDlgItem(hwnd, kCloseId), rc.right - pad - 100, rc.bottom - pad - 26, 100, 26, TRUE);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN || msg == WM_LBUTTONUP) {
        PostMessageW(GetParent(hwnd), kSyncMsg, (WPARAM)hwnd, 0);
    }
    return result;
}

LRESULT CALLBACK DiffProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_diff.hwnd = hwnd;
            const DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL;

            g_diff.left = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", style, 0, 0, 10, 10, hwnd,
                                          (HMENU)kLeftId, nullptr, nullptr);
            g_diff.right = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", style, 0, 0, 10, 10, hwnd,
                                           (HMENU)kRightId, nullptr, nullptr);
            g_diff.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                            (HMENU)kStatusId, nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10,
                            10, hwnd, (HMENU)kCloseId, nullptr, nullptr);

            g_diff.font = CreateFontW(-Scale(14, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      FIXED_PITCH | FF_MODERN, L"Consolas");
            for (HWND h : {g_diff.left, g_diff.right}) {
                SendMessageW(h, WM_SETFONT, (WPARAM)g_diff.font, TRUE);
                SetWindowSubclass(h, EditProc, 2, 0);
            }
            const HFONT uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            SendMessageW(g_diff.status, WM_SETFONT, (WPARAM)uiFont, TRUE);
            SendMessageW(GetDlgItem(hwnd, kCloseId), WM_SETFONT, (WPARAM)uiFont, TRUE);

            Layout(hwnd);
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd);
            return 0;

        case kSyncMsg: {
            const HWND from = reinterpret_cast<HWND>(wp);
            if (from == g_diff.left) {
                SyncScroll(g_diff.left, g_diff.right);
            } else if (from == g_diff.right) {
                SyncScroll(g_diff.right, g_diff.left);
            }
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == kCloseId) {
                g_diff.done = true;
                return 0;
            }
            break;

        case WM_CLOSE:
            g_diff.done = true;
            return 0;

        case WM_DESTROY:
            if (g_diff.font) DeleteObject(g_diff.font);
            g_diff.font = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void DiffFiles(HWND owner, const std::wstring& leftPath, const std::wstring& rightPath) {
    static bool registered = false;
    if (!registered) {
        LoadLibraryW(L"msftedit.dll");
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DiffProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDiffClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    bool okLeft = false;
    bool okRight = false;
    const auto leftLines = LoadLines(leftPath, okLeft);
    const auto rightLines = LoadLines(rightPath, okRight);
    if (!okLeft || !okRight) {
        MessageBoxW(owner, L"Не удалось прочитать один из файлов.", L"Сравнение файлов", MB_ICONERROR | MB_OK);
        return;
    }

    std::vector<Line> leftRows, rightRows;
    int equalCount = 0, addedCount = 0, removedCount = 0;
    if (!BuildDiff(leftLines, rightLines, leftRows, rightRows, equalCount, addedCount, removedCount)) {
        MessageBoxW(owner,
                    L"Файлы слишком большие для построчного сравнения (больше 2000 строк в одном из них).",
                    L"Сравнение файлов", MB_ICONINFORMATION | MB_OK);
        return;
    }

    g_diff = DiffState{};
    g_diff.leftName = leftPath;
    g_diff.rightName = rightPath;
    g_diff.added = addedCount;
    g_diff.removed = removedCount;
    g_diff.equal = equalCount;

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = 1100, h = 700;
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    const std::wstring title = L"Сравнение: " + leftPath.substr(leftPath.find_last_of(L'\\') + 1) + L" ↔ " +
                               rightPath.substr(rightPath.find_last_of(L'\\') + 1);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kDiffClass, title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;

    PaintSide(g_diff.left, leftRows);
    PaintSide(g_diff.right, rightRows);
    UpdateStatus();

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!g_diff.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
}
