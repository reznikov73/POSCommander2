// Общие заголовки и мелкие утилиты, нужные всем модулям.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>

extern HINSTANCE g_inst;

// Собственные сообщения приложения.
#define WM_APP_STATUS (WM_APP + 1)

// Масштабирование размеров под текущий DPI монитора.
inline int Scale(int value, UINT dpi) { return MulDiv(value, (int)dpi, 96); }

// COM-указатель с подсчётом ссылок: освобождает объект сам.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void reset() {
        if (p_) { p_->Release(); p_ = nullptr; }
    }

private:
    T* p_ = nullptr;
};
