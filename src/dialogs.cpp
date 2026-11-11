// Реализация диалогов без .rc: шаблон собирается в памяти.
#include "dialogs.h"
#include "settings.h"
#include "version.h"
#include <shellapi.h>
#include "util.h"

#include <cstdio>

#include <commctrl.h>
#include <richedit.h>
#include <commdlg.h>
#include <cstring>

namespace {

// ---------- построитель шаблона диалога ----------

void PutW(std::vector<BYTE>& b, WORD v) {
    b.push_back((BYTE)(v & 0xFF));
    b.push_back((BYTE)(v >> 8));
}
void PutD(std::vector<BYTE>& b, DWORD v) {
    for (int i = 0; i < 4; i++) b.push_back((BYTE)((v >> (8 * i)) & 0xFF));
}
void PutS(std::vector<BYTE>& b, const wchar_t* s) {
    const BYTE* p = reinterpret_cast<const BYTE*>(s);
    size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    b.insert(b.end(), p, p + bytes);
}
void Align4(std::vector<BYTE>& b) {
    while (b.size() % 4) b.push_back(0);
}

class DlgBuilder {
public:
    DlgBuilder(const wchar_t* title, short cx, short cy) {
        PutD(buf_, DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);
        PutD(buf_, 0);
        PutW(buf_, 0);  // число элементов выставим в Get()
        PutW(buf_, 0);
        PutW(buf_, 0);
        PutW(buf_, (WORD)cx);
        PutW(buf_, (WORD)cy);
        PutW(buf_, 0);  // меню нет
        PutW(buf_, 0);  // класс по умолчанию
        PutS(buf_, title);
        PutW(buf_, 8);  // кегль шрифта
        PutS(buf_, L"Segoe UI");
    }

    // cls: 0x0082 — static, 0x0081 — edit, 0x0080 — button.
    void Add(const wchar_t* text, WORD id, WORD cls, DWORD style, short x, short y, short cx, short cy) {
        Align4(buf_);
        PutD(buf_, style);
        PutD(buf_, 0);
        PutW(buf_, (WORD)x);
        PutW(buf_, (WORD)y);
        PutW(buf_, (WORD)cx);
        PutW(buf_, (WORD)cy);
        PutW(buf_, id);
        PutW(buf_, 0xFFFF);
        PutW(buf_, cls);
        PutS(buf_, text ? text : L"");
        PutW(buf_, 0);  // данных создания нет
        count_++;
    }

    DLGTEMPLATE* Get() {
        auto* tpl = reinterpret_cast<DLGTEMPLATE*>(buf_.data());
        tpl->cdit = count_;
        return tpl;
    }

private:
    std::vector<BYTE> buf_;
    WORD count_ = 0;
};

const WORD kClsButton = 0x0080;
const WORD kClsEdit = 0x0081;
const WORD kClsStatic = 0x0082;

const int kIdPrompt = 100;
const int kIdEdit = 101;

struct InputCtx {
    std::wstring title;
    std::wstring prompt;
    std::wstring value;
    bool isPath = false;
    bool ok = false;
};

INT_PTR CALLBACK InputProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<InputCtx*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<InputCtx*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
            SetWindowTextW(dlg, ctx->title.c_str());
            SetDlgItemTextW(dlg, kIdPrompt, ctx->prompt.c_str());
            SetDlgItemTextW(dlg, kIdEdit, ctx->value.c_str());
            if (ctx->isPath) SHAutoComplete(GetDlgItem(dlg, kIdEdit), SHACF_FILESYSTEM);
            SendDlgItemMessageW(dlg, kIdEdit, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, kIdEdit));
            return FALSE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[32768]{};
                GetDlgItemTextW(dlg, kIdEdit, buf, 32768);
                if (ctx) {
                    ctx->value = buf;
                    ctx->ok = true;
                }
                EndDialog(dlg, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, 0);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

// ---------- копирование/перемещение (F5/F6) ----------

const int kIdMask = 102;
const int kIdOverwrite = 103;
const int kIdBackground = 104;

struct CopyCtx {
    std::wstring title;
    std::wstring prompt;
    CopyMoveOptions* options = nullptr;
};

INT_PTR CALLBACK CopyProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<CopyCtx*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<CopyCtx*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
            SetWindowTextW(dlg, ctx->title.c_str());
            SetDlgItemTextW(dlg, kIdPrompt, ctx->prompt.c_str());
            SetDlgItemTextW(dlg, kIdEdit, ctx->options->target.c_str());
            SetDlgItemTextW(dlg, kIdMask, ctx->options->mask.c_str());
            CheckDlgButton(dlg, kIdOverwrite, ctx->options->overwriteAll ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, kIdBackground, ctx->options->background ? BST_CHECKED : BST_UNCHECKED);
            SHAutoComplete(GetDlgItem(dlg, kIdEdit), SHACF_FILESYSTEM);
            SendDlgItemMessageW(dlg, kIdEdit, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, kIdEdit));
            return FALSE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t path[32768]{};
                wchar_t mask[1024]{};
                GetDlgItemTextW(dlg, kIdEdit, path, 32768);
                GetDlgItemTextW(dlg, kIdMask, mask, 1024);
                if (ctx) {
                    ctx->options->target = path;
                    ctx->options->mask = mask;
                    ctx->options->overwriteAll = IsDlgButtonChecked(dlg, kIdOverwrite) == BST_CHECKED;
                    ctx->options->background = IsDlgButtonChecked(dlg, kIdBackground) == BST_CHECKED;
                }
                EndDialog(dlg, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, 0);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool CopyMoveBox(HWND owner, const std::wstring& title, const std::wstring& prompt, CopyMoveOptions& options) {
    DlgBuilder builder(title.c_str(), 330, 128);
    builder.Add(prompt.c_str(), kIdPrompt, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 8, 310, 20);
    builder.Add(L"", kIdEdit, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 10, 28, 310, 14);
    builder.Add(L"Маска файлов:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 50, 70, 12);
    builder.Add(L"", kIdMask, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 84, 48, 100, 14);
    builder.Add(L"Перезаписывать без запроса", kIdOverwrite, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 10, 68, 200, 12);
    builder.Add(L"В фоне (в очередь операций)", kIdBackground, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 10, 84, 220, 12);
    builder.Add(L"OK", IDOK, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 198, 106, 58, 16);
    builder.Add(L"Отмена", IDCANCEL, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 262, 106, 58, 16);

    CopyCtx ctx;
    ctx.title = title;
    ctx.prompt = prompt;
    ctx.options = &options;

    const INT_PTR result = DialogBoxIndirectParamW(g_inst, builder.Get(), owner, CopyProc, (LPARAM)&ctx);
    LogDebug(L"CopyMoveBox result=" + std::to_wstring((long long)result) + L" target='" + options.target +
             L"' mask='" + options.mask + L"' overwrite=" + std::to_wstring(options.overwriteAll ? 1 : 0) +
             L" background=" + std::to_wstring(options.background ? 1 : 0));
    return result == 1;
}

namespace {

// ---------- просмотрщик ----------

struct ListerState {
    std::wstring path;
    std::vector<BYTE> raw;
    UINT codepage = CP_ACP;      // текущая кодировка
    bool hex = false;            // режим дампа
    bool markdown = false;       // разметка MD вместо сырого текста
    bool rich = false;           // удалось создать RichEdit
    std::vector<std::pair<std::wstring, std::wstring>> links;   // текст ссылки → URL
    std::vector<std::pair<int, std::wstring>> outline;          // заголовки Markdown: уровень, текст
    bool syntaxOn = true;        // подсветка синтаксиса включается/выключается клавишей C
    std::wstring findNeedle;     // последняя строка поиска
    HFONT font = nullptr;
    HWND edit = nullptr;
    HWND hint = nullptr;
};

// Автоопределение: BOM UTF-16 / UTF-8, иначе проверка на корректный UTF-8, иначе ANSI.
UINT DetectCodepage(const std::vector<BYTE>& raw) {
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) return 1200;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) return CP_UTF8;
    if (!raw.empty()) {
        int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)raw.data(), (int)raw.size(), nullptr, 0);
        if (wide > 0) return CP_UTF8;
    }
    return CP_ACP;
}

// Похоже ли на двоичный файл: много неуправляемых байтов в начале.
bool LooksBinary(const std::vector<BYTE>& raw) {
    const size_t sample = raw.size() < 4096 ? raw.size() : 4096;
    if (sample == 0) return false;
    size_t odd = 0;
    for (size_t i = 0; i < sample; ++i) {
        const BYTE b = raw[i];
        if (b == 0) return true;
        if (b < 9 || (b > 13 && b < 32)) ++odd;
    }
    return odd * 5 > sample;   // больше 20% странных байтов
}

std::wstring DecodeBytes(const std::vector<BYTE>& raw, UINT codepage) {
    if (raw.empty()) return L"(пустой файл)";

    const BYTE* data = raw.data();
    size_t size = raw.size();
    UINT cp = codepage;

    if (cp == 1200 && size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        data += 2;
        size -= 2;
    } else if (cp == CP_UTF8 && size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        size -= 3;
    }
    if (size == 0) return L"(пустой файл)";

    int need = MultiByteToWideChar(cp, 0, (LPCCH)data, (int)size, nullptr, 0);
    if (need <= 0) return L"(не читается в этой кодировке)";
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(cp, 0, (LPCCH)data, (int)size, out.data(), need);
    return out;
}

// Дамп в стиле hex-редактора: смещение, 16 байт, ASCII.
std::wstring HexDump(const std::vector<BYTE>& raw, size_t limit) {
    if (raw.empty()) return L"(пустой файл)";

    const size_t total = raw.size();
    const size_t n = total < limit ? total : limit;
    std::wstring out;
    out.reserve(n * 6);
    wchar_t line[160];

    for (size_t offset = 0; offset < n; offset += 16) {
        std::wstring hex, ascii;
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < n) {
                const BYTE b = raw[offset + i];
                swprintf(line, 160, L"%02X ", b);
                hex += line;
                ascii.push_back((b >= 32 && b < 127) ? (wchar_t)b : L'.');
            } else {
                hex += L"   ";
            }
        }
        swprintf(line, 160, L"%08llX  %ls %ls\r\n", (unsigned long long)offset, hex.c_str(), ascii.c_str());
        out += line;
    }
    if (total > n) {
        out += L"\r\nпоказаны первые " + std::to_wstring(n) + L" байт из " + std::to_wstring(total);
    }
    return out;
}

// Win32 EDIT и RichEdit понимают только CRLF: одиночные LF/CR склеивают строки
// (именно поэтому README с LF раньше показывался одной длинной строкой).
std::wstring NormalizeEol(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') {
            out += L"\r\n";
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (c == L'\n') {
            out += L"\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string RtfEscape(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) {
        switch (c) {
            case L'\\': out += "\\\\"; break;
            case L'{': out += "\\{"; break;
            case L'}': out += "\\}"; break;
            case L'\t': out += "\\tab "; break;
            case L'\r': break;
            case L'\n': out += "\\line "; break;
            default:
                if (c < 128) {
                    out.push_back((char)c);
                } else {
                    char buf[16]{};
                    snprintf(buf, sizeof(buf), "\\u%d?", (int)(short)c);
                    out += buf;
                }
                break;
        }
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>>* g_mdLinks = nullptr;  // текст → URL, заполняется при конвертации
std::vector<std::pair<int, std::wstring>>* g_mdOutline = nullptr;          // заголовки для оглавления

// Инлайновая разметка: `код`, **жирный**, *курсив*, ~~зачёркнутый~~, [текст](url).
// Ссылка отдаётся RTF-полем HYPERLINK: RichEdit сам помечает её как ссылку и
// присылает EN_LINK, поэтому клик по ней можно обработать.
std::string InlineRtf(const std::wstring& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == L'`') {
            const size_t end = s.find(L'`', i + 1);
            if (end != std::wstring::npos) {
                out += "\\f1\\highlight4 ";
                out += RtfEscape(s.substr(i + 1, end - i - 1));
                out += "\\highlight0\\f0 ";
                i = end + 1;
                continue;
            }
        }
        if (i + 1 < s.size() && s[i] == L'*' && s[i + 1] == L'*') {
            const size_t end = s.find(L"**", i + 2);
            if (end != std::wstring::npos) {
                out += "\\b ";
                out += InlineRtf(s.substr(i + 2, end - i - 2));
                out += "\\b0 ";
                i = end + 2;
                continue;
            }
        }
        if (i + 1 < s.size() && s[i] == L'~' && s[i + 1] == L'~') {
            const size_t end = s.find(L"~~", i + 2);
            if (end != std::wstring::npos) {
                out += "\\strike ";
                out += InlineRtf(s.substr(i + 2, end - i - 2));
                out += "\\strike0 ";
                i = end + 2;
                continue;
            }
        }
        if (s[i] == L'*') {
            const size_t end = s.find(L'*', i + 1);
            if (end != std::wstring::npos && end > i + 1) {
                out += "\\i ";
                out += InlineRtf(s.substr(i + 1, end - i - 1));
                out += "\\i0 ";
                i = end + 1;
                continue;
            }
        }
        if (s[i] == L'[') {
            const size_t close = s.find(L']', i + 1);
            if (close != std::wstring::npos && close + 1 < s.size() && s[close + 1] == L'(') {
                const size_t paren = s.find(L')', close + 2);
                if (paren != std::wstring::npos) {
                    const std::wstring text = s.substr(i + 1, close - i - 1);
                    const std::wstring url = s.substr(close + 2, paren - close - 2);
                    if (g_mdLinks) g_mdLinks->push_back({text, url});
                    out += "{\\field{\\*\\fldinst{HYPERLINK \"";
                    out += RtfEscape(url);
                    out += "\"}}{\\fldrslt{\\cf2\\ul ";
                    out += InlineRtf(text);
                    out += "}}}";
                    i = paren + 1;
                    continue;
                }
            }
        }
        out += RtfEscape(std::wstring(1, s[i]));
        ++i;
    }
    return out;
}

std::vector<std::wstring> SplitTableRow(const std::wstring& line) {
    std::wstring body = line;
    while (!body.empty() && (body.front() == L'|' || body.front() == L' ')) body.erase(body.begin());
    while (!body.empty() && (body.back() == L'|' || body.back() == L' ')) body.pop_back();

    std::vector<std::wstring> cells;
    std::wstring cur;
    for (wchar_t c : body) {
        if (c == L'|') {
            cells.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.push_back(cur);
    for (auto& cell : cells) {
        while (!cell.empty() && (cell.front() == L' ' || cell.front() == L'\t')) cell.erase(cell.begin());
        while (!cell.empty() && (cell.back() == L' ' || cell.back() == L'\t')) cell.pop_back();
    }
    return cells;
}

// Настоящая таблица средствами RTF: сетка с рамками, шапка жирная.
void EmitTable(std::string& out, const std::vector<std::vector<std::wstring>>& rows) {
    if (rows.empty()) return;

    size_t cols = 0;
    for (const auto& row : rows) cols = row.size() > cols ? row.size() : cols;
    if (cols == 0) return;

    std::vector<size_t> weight(cols, 4);
    for (const auto& row : rows) {
        for (size_t c = 0; c < row.size(); ++c) {
            const size_t len = row[c].size();
            if (len > weight[c]) weight[c] = len < 48 ? len : 48;
        }
    }
    size_t sum = 0;
    for (size_t w : weight) sum += w;

    std::vector<int> cellx(cols);
    int acc = 0;
    for (size_t c = 0; c < cols; ++c) {
        acc += (int)((long long)9000 * (long long)weight[c] / (long long)sum);
        cellx[c] = acc;
    }
    cellx[cols - 1] = 9000;

    out += "\\pard\\par ";
    for (size_t r = 0; r < rows.size(); ++r) {
        out += "\\trowd\\trgaph60\\trleft0\\trrh0";
        for (size_t c = 0; c < cols; ++c) {
            char buf[64]{};
            snprintf(buf, sizeof(buf), "\\clbrdrt\\brdrs\\brdrw6\\brdrcf3\\clbrdrb\\brdrs\\brdrw6\\brdrcf3"
                                       "\\clbrdrl\\brdrs\\brdrw6\\brdrcf3\\clbrdrr\\brdrs\\brdrw6\\brdrcf3"
                                       "\\cellx%d",
                     cellx[c]);
            out += buf;
        }
        out += "\\intbl ";
        for (size_t c = 0; c < cols; ++c) {
            if (r == 0) out += "\\b ";
            out += InlineRtf(c < rows[r].size() ? rows[r][c] : L"");
            if (r == 0) out += "\\b0 ";
            out += "\\cell ";
        }
        out += "\\row ";
    }
    out += "\\pard\\f0\\fs20 ";
}

std::string MarkdownToRtf(const std::wstring& md) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= md.size()) {
        const size_t e = md.find(L"\r\n", start);
        if (e == std::wstring::npos) {
            lines.push_back(md.substr(start));
            break;
        }
        lines.push_back(md.substr(start, e - start));
        start = e + 2;
    }

    std::string out;
    out += "{\\rtf1\\ansi\\ansicpg1251\\deff0";
    out += "{\\fonttbl{\\f0\\fswiss\\fcharset204 Segoe UI;}"
           "{\\f1\\fmodern\\fcharset204 Consolas;}"
           "{\\f2\\fswiss\\fcharset204 Segoe UI Semibold;}}";
    out += "{\\colortbl;\\red0\\green0\\blue0;\\red30\\green95\\blue168;"
           "\\red120\\green126\\blue134;\\red243\\green243\\blue243;"
           "\\red42\\green122\\blue42;}";
    out += "\\viewkind4\\uc1\\pard\\f0\\fs20\\sl264\\slmult1 ";

    auto is_table_row = [](const std::wstring& l) {
        size_t pipes = 0;
        for (wchar_t c : l) {
            if (c == L'|') ++pipes;
        }
        return pipes >= 2;
    };
    auto is_table_sep = [](const std::wstring& l) {
        if (l.find(L'-') == std::wstring::npos) return false;
        return l.find_first_not_of(L"|-: ") == std::wstring::npos;
    };

    const size_t kCodeLimit = 200;      // длинные строки кода обрезаем, чтобы не рвались переносом
    bool fence = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring line = lines[i];
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();

        if (line.rfind(L"```", 0) == 0) {
            fence = !fence;
            out += fence ? "\\pard\\f1\\fs18\\highlight4\\li0\\ri0 "
                         : "\\pard\\highlight0\\f0\\fs20 ";
            continue;
        }
        if (fence) {
            std::wstring code = line;
            if (code.size() > kCodeLimit) {
                code.resize(kCodeLimit);
                code += L"…";
            }
            out += "\\pard\\f1\\fs18\\highlight4\\li0\\ri0 ";
            out += RtfEscape(code);
            out += "\\par ";
            continue;
        }

        if (is_table_row(line)) {
            std::vector<std::vector<std::wstring>> rows;
            size_t j = i;
            while (j < lines.size() && is_table_row(lines[j])) {
                if (!is_table_sep(lines[j])) rows.push_back(SplitTableRow(lines[j]));
                ++j;
            }
            EmitTable(out, rows);
            i = j - 1;
            continue;
        }

        size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == L'#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < line.size() && line[hashes] == L' ') {
            int fs = 20;
            switch (hashes) {
                case 1: fs = 36; break;
                case 2: fs = 30; break;
                case 3: fs = 26; break;
                default: fs = 22; break;
            }
            if (g_mdOutline) g_mdOutline->emplace_back((int)hashes, line.substr(hashes + 1));
            char head[96]{};
            snprintf(head, sizeof(head), "\\pard\\sb240\\sa120\\f2\\b\\fs%d ", fs);
            out += head;
            out += InlineRtf(line.substr(hashes + 1));
            out += "\\b0\\f0\\fs20\\sb0\\sa0\\par ";
            continue;
        }

        const bool hr = line.size() >= 3 && line.find_first_not_of(line[0]) == std::wstring::npos &&
                        (line[0] == L'-' || line[0] == L'*' || line[0] == L'_');
        if (hr) {
            out += "\\pard\\brdrb\\brdrs\\brdrw10\\brdrcf3\\par ";
            continue;
        }

        size_t indent = 0;
        while (indent < line.size() && line[indent] == L' ') ++indent;
        const std::wstring rest = line.substr(indent);
        const bool bullet = rest.size() > 1 && (rest[0] == L'-' || rest[0] == L'*' || rest[0] == L'+') &&
                            rest[1] == L' ';
        size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= L'0' && rest[digits] <= L'9') ++digits;
        const bool ordered = digits > 0 && digits + 1 < rest.size() && rest[digits] == L'.' &&
                             rest[digits + 1] == L' ';
        if (bullet || ordered) {
            const int left = 360 + (int)(indent / 2) * 240;
            char head[96]{};
            snprintf(head, sizeof(head), "\\pard\\li%d\\fi-180 ", left);
            out += head;
            if (bullet) {
                out += "\\u8226?  ";
                out += InlineRtf(rest.substr(2));
            } else {
                out += InlineRtf(rest.substr(0, digits));
                out += ".  ";
                out += InlineRtf(rest.substr(digits + 2));
            }
            out += "\\par ";
            continue;
        }

        if (!rest.empty() && rest[0] == L'>') {
            out += "\\pard\\li360\\cf3 ";
            out += InlineRtf(rest.substr(1));
            out += "\\cf1\\li0\\par ";
            continue;
        }

        if (rest.empty()) {
            out += "\\pard\\par ";
            continue;
        }

        out += "\\pard ";
        out += InlineRtf(rest);
        out += "\\par ";
    }

    out += "}";
    return out;
}

struct RtfInput {
    const std::string* text;
    size_t pos;
};

DWORD CALLBACK RtfStreamIn(DWORD_PTR cookie, LPBYTE buff, LONG cb, LONG* pcb) {
    auto* in = reinterpret_cast<RtfInput*>(cookie);
    const size_t left = in->text->size() - in->pos;
    const size_t n = left < (size_t)cb ? left : (size_t)cb;
    if (n) memcpy(buff, in->text->data() + in->pos, n);
    in->pos += n;
    *pcb = (LONG)n;
    return 0;
}

void ShowMarkdown(HWND edit, const std::wstring& text,
                  std::vector<std::pair<std::wstring, std::wstring>>& links,
                  std::vector<std::pair<int, std::wstring>>& outline) {
    links.clear();
    outline.clear();
    g_mdLinks = &links;
    g_mdOutline = &outline;
    const std::string rtf = MarkdownToRtf(text);
    g_mdLinks = nullptr;
    g_mdOutline = nullptr;
    RtfInput in{&rtf, 0};
    EDITSTREAM es{};
    es.dwCookie = (DWORD_PTR)&in;
    es.pfnCallback = RtfStreamIn;
    SendMessageW(edit, EM_STREAMIN, SF_RTF, (LPARAM)&es);
}

// Снимаем подсветку со всего текста: фон возвращается к автоматическому.
void ClearHighlights(HWND edit) {
    CHARRANGE all{0, -1};
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&all);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BACKCOLOR;
    cf.dwEffects = CFE_AUTOBACKCOLOR;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

// Красим все вхождения строки поиска, а не только текущее.
void HighlightAll(HWND edit, const std::wstring& needle) {
    ClearHighlights(edit);
    if (needle.empty()) return;

    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BACKCOLOR;
    cf.crBackColor = RGB(255, 235, 130);
    cf.dwEffects = 0;

    FINDTEXTEXW ft{};
    ft.lpstrText = (LPWSTR)needle.c_str();
    ft.chrg.cpMin = 0;
    ft.chrg.cpMax = -1;

    int guard = 0;
    while (SendMessageW(edit, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft) >= 0) {
        SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        if (ft.chrgText.cpMax <= ft.chrg.cpMin) break;
        ft.chrg.cpMin = ft.chrgText.cpMax;
        ft.chrg.cpMax = -1;
        if (++guard > 5000) break;
    }
}

enum class SyntaxLang { None, Cpp, Python, Json, Ini };

SyntaxLang LangByExtension(const std::wstring& ext) {
    if (ext == L".cpp" || ext == L".cc" || ext == L".c" || ext == L".h" || ext == L".hpp" || ext == L".cxx")
        return SyntaxLang::Cpp;
    if (ext == L".py") return SyntaxLang::Python;
    if (ext == L".json") return SyntaxLang::Json;
    if (ext == L".ini" || ext == L".cfg" || ext == L".conf") return SyntaxLang::Ini;
    return SyntaxLang::None;
}

bool IsKeyword(SyntaxLang lang, const std::wstring& word) {
    static const wchar_t* cppWords[] = {
        L"alignas", L"alignof", L"auto", L"bool", L"break", L"case", L"catch", L"char", L"class", L"const",
        L"constexpr", L"continue", L"default", L"delete", L"do", L"double", L"else", L"enum", L"explicit",
        L"extern", L"false", L"float", L"for", L"friend", L"if", L"inline", L"int", L"long", L"namespace",
        L"new", L"noexcept", L"nullptr", L"operator", L"private", L"protected", L"public", L"return", L"short",
        L"signed", L"sizeof", L"static", L"struct", L"switch", L"template", L"this", L"throw", L"true", L"try",
        L"typedef", L"typename", L"union", L"unsigned", L"using", L"virtual", L"void", L"volatile", L"while"};
    static const wchar_t* pyWords[] = {
        L"and",    L"as",     L"assert", L"async",  L"await",  L"break",   L"class",  L"continue", L"def",
        L"del",    L"elif",   L"else",   L"except", L"False",  L"finally", L"for",    L"from",     L"global",
        L"if",     L"import", L"in",     L"is",     L"lambda", L"None",    L"nonlocal", L"not",    L"or",
        L"pass",   L"raise",  L"return", L"True",   L"try",    L"while",   L"with",   L"yield"};
    static const wchar_t* jsonWords[] = {L"true", L"false", L"null"};

    const wchar_t** list = nullptr;
    size_t count = 0;
    switch (lang) {
        case SyntaxLang::Cpp: list = cppWords; count = sizeof(cppWords) / sizeof(cppWords[0]); break;
        case SyntaxLang::Python: list = pyWords; count = sizeof(pyWords) / sizeof(pyWords[0]); break;
        case SyntaxLang::Json: list = jsonWords; count = sizeof(jsonWords) / sizeof(jsonWords[0]); break;
        default: return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (word == list[i]) return true;
    }
    return false;
}

bool IsBuiltin(SyntaxLang lang, const std::wstring& word) {
    if (lang != SyntaxLang::Python) return false;
    static const wchar_t* builtins[] = {
        L"abs",   L"all",     L"any",    L"bin",    L"bool",   L"bytes",  L"dict",   L"dir",    L"enumerate",
        L"eval",  L"float",   L"format", L"getattr", L"hasattr", L"hash",  L"id",     L"input",  L"int",
        L"isinstance", L"issubclass", L"iter", L"len", L"list", L"map",  L"max",    L"min",    L"next",
        L"object", L"open",   L"ord",    L"pow",    L"print",  L"range",  L"repr",   L"reversed", L"round",
        L"set",   L"sorted",  L"str",    L"sum",    L"super",  L"tuple",  L"type",   L"zip",
        L"ValueError", L"TypeError", L"KeyError", L"IndexError", L"Exception", L"ZeroDivisionError"};
    for (const wchar_t* b : builtins) {
        if (word == b) return true;
    }
    return false;
}

void PaintRange(HWND edit, long from, long to, COLORREF color) {
    if (to <= from) return;
    CHARRANGE range{from, to};
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    cf.dwEffects = 0;
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&range);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

// Простая раскраска: комментарии, строки, числа и ключевые слова.
void HighlightSyntax(HWND edit, const std::wstring& text, SyntaxLang lang) {
    if (lang == SyntaxLang::None || text.empty()) return;
    if (text.size() > 400 * 1024) return;             // очень большие файлы не раскрашиваем

    // Палитра как в Visual Studio: синие ключевые слова, тёмно-красные строки,
    // зелёные комментарии, бирюзовые числа и встроенные функции.
    const COLORREF kKeyword = RGB(0, 0, 255);
    const COLORREF kString = RGB(163, 21, 21);
    const COLORREF kComment = RGB(0, 128, 0);
    const COLORREF kNumber = RGB(9, 134, 88);
    const COLORREF kSection = RGB(38, 127, 153);
    const COLORREF kBuiltin = RGB(38, 127, 153);
    const COLORREF kJsonKey = RGB(4, 81, 165);     // ключ JSON — синий, как в VS Code

    // RichEdit считает абзац одним CR, а мы индексируем исходный текст с CRLF.
    // Без пересчёта раскраска уезжает на соседние слова (та же ловушка, что в diff).
    std::vector<long> cp(text.size() + 1, 0);
    {
        long shift = 0;
        for (size_t k = 0; k < text.size(); ++k) {
            cp[k] = (long)k - shift;
            if (text[k] == L'\r' && k + 1 < text.size() && text[k + 1] == L'\n') ++shift;
        }
        cp[text.size()] = (long)text.size() - shift;
    }

    const bool cLike = lang == SyntaxLang::Cpp;
    size_t i = 0;
    int guard = 0;

    while (i < text.size() && guard < 20000) {
        const wchar_t c = text[i];

        // комментарии
        if (cLike && c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
            size_t end = text.find(L'\n', i);
            if (end == std::wstring::npos) end = text.size();
            PaintRange(edit, cp[i], cp[end], kComment);
            ++guard;
            i = end;
            continue;
        }
        if (cLike && c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            size_t end = text.find(L"*/", i + 2);
            end = (end == std::wstring::npos) ? text.size() : end + 2;
            PaintRange(edit, cp[i], cp[end], kComment);
            ++guard;
            i = end;
            continue;
        }
        if (lang == SyntaxLang::Python && c == L'#') {
            size_t end = text.find(L'\n', i);
            if (end == std::wstring::npos) end = text.size();
            PaintRange(edit, cp[i], cp[end], kComment);
            ++guard;
            i = end;
            continue;
        }
        if (lang == SyntaxLang::Ini && (c == L';' || c == L'#')) {
            size_t end = text.find(L'\n', i);
            if (end == std::wstring::npos) end = text.size();
            PaintRange(edit, cp[i], cp[end], kComment);
            ++guard;
            i = end;
            continue;
        }

        // тройные кавычки Python (docstring)
        if (lang == SyntaxLang::Python && (c == L'"' || c == L'\'') && i + 2 < text.size() &&
            text[i + 1] == c && text[i + 2] == c) {
            const std::wstring closing = std::wstring(3, c);
            size_t end = text.find(closing, i + 3);
            end = (end == std::wstring::npos) ? text.size() : end + 3;
            PaintRange(edit, cp[i], cp[end], kString);
            ++guard;
            i = end;
            continue;
        }

        // строки
        if (c == L'"' || c == L'\'') {
            size_t j = i + 1;
            while (j < text.size() && text[j] != c) {
                if (text[j] == L'\\' && j + 1 < text.size()) ++j;
                if (text[j] == L'\n') break;
                ++j;
            }
            if (j < text.size() && text[j] == c) ++j;

            // В JSON строка перед двоеточием — это ключ, а не значение.
            bool isKey = false;
            if (lang == SyntaxLang::Json) {
                size_t k = j;
                while (k < text.size() && (text[k] == L' ' || text[k] == L'\t')) ++k;
                isKey = k < text.size() && text[k] == L':';
            }
            PaintRange(edit, cp[i], cp[j], isKey ? kJsonKey : kString);
            ++guard;
            i = j;
            continue;
        }

        // секция ini
        if (lang == SyntaxLang::Ini && c == L'[') {
            size_t end = text.find(L']', i);
            if (end != std::wstring::npos) {
                PaintRange(edit, cp[i], cp[end + 1], kSection);
                ++guard;
                i = end + 1;
                continue;
            }
        }

        // числа
        if (c >= L'0' && c <= L'9') {
            size_t j = i;
            while (j < text.size() && ((text[j] >= L'0' && text[j] <= L'9') || text[j] == L'.' ||
                                       text[j] == L'x' || (text[j] >= L'a' && text[j] <= L'f') ||
                                       (text[j] >= L'A' && text[j] <= L'F'))) {
                ++j;
            }
            PaintRange(edit, cp[i], cp[j], kNumber);
            ++guard;
            i = j;
            continue;
        }

        // слова
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_') {
            size_t j = i;
            while (j < text.size() && ((text[j] >= L'a' && text[j] <= L'z') || (text[j] >= L'A' && text[j] <= L'Z') ||
                                       (text[j] >= L'0' && text[j] <= L'9') || text[j] == L'_')) {
                ++j;
            }
            const std::wstring word = text.substr(i, j - i);
            if (IsKeyword(lang, word)) {
                PaintRange(edit, cp[i], cp[j], kKeyword);
            } else if (IsBuiltin(lang, word)) {
                PaintRange(edit, cp[i], cp[j], kBuiltin);
            }
            ++guard;
            i = j;
            continue;
        }

        ++i;
    }

    CHARRANGE none{0, 0};
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&none);
}

// ---- оглавление Markdown ----
struct OutlineState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    int chosen = -1;
    bool done = false;
};

OutlineState g_outline;

void LayoutOutline(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    MoveWindow(g_outline.list, 8, 8, rc.right - 16, rc.bottom - 16, TRUE);
}

LRESULT CALLBACK OutlineProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_outline.hwnd = hwnd;
            g_outline.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                             0, 0, 10, 10, hwnd, (HMENU)1, nullptr, nullptr);
            SendMessageW(g_outline.list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            LayoutOutline(hwnd);
            return 0;
        }
        case WM_SIZE:
            LayoutOutline(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == LBN_DBLCLK) {
                g_outline.chosen = (int)SendMessageW(g_outline.list, LB_GETCURSEL, 0, 0);
                g_outline.done = true;
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                g_outline.chosen = (int)SendMessageW(g_outline.list, LB_GETCURSEL, 0, 0);
                g_outline.done = true;
                return 0;
            }
            if (wp == VK_ESCAPE) {
                g_outline.done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            g_outline.done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void JumpToHeading(HWND parent, ListerState* st, const std::wstring& heading) {
    FINDTEXTEXW ft{};
    ft.lpstrText = (LPWSTR)heading.c_str();
    ft.chrg.cpMin = 0;
    ft.chrg.cpMax = -1;
    if (SendMessageW(st->edit, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft) < 0) return;

    SendMessageW(st->edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(st->edit, EM_SCROLLCARET, 0, 0);
    SetFocus(st->edit);
    (void)parent;
}

void ShowOutline(HWND parent, ListerState* st) {
    if (!st || st->outline.empty()) {
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OutlineProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"POSOutline2";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_outline = OutlineState{};

    RECT prc{};
    GetWindowRect(parent, &prc);
    const int w = 520, h = 420;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"POSOutline2", L"Оглавление",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, prc.left + 60, prc.top + 60, w, h, parent,
                                nullptr, g_inst, nullptr);
    if (!hwnd) return;

    for (const auto& kv : st->outline) {
        std::wstring line;
        for (int i = 1; i < kv.first; ++i) line += L"    ";
        line += kv.second;
        SendMessageW(g_outline.list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(g_outline.list);
    SendMessageW(g_outline.list, LB_SETCURSEL, 0, 0);

    MSG msg;
    while (!g_outline.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);

    if (g_outline.chosen >= 0 && g_outline.chosen < (int)st->outline.size()) {
        JumpToHeading(parent, st, st->outline[g_outline.chosen].second);
    }
}

std::wstring RangeText(HWND edit, CHARRANGE chrg) {
    const int len = chrg.cpMax - chrg.cpMin;
    if (len <= 0 || len > 4096) return {};
    std::wstring buf((size_t)len + 1, L'\0');
    TEXTRANGEW tr{};
    tr.chrg = chrg;
    tr.lpstrText = buf.data();
    SendMessageW(edit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

// Ctrl+F — спросить строку, Enter — искать дальше.
void FindInLister(HWND parent, ListerState* st, bool ask, bool backward = false) {
    if (!st || !st->edit) return;

    std::wstring needle = st->findNeedle;
    if (ask) {
        if (!InputBox(parent, L"Поиск по тексту", L"Найти:", needle, false)) return;
        st->findNeedle = needle;
    }
    if (needle.empty()) return;

    CHARRANGE sel{};
    SendMessageW(st->edit, EM_EXGETSEL, 0, (LPARAM)&sel);

    FINDTEXTEXW ft{};
    ft.lpstrText = (LPWSTR)needle.c_str();
    const WPARAM flags = backward ? 0 : FR_DOWN;
    if (backward) {
        ft.chrg.cpMin = 0;                 // ищем от начала до каретки
        ft.chrg.cpMax = sel.cpMin;
    } else {
        ft.chrg.cpMin = sel.cpMax;
        ft.chrg.cpMax = -1;
    }
    LRESULT found = SendMessageW(st->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    if (found < 0) {                       // дошли до края — продолжаем с другого конца
        if (backward) {
            ft.chrg.cpMin = sel.cpMax;
            ft.chrg.cpMax = -1;
        } else {
            ft.chrg.cpMin = 0;
            ft.chrg.cpMax = -1;
        }
        found = SendMessageW(st->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    }
    if (found < 0) {
        MessageBeep(MB_ICONINFORMATION);
        return;
    }
    const CHARRANGE hit = ft.chrgText;
    HighlightAll(st->edit, needle);
    SendMessageW(st->edit, EM_EXSETSEL, 0, (LPARAM)&hit);
    SendMessageW(st->edit, EM_SCROLLCARET, 0, 0);
}

void RefreshLister(HWND hwnd, ListerState* st) {
    if (!st || !st->edit) return;

    const size_t kTextLimit = 4u * 1024 * 1024;
    if (st->hex) {
        const std::wstring dump = HexDump(st->raw, 64u * 1024);
        SetWindowTextW(st->edit, dump.c_str());
    } else {
        std::wstring body = NormalizeEol(DecodeBytes(st->raw, st->codepage));
        if (body.size() > kTextLimit) {
            body.resize(kTextLimit);
            body += L"\r\n… файл обрезан";
        }
        if (st->markdown && st->rich) {
            ShowMarkdown(st->edit, body, st->links, st->outline);
        } else {
            SetWindowTextW(st->edit, body.c_str());
            const size_t dot = st->path.find_last_of(L'.');
            if (st->syntaxOn && dot != std::wstring::npos) {
                const SyntaxLang lang = LangByExtension(Lower(st->path.substr(dot)));
                HighlightSyntax(st->edit, body, lang);
            }
        }
    }
    SendMessageW(st->edit, EM_SETSEL, 0, 0);
    SendMessageW(st->edit, EM_SCROLLCARET, 0, 0);

    std::wstring name = st->path.substr(st->path.find_last_of(L'\\') + 1);
    std::wstring mode;
    if (st->hex) mode = L"hex";
    else if (st->markdown && st->rich) mode = L"markdown";
    else if (st->codepage == CP_UTF8) mode = L"UTF-8";
    else if (st->codepage == 1200) mode = L"UTF-16";
    else if (st->codepage == CP_OEMCP) mode = L"OEM";
    else mode = L"ANSI";

    SetWindowTextW(hwnd, (L"Просмотр — " + name + L" · " + mode).c_str());
    if (st->hint) {
        SetWindowTextW(st->hint,
                        L"1 ANSI · 2 OEM · 3 UTF-8 · 4 UTF-16 · T текст · M markdown · H hex · "
                        L"Ctrl+F поиск · F3 далее · G оглавление · C подсветка · F4 редактор · ESC закрыть");
    }
}

// Просмотрщик: ESC/F3/F10 закрывают окно, даже когда фокус в дочернем EDIT.
LRESULT CALLBACK ListerEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN) {
        HWND parent = GetParent(hwnd);
        auto* st = reinterpret_cast<ListerState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));

        if (wParam == VK_ESCAPE || wParam == VK_F10) {
            DestroyWindow(parent);
            return 0;
        }
        if (wParam == VK_F3) {
            const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (st && !st->findNeedle.empty()) FindInLister(parent, st, false, back);
            else if (st) FindInLister(parent, st, true, false);
            return 0;
        }
        if (wParam == VK_F4 && st) {
            // Открыть тот же файл в программе, связанной с расширением (обычно редактор).
            HINSTANCE hr = ShellExecuteW(parent, L"open", st->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)hr <= 32) MessageBeep(MB_ICONWARNING);
            return 0;
        }
        if (st && wParam == VK_RETURN) {
            FindInLister(parent, st, false);
            return 0;
        }
        if (st && wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            FindInLister(parent, st, true);
            return 0;
        }
        if (st && wParam == 'G') {
            ShowOutline(parent, st);
            return 0;
        }
        if (st && wParam == 'C') {
            st->syntaxOn = !st->syntaxOn;
            RefreshLister(parent, st);
            return 0;
        }
        if (st) {
            UINT cp = 0;
            bool switchCp = true;
            switch (wParam) {
                case '1': cp = CP_ACP; break;
                case '2': cp = CP_OEMCP; break;
                case '3': cp = CP_UTF8; break;
                case '4': cp = 1200; break;
                case 'H': st->hex = true; RefreshLister(parent, st); return 0;
                case 'T': st->hex = false; st->markdown = false; RefreshLister(parent, st); return 0;
                case 'M':
                    st->hex = false;
                    st->markdown = !st->markdown;
                    RefreshLister(parent, st);
                    return 0;
                default: switchCp = false; break;
            }
            if (switchCp) {
                st->hex = false;
                st->codepage = cp;
                RefreshLister(parent, st);
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ListerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<ListerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<ListerState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

            // RichEdit 4.1: нужен и для markdown, и для нормального переноса строк.
            st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                           ES_AUTOVSCROLL,
                                       0, 0, 10, 10, hwnd, (HMENU)1, g_inst, nullptr);
            st->rich = st->edit != nullptr;
            if (!st->edit) {
                st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                               ES_AUTOVSCROLL,
                                           0, 0, 10, 10, hwnd, (HMENU)1, g_inst, nullptr);
            }
            st->hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                       0, 0, 10, 10, hwnd, (HMENU)2, g_inst, nullptr);

            if (st->edit) {
                st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
                SendMessageW(st->edit, WM_SETFONT, (WPARAM)st->font, TRUE);
                if (st->hint) SendMessageW(st->hint, WM_SETFONT, (WPARAM)st->font, TRUE);
                SendMessageW(st->edit, EM_SETEVENTMASK, 0, ENM_LINK);
                SetWindowSubclass(st->edit, ListerEditProc, 1, 0);
                RefreshLister(hwnd, st);
            }
            return 0;
        }
        case WM_SIZE: {
            if (st && st->edit) {
                const int hintH = Scale(22, WindowDpi(hwnd));
                int w = LOWORD(lParam);
                int h = HIWORD(lParam);
                MoveWindow(st->edit, 0, 0, w, h - hintH, TRUE);
                if (st->hint) MoveWindow(st->hint, 0, h - hintH, w, hintH, TRUE);
            }
            return 0;
        }
        case WM_SETFOCUS:
            if (st && st->edit) SetFocus(st->edit);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_F10) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (wParam == VK_F3) {
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (st && !st->findNeedle.empty()) FindInLister(hwnd, st, false, back);
                else if (st) FindInLister(hwnd, st, true, false);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lParam);
            if (st && nm && nm->code == EN_LINK) {
                auto* link = reinterpret_cast<ENLINK*>(lParam);
                if (link->msg == WM_LBUTTONUP) {
                    const std::wstring text = Trim(RangeText(st->edit, link->chrg));
                    for (const auto& kv : st->links) {
                        if (kv.first == text) {
                            ShellExecuteW(hwnd, L"open", kv.second.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            break;
                        }
                    }
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) {
                RECT wr{};
                if (GetWindowRect(hwnd, &wr)) {
                    const std::wstring iniPath = SettingsFilePath();
                    wchar_t buf[32]{};
                    swprintf(buf, 32, L"%d", (int)wr.left);
                    WritePrivateProfileStringW(L"Lister", L"X", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)wr.top);
                    WritePrivateProfileStringW(L"Lister", L"Y", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.right - wr.left));
                    WritePrivateProfileStringW(L"Lister", L"W", buf, iniPath.c_str());
                    swprintf(buf, 32, L"%d", (int)(wr.bottom - wr.top));
                    WritePrivateProfileStringW(L"Lister", L"H", buf, iniPath.c_str());
                }
                if (st->font) DeleteObject(st->font);
                delete st;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>& out, std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = ErrText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const LONGLONG limit = 8LL * 1024 * 1024;
    DWORD toRead = (DWORD)(size.QuadPart > limit ? limit : size.QuadPart);
    out.resize(toRead);
    DWORD read = 0;
    BOOL ok = toRead == 0 ? TRUE : ReadFile(file, out.data(), toRead, &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        error = ErrText(GetLastError());
        return false;
    }
    out.resize(read);
    return true;
}

}  // namespace

// ---------- распаковка: каталог + пароль ----------

const int kIdUnpTarget = 201;
const int kIdUnpPass = 202;

struct UnpackDlgCtx {
    std::wstring target;
    std::wstring password;
};

INT_PTR CALLBACK UnpackDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<UnpackDlgCtx*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<UnpackDlgCtx*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
            SetDlgItemTextW(dlg, kIdUnpTarget, ctx->target.c_str());
            SetDlgItemTextW(dlg, kIdUnpPass, ctx->password.c_str());
            SHAutoComplete(GetDlgItem(dlg, kIdUnpTarget), SHACF_FILESYSTEM);
            SendDlgItemMessageW(dlg, kIdUnpTarget, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, kIdUnpTarget));
            return FALSE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDOK) {
                wchar_t buf[32768]{};
                GetDlgItemTextW(dlg, kIdUnpTarget, buf, 32768);
                if (ctx) {
                    ctx->target = buf;
                    wchar_t pass[512]{};
                    GetDlgItemTextW(dlg, kIdUnpPass, pass, 512);
                    ctx->password = pass;
                }
                EndDialog(dlg, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, 0);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

bool UnpackDialog(HWND owner, const std::wstring& archiveName, std::wstring& target, std::wstring& password) {
    const std::wstring title = L"Распаковка";
    const std::wstring prompt = L"Распаковать «" + archiveName + L"» в каталог:";

    DlgBuilder builder(title.c_str(), 380, 132);
    builder.Add(prompt.c_str(), 200, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 10, 8, 360, 18);
    builder.Add(L"", kIdUnpTarget, kClsEdit, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                10, 28, 360, 14);
    builder.Add(L"Пароль (если архив зашифрован):", 203, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 52, 360, 14);
    builder.Add(L"", kIdUnpPass, kClsEdit, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                10, 68, 360, 14);
    builder.Add(L"OK", IDOK, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 248, 100, 58, 18);
    builder.Add(L"Отмена", IDCANCEL, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 312, 100, 58, 18);

    UnpackDlgCtx ctx;
    ctx.target = target;
    ctx.password = password;
    INT_PTR result = DialogBoxIndirectParamW(g_inst, builder.Get(), owner, UnpackDlgProc, (LPARAM)&ctx);
    if (result == 1) {
        target = ctx.target;
        password = ctx.password;
        return true;
    }
    return false;
}

bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              bool isPath) {
    DlgBuilder builder(title.c_str(), 300, 78);
    builder.Add(prompt.c_str(), kIdPrompt, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 8, 280, 20);
    builder.Add(L"", kIdEdit, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 10, 30, 280, 14);
    builder.Add(L"OK", IDOK, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 168, 54, 58, 16);
    builder.Add(L"Отмена", IDCANCEL, kClsButton, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 232, 54, 58, 16);

    InputCtx ctx;
    ctx.title = title;
    ctx.prompt = prompt;
    ctx.value = value;
    ctx.isPath = isPath;

    LogDebug(L"InputBox title='" + title + L"' valueLen=" + std::to_wstring(value.size()) +
             L" isPath=" + std::to_wstring(isPath ? 1 : 0));
    INT_PTR result = DialogBoxIndirectParamW(g_inst, builder.Get(), owner, InputProc, (LPARAM)&ctx);
    LogDebug(L"InputBox result=" + std::to_wstring((long long)result));
    if (result == 1 && ctx.ok) {
        value = ctx.value;
        return true;
    }
    return false;
}

void ShowLister(HWND owner, const std::wstring& path) {
    std::vector<BYTE> raw;
    std::wstring error;
    if (!ReadWholeFile(path, raw, error)) {
        MessageBoxW(owner, (L"Не удалось открыть файл:\n" + path + L"\n\n" + error).c_str(), L"Просмотр",
                    MB_ICONERROR | MB_OK);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ListerProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"POSLister2";
        RegisterClassExW(&wc);
        registered = true;
    }

    static bool richLoaded = false;
    if (!richLoaded) {
        LoadLibraryW(L"msftedit.dll");
        richLoaded = true;
    }

    auto* st = new ListerState();
    st->path = path;
    st->raw = std::move(raw);
    st->codepage = DetectCodepage(st->raw);
    st->hex = LooksBinary(st->raw);

    std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size()
                                                                                 : path.find_last_of(L'.'));
    ext = Lower(ext);
    st->markdown = !st->hex && (ext == L".md" || ext == L".markdown" || ext == L".mdown");
    st->syntaxOn = GetSettings().syntaxHighlight;

    std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
    std::wstring title = L"Просмотр — " + name;

    RECT ownerRect{0, 0, 900, 640};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);
    int width = (ownerRect.right - ownerRect.left) - 80;
    int height = (ownerRect.bottom - ownerRect.top) - 80;
    if (width < 500) width = 800;
    if (height < 400) height = 600;

    // Свои координаты просмотрщик помнит отдельно от окна поиска.
    const std::wstring iniPath = SettingsFilePath();
    const int savedX = (int)GetPrivateProfileIntW(L"Lister", L"X", ownerRect.left + 40, iniPath.c_str());
    const int savedY = (int)GetPrivateProfileIntW(L"Lister", L"Y", ownerRect.top + 40, iniPath.c_str());
    const int savedW = (int)GetPrivateProfileIntW(L"Lister", L"W", width, iniPath.c_str());
    const int savedH = (int)GetPrivateProfileIntW(L"Lister", L"H", height, iniPath.c_str());
    const RECT frame = ClampWindowRect(RECT{savedX, savedY, savedX + savedW, savedY + savedH});
    width = frame.right - frame.left;
    height = frame.bottom - frame.top;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"POSLister2", title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, frame.left, frame.top,
                                width, height, nullptr, nullptr, g_inst, st);
    if (!hwnd) {
        delete st;
        return;
    }

    // Модальный режим: владелец отключается, поэтому ввод (в том числе ESC/F3/F10)
    // гарантированно попадает в просмотрщик, а не в список файлов.
    bool ownerEnabled = owner && IsWindow(owner) && IsWindowEnabled(owner);
    if (ownerEnabled) EnableWindow(owner, FALSE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (ownerEnabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        SetFocus(owner);
    }
}

// ---------- подключение к FTP ----------

namespace {

const int kIdFtpSite = 110;
const int kIdFtpHost = 111;
const int kIdFtpPort = 112;
const int kIdFtpUser = 113;
const int kIdFtpPass = 114;
const int kIdFtpPassive = 115;
const int kIdFtpAnon = 116;
const int kIdFtpSave = 117;

struct FtpCtx {
    FtpConnectOptions* options = nullptr;
    const std::vector<FtpSite>* sites = nullptr;
};

std::wstring FtpProfileName(const FtpSite& site) {
    if (!site.name.empty()) return site.name;
    return site.user.empty() ? site.host : (site.user + L"@" + site.host);
}

void FtpFill(HWND dlg, const FtpSite& site) {
    SetDlgItemTextW(dlg, kIdFtpHost, site.host.c_str());
    wchar_t port[16]{};
    swprintf(port, 16, L"%d", site.port > 0 ? site.port : 21);
    SetDlgItemTextW(dlg, kIdFtpPort, port);
    SetDlgItemTextW(dlg, kIdFtpUser, site.user.c_str());
    SetDlgItemTextW(dlg, kIdFtpPass, site.password.c_str());
    CheckDlgButton(dlg, kIdFtpPassive, site.passive ? BST_CHECKED : BST_UNCHECKED);
    const bool anon = site.user.empty() && !site.host.empty();
    CheckDlgButton(dlg, kIdFtpAnon, anon ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(dlg, kIdFtpUser), !anon);
    EnableWindow(GetDlgItem(dlg, kIdFtpPass), !anon);
}

bool FtpRead(HWND dlg, FtpSite& site, std::wstring& error) {
    wchar_t host[1024]{};
    wchar_t port[32]{};
    wchar_t user[512]{};
    wchar_t pass[512]{};
    wchar_t profile[512]{};
    GetDlgItemTextW(dlg, kIdFtpHost, host, 1024);
    GetDlgItemTextW(dlg, kIdFtpPort, port, 32);
    GetDlgItemTextW(dlg, kIdFtpUser, user, 512);
    GetDlgItemTextW(dlg, kIdFtpPass, pass, 512);
    GetDlgItemTextW(dlg, kIdFtpSite, profile, 512);

    FtpServerField field;
    if (!FtpParseServerField(host, field, error)) return false;

    site.host = field.host;
    if (field.port > 0) {
        site.port = field.port;
    } else {
        site.port = _wtoi(port);
        if (site.port <= 0 || site.port > 65535) site.port = 21;
    }

    const bool anon = IsDlgButtonChecked(dlg, kIdFtpAnon) == BST_CHECKED;
    if (anon) {
        site.user.clear();
        site.password.clear();
    } else {
        site.user = !field.user.empty() ? field.user : Trim(user);
        site.password = !field.password.empty() ? field.password : pass;
    }
    site.name = Trim(profile);
    if (site.name.empty()) site.name = site.user.empty() ? site.host : (site.user + L"@" + site.host);
    site.passive = IsDlgButtonChecked(dlg, kIdFtpPassive) == BST_CHECKED;
    if (field.hasRoot) site.root = field.root;
    return true;
}

INT_PTR CALLBACK FtpProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FtpCtx*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<FtpCtx*>(lParam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
            if (!ctx) return FALSE;

            if (ctx->sites) {
                for (size_t i = 0; i < ctx->sites->size(); ++i) {
                    const FtpSite& site = (*ctx->sites)[i];
                    std::wstring label = site.name;
                    if (label.empty()) label = site.user.empty() ? site.host : (site.user + L"@" + site.host);
                    SendDlgItemMessageW(dlg, kIdFtpSite, CB_ADDSTRING, 0, (LPARAM)label.c_str());
                }
            }

            if (ctx->options->site.host.empty() && ctx->sites && !ctx->sites->empty()) {
                FtpFill(dlg, (*ctx->sites)[0]);
                SendDlgItemMessageW(dlg, kIdFtpSite, CB_SETCURSEL, 0, 0);
            } else {
                FtpFill(dlg, ctx->options->site);
                // Если это уже сохранённый профиль — подсветить его в списке.
                bool matched = false;
                if (ctx->sites) {
                    for (size_t i = 0; i < ctx->sites->size(); ++i) {
                        const FtpSite& saved = (*ctx->sites)[i];
                        if (SamePath(saved.host, ctx->options->site.host) &&
                            saved.port == ctx->options->site.port &&
                            SamePath(saved.user, ctx->options->site.user)) {
                            SendDlgItemMessageW(dlg, kIdFtpSite, CB_SETCURSEL, i, 0);
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched) {
                    const std::wstring profile = FtpProfileName(ctx->options->site);
                    if (!profile.empty()) SetDlgItemTextW(dlg, kIdFtpSite, profile.c_str());
                }
            }
            SetFocus(GetDlgItem(dlg, kIdFtpHost));
            SendDlgItemMessageW(dlg, kIdFtpHost, EM_SETSEL, 0, -1);
            return FALSE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == kIdFtpSite && code == CBN_SELCHANGE) {
                const int sel = (int)SendDlgItemMessageW(dlg, kIdFtpSite, CB_GETCURSEL, 0, 0);
                if (ctx && ctx->sites && sel >= 0 && sel < (int)ctx->sites->size()) {
                    const FtpSite& saved = (*ctx->sites)[sel];
                    FtpFill(dlg, saved);
                    if (ctx->options) {
                        ctx->options->site.root = saved.root;
                        ctx->options->site.name = saved.name;
                    }
                }
                return TRUE;
            }
            if (id == kIdFtpAnon && code == BN_CLICKED) {
                const bool anon = IsDlgButtonChecked(dlg, kIdFtpAnon) == BST_CHECKED;
                EnableWindow(GetDlgItem(dlg, kIdFtpUser), !anon);
                EnableWindow(GetDlgItem(dlg, kIdFtpPass), !anon);
                return TRUE;
            }
            if (id == IDOK) {
                FtpSite site;
                if (ctx && ctx->options) site = ctx->options->site;
                std::wstring readError;
                if (!FtpRead(dlg, site, readError)) {
                    MessageBoxW(dlg, readError.c_str(), L"FTP", MB_ICONWARNING | MB_OK);
                    SetFocus(GetDlgItem(dlg, kIdFtpHost));
                    return TRUE;
                }
                if (ctx && ctx->options) {
                    if (site.root.empty()) site.root = L"/";
                    if (site.root[0] != L'/') site.root = L"/" + site.root;
                    ctx->options->site = site;
                    ctx->options->saveSite = IsDlgButtonChecked(dlg, kIdFtpSave) == BST_CHECKED;
                }
                EndDialog(dlg, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, 0);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool FtpConnectBox(HWND owner, FtpConnectOptions& options, const std::vector<FtpSite>& sites) {
    DlgBuilder builder(L"FTP: подключение", 380, 176);
    builder.Add(L"Профиль:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 8, 120, 10);
    builder.Add(L"", kIdFtpSite, 0x0085,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 10, 20, 360, 90);
    builder.Add(L"Сервер:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 44, 50, 10);
    builder.Add(L"", kIdFtpHost, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 62, 42, 210, 14);
    builder.Add(L"Порт:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 282, 44, 34, 10);
    builder.Add(L"", kIdFtpPort, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 318, 42, 52, 14);
    builder.Add(L"Пользователь:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 66, 80, 10);
    builder.Add(L"", kIdFtpUser, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 96, 64, 140, 14);
    builder.Add(L"Пароль:", 0, kClsStatic, WS_CHILD | WS_VISIBLE | SS_LEFT, 244, 66, 42, 10);
    builder.Add(L"", kIdFtpPass, kClsEdit,
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 290, 64, 80, 14);
    builder.Add(L"Пассивный режим", kIdFtpPassive, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 10, 88, 150, 12);
    builder.Add(L"Анонимно", kIdFtpAnon, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 170, 88, 100, 12);
    builder.Add(L"Сохранить профиль", kIdFtpSave, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 10, 106, 150, 12);
    builder.Add(L"Пароль пишется в INI в обфусцированном виде.", 0, kClsStatic,
                WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 126, 360, 10);
    builder.Add(L"OK", IDOK, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 250, 150, 58, 16);
    builder.Add(L"Отмена", IDCANCEL, kClsButton,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 312, 150, 58, 16);

    FtpCtx ctx;
    ctx.options = &options;
    ctx.sites = &sites;
    const INT_PTR result = DialogBoxIndirectParamW(g_inst, builder.Get(), owner, FtpProc, (LPARAM)&ctx);
    LogDebug(L"FtpConnectBox result=" + std::to_wstring((long long)result) + L" host='" + options.site.host +
             L"' user='" + options.site.user + L"' save=" + std::to_wstring(options.saveSite ? 1 : 0));
    return result == 1;
}

// ---------- окно прогресса FTP ----------

namespace {

const wchar_t* kFtpProgressClass = L"POSFtpProgress2";
const int kFtpProgTextId = 80;
const int kFtpProgPercentId = 81;
const int kFtpProgBarId = 82;
const int kFtpProgCancelId = 83;
const int kFtpProgTargetId = 84;
const int kFtpProgStatsId = 85;

struct FtpProgState {
    HWND hwnd = nullptr;
    HWND text = nullptr;
    HWND target = nullptr;
    HWND percent = nullptr;
    HWND stats = nullptr;
    HWND bar = nullptr;
    HWND cancel = nullptr;
    HWND owner = nullptr;
    bool ownerDisabled = false;
    bool cancelled = false;
    bool shown = false;
    ULONGLONG started = 0;
    ULONGLONG lastUi = 0;
    ULONGLONG lastPump = 0;
};

FtpProgState g_ftpProg;

void FtpProgLayout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 12, lineH = 18, barH = 16, btnW = 100, btnH = 26;
    MoveWindow(g_ftpProg.text, pad, pad, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_ftpProg.target, pad, pad + lineH + 2, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_ftpProg.percent, pad, pad + 2 * lineH + 8, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_ftpProg.stats, pad, pad + 3 * lineH + 8, rc.right - 2 * pad, lineH, TRUE);
    MoveWindow(g_ftpProg.bar, pad, pad + 4 * lineH + 12, rc.right - 2 * pad, barH, TRUE);
    MoveWindow(g_ftpProg.cancel, rc.right - pad - btnW, rc.bottom - pad - btnH, btnW, btnH, TRUE);
}

LRESULT CALLBACK FtpProgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_ftpProg.hwnd = hwnd;
            g_ftpProg.text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                             0, 0, 10, 10, hwnd, (HMENU)kFtpProgTextId, nullptr, nullptr);
            g_ftpProg.target = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                               0, 0, 10, 10, hwnd, (HMENU)kFtpProgTargetId, nullptr, nullptr);
            g_ftpProg.percent = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                0, 0, 10, 10, hwnd, (HMENU)kFtpProgPercentId, nullptr, nullptr);
            g_ftpProg.stats = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                              0, 0, 10, 10, hwnd, (HMENU)kFtpProgStatsId, nullptr, nullptr);
            g_ftpProg.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                            0, 0, 10, 10, hwnd, (HMENU)kFtpProgBarId, nullptr, nullptr);
            g_ftpProg.cancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                               0, 0, 10, 10, hwnd, (HMENU)kFtpProgCancelId, nullptr, nullptr);
            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            const HWND kids[] = {g_ftpProg.text, g_ftpProg.target, g_ftpProg.percent, g_ftpProg.stats,
                                 g_ftpProg.bar, g_ftpProg.cancel};
            for (HWND k : kids) SendMessageW(k, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(g_ftpProg.bar, PBM_SETRANGE32, 0, 1000);
            FtpProgLayout(hwnd);
            return 0;
        }
        case WM_SIZE:
            FtpProgLayout(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kFtpProgCancelId) {
                g_ftpProg.cancelled = true;
                EnableWindow(g_ftpProg.cancel, FALSE);
                SetWindowTextW(g_ftpProg.percent, L"Отмена…");
                return 0;
            }
            break;
        case WM_CLOSE:
            g_ftpProg.cancelled = true;
            return 0;
        case WM_DESTROY:
            g_ftpProg.hwnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void FtpProgPump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

void FtpProgressOpen(HWND owner, const std::wstring& title, const std::wstring& target) {
    if (g_ftpProg.hwnd) FtpProgressClose();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FtpProgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kFtpProgressClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    g_ftpProg.cancelled = false;
    g_ftpProg.shown = false;
    g_ftpProg.started = GetTickCount64();
    g_ftpProg.lastUi = 0;
    g_ftpProg.lastPump = 0;
    g_ftpProg.owner = owner;

    const int w = 480, h = 196;
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int x = orc.left + ((orc.right - orc.left) - w) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    g_ftpProg.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kFtpProgressClass, title.c_str(),
                                     WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h,
                                     owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_ftpProg.hwnd) return;
    EnableWindow(owner, FALSE);
    g_ftpProg.ownerDisabled = true;
    SetWindowTextW(g_ftpProg.text, L"Подготовка…");
    SetWindowTextW(g_ftpProg.target, target.empty() ? L"" : (L"Куда: " + target).c_str());
    SetWindowTextW(g_ftpProg.percent, L"");
    SetWindowTextW(g_ftpProg.stats, L"");
}

bool FtpProgressUpdate(const std::wstring& file, ULONGLONG done, ULONGLONG total,
                       int filesDone, ULONGLONG bytesDone) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_ftpProg.lastPump >= 40) {
        FtpProgPump();
        g_ftpProg.lastPump = now;
    }

    // Не дёргаем перерисовку на каждые 64 Кб — иначе передача сама себя тормозит.
    const bool force = (total > 0 && done >= total);
    if (g_ftpProg.hwnd && (force || now - g_ftpProg.lastUi >= 100)) {
        g_ftpProg.lastUi = now;
        if (!g_ftpProg.shown && now - g_ftpProg.started > 250) {
            ShowWindow(g_ftpProg.hwnd, SW_SHOW);
            UpdateWindow(g_ftpProg.hwnd);
            g_ftpProg.shown = true;
        }
        if (g_ftpProg.shown) {
            SetWindowTextW(g_ftpProg.text, file.c_str());
            UpdateWindow(g_ftpProg.text);
            std::wstring info;
            if (total > 0) {
                const ULONGLONG percent = done * 100 / total;
                SendMessageW(g_ftpProg.bar, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
                info = FmtSize(done) + L" из " + FmtSize(total) + L" (" + std::to_wstring(percent) + L"%)";
            } else {
                SendMessageW(g_ftpProg.bar, PBM_SETPOS, 0, 0);
                info = FmtSize(done);
            }
            SetWindowTextW(g_ftpProg.percent, info.c_str());
            UpdateWindow(g_ftpProg.percent);

            if (filesDone >= 0) {
                std::wstring stats = L"Готово файлов: " + std::to_wstring(filesDone);
                if (bytesDone > 0) stats += L" · всего: " + FmtSize(bytesDone);
                SetWindowTextW(g_ftpProg.stats, stats.c_str());
                UpdateWindow(g_ftpProg.stats);
            }
            InvalidateRect(g_ftpProg.bar, nullptr, TRUE);
            UpdateWindow(g_ftpProg.bar);
        }
    }
    return !g_ftpProg.cancelled;
}

void FtpProgressClose() {
    if (!g_ftpProg.hwnd) return;
    HWND hwnd = g_ftpProg.hwnd;
    g_ftpProg.hwnd = nullptr;
    DestroyWindow(hwnd);
    if (g_ftpProg.ownerDisabled && g_ftpProg.owner) {
        EnableWindow(g_ftpProg.owner, TRUE);
        SetActiveWindow(g_ftpProg.owner);
    }
    g_ftpProg.ownerDisabled = false;
    g_ftpProg.cancelled = false;
    g_ftpProg.shown = false;
}

// ---------- «О программе» ----------

// Клик по ссылке в TaskDialog — открываем сайт или почту.
HRESULT CALLBACK AboutLinkProc(HWND, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

void ShowAbout(HWND owner) {
    const std::wstring content =
        L"Двухпанельный файловый менеджер в духе Total Commander.\r\n"
        L"C++20, чистый Win32 API, без внешних зависимостей.\r\n\r\n"
        L"Разработчик: " POSFILES_COMPANY L"\r\n"
        L"Сайт: <a href=\"" POSFILES_SITE_URL L"\">" POSFILES_SITE L"</a>\r\n"
        L"Почта: <a href=\"mailto:" POSFILES_EMAIL L"\">" POSFILES_EMAIL L"</a>";

    const std::wstring build = std::wstring(L"Сборка ") + POSFILES_VERSION_FULL;

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.hInstance = g_inst;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ENABLE_HYPERLINKS |
                  TDF_USE_HICON_MAIN;   // иконка задана как HICON, а не как имя ресурса
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = L"О программе";
    cfg.hMainIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(101));
    cfg.pszMainInstruction = L"PosFiles " POSFILES_VERSION_FULL;
    cfg.pszContent = content.c_str();
    cfg.pszExpandedInformation = build.c_str();
    cfg.pszExpandedControlText = L"Подробнее о сборке";
    cfg.pszFooter = L"© 2026 " POSFILES_COMPANY;
    cfg.pfCallback = AboutLinkProc;

    int pressed = 0;
    const HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    {
        wchar_t hrbuf[64]{};
        swprintf(hrbuf, 64, L"about: hr=0x%08X", (unsigned)hr);
        LogDebug(hrbuf);
    }
    if (FAILED(hr)) {
        // Совсем старый comctl32: простой текст без ссылок.
        wchar_t text[512]{};
        swprintf(text, 512, L"PosFiles %ls\n\n%ls\n%ls\n%ls", POSFILES_VERSION_FULL, POSFILES_COMPANY,
                 POSFILES_SITE, POSFILES_EMAIL);
        MessageBoxW(owner, text, L"О программе", MB_ICONINFORMATION | MB_OK);
    }
}
