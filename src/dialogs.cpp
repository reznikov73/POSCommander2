// Реализация диалогов без .rc: шаблон собирается в памяти.
#include "dialogs.h"
#include "util.h"

#include <cstdio>

#include <commctrl.h>

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

void RefreshLister(HWND hwnd, ListerState* st) {
    if (!st || !st->edit) return;

    const size_t kTextLimit = 4u * 1024 * 1024;
    std::wstring body;
    if (st->hex) {
        body = HexDump(st->raw, 64u * 1024);
    } else {
        body = DecodeBytes(st->raw, st->codepage);
        if (body.size() > kTextLimit) {
            body.resize(kTextLimit);
            body += L"\r\n… файл обрезан";
        }
    }

    SetWindowTextW(st->edit, body.c_str());
    SendMessageW(st->edit, EM_SETSEL, 0, 0);
    SendMessageW(st->edit, EM_SCROLLCARET, 0, 0);

    std::wstring name = st->path.substr(st->path.find_last_of(L'\\') + 1);
    std::wstring mode;
    if (st->hex) mode = L"hex";
    else if (st->codepage == CP_UTF8) mode = L"UTF-8";
    else if (st->codepage == 1200) mode = L"UTF-16";
    else if (st->codepage == CP_OEMCP) mode = L"OEM";
    else mode = L"ANSI";

    SetWindowTextW(hwnd, (L"Просмотр — " + name + L" · " + mode).c_str());
    if (st->hint) {
        SetWindowTextW(st->hint, L"1 ANSI · 2 OEM · 3 UTF-8 · 4 UTF-16 · H hex · T текст · ESC закрыть");
    }
}

// Просмотрщик: ESC/F3/F10 закрывают окно, даже когда фокус в дочернем EDIT.
LRESULT CALLBACK ListerEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN) {
        HWND parent = GetParent(hwnd);
        auto* st = reinterpret_cast<ListerState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));

        if (wParam == VK_ESCAPE || wParam == VK_F3 || wParam == VK_F10) {
            DestroyWindow(parent);
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
                case 'H': st->hex = !st->hex; RefreshLister(parent, st); return 0;
                case 'T': st->hex = false; RefreshLister(parent, st); return 0;
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

            st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                           ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                       0, 0, 10, 10, hwnd, (HMENU)1, g_inst, nullptr);
            st->hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                       0, 0, 10, 10, hwnd, (HMENU)2, g_inst, nullptr);

            if (st->edit) {
                st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
                SendMessageW(st->edit, WM_SETFONT, (WPARAM)st->font, TRUE);
                if (st->hint) SendMessageW(st->hint, WM_SETFONT, (WPARAM)st->font, TRUE);
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
            if (wParam == VK_ESCAPE || wParam == VK_F3 || wParam == VK_F10) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) {
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

    auto* st = new ListerState();
    st->path = path;
    st->raw = std::move(raw);
    st->codepage = DetectCodepage(st->raw);
    st->hex = LooksBinary(st->raw);

    std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
    std::wstring title = L"Просмотр — " + name;

    RECT ownerRect{0, 0, 900, 640};
    if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);
    int width = (ownerRect.right - ownerRect.left) - 80;
    int height = (ownerRect.bottom - ownerRect.top) - 80;
    if (width < 500) width = 800;
    if (height < 400) height = 600;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"POSLister2", title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, ownerRect.left + 40, ownerRect.top + 40,
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
