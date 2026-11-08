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

// ---------- просмотрщик ----------

struct ListerState {
    std::wstring path;
    std::wstring text;
    HFONT font = nullptr;
    HWND edit = nullptr;
};

std::wstring BytesToText(const std::vector<BYTE>& raw) {
    if (raw.empty()) return L"(пустой файл)";

    const BYTE* data = raw.data();
    size_t size = raw.size();
    UINT codepage = CP_ACP;

    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        size -= 3;
        codepage = CP_UTF8;
    } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        data += 2;
        size -= 2;
        codepage = 1200;  // UTF-16 LE
    } else {
        // Проверяем, что байты — корректный UTF-8; иначе системная кодировка.
        int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)data, (int)size, nullptr, 0);
        codepage = wide > 0 ? CP_UTF8 : CP_ACP;
    }

    int need = MultiByteToWideChar(codepage, 0, (LPCCH)data, (int)size, nullptr, 0);
    if (need <= 0) return L"(не текст)";
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(codepage, 0, (LPCCH)data, (int)size, out.data(), need);
    return out;
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
    BOOL ok = ReadFile(file, out.data(), toRead, &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        error = ErrText(GetLastError());
        return false;
    }
    out.resize(read);
    return true;
}

// Просмотрщик: ESC/F3/F10 закрывают окно, даже когда фокус в дочернем EDIT.
LRESULT CALLBACK ListerEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN && (wParam == VK_ESCAPE || wParam == VK_F3 || wParam == VK_F10)) {
        DestroyWindow(GetParent(hwnd));
        return 0;
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
            if (st->edit) {
                st->font = CreateFontW(-Scale(15, WindowDpi(hwnd)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
                SendMessageW(st->edit, WM_SETFONT, (WPARAM)st->font, TRUE);
                SetWindowTextW(st->edit, st->text.c_str());
                SetWindowSubclass(st->edit, ListerEditProc, 1, 0);
            }
            return 0;
        }
        case WM_SIZE: {
            if (st && st->edit) {
                MoveWindow(st->edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
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

}  // namespace

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

    LogDebug(L"InputBox title='" + title + L"' valueLen=" + std::to_wstring(value.size()) + L" isPath=" + std::to_wstring(isPath ? 1 : 0));
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
    st->text = BytesToText(raw);

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
