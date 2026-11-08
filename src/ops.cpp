#include "ops.h"

#include "util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace {

// Строка pFrom/pTo для SHFileOperation: список путей, каждый завершён \0, в конце ещё один \0.
std::wstring BuildPathList(const std::vector<std::wstring>& paths) {
    std::wstring out;
    for (auto& p : paths) {
        out += p;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

// ---------- SHFileOperation: резервный путь ----------

bool CopyMoveLegacy(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                    bool move, bool silent) {
    std::wstring from = BuildPathList(sources);
    std::wstring to;
    if (!target.empty()) {
        to = target;
        to.push_back(L'\0');
        to.push_back(L'\0');
    }

    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = target.empty() ? nullptr : to.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (silent) op.fFlags |= FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;

    int result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
}

bool DeleteLegacy(HWND owner, const std::vector<std::wstring>& items, bool silent) {
    std::wstring from = BuildPathList(items);

    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
    if (silent) op.fFlags |= FOF_SILENT | FOF_NOERRORUI;

    int result = SHFileOperationW(&op);
    return result == 0 && !op.fAnyOperationsAborted;
}

// ---------- IFileOperation: основной путь ----------

// Возвращает false, только если COM-операция недоступна (тогда есть смысл в резервном пути).
bool CopyMoveModern(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                    bool move, bool* performed) {
    *performed = false;

    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) || !op) {
        return false;
    }

    DWORD flags = FOF_NOCONFIRMMKDIR;
    if (owner) op->SetOwnerWindow(owner);
    op->SetOperationFlags(flags);

    IShellItem* dst = nullptr;
    HRESULT hrDst = SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&dst));
    LogDebug(L"IFileOperation dst='" + target + L"' hr=" + std::to_wstring((long long)hrDst));
    if (FAILED(hrDst) || !dst) {
        op->Release();
        return false;
    }

    bool queued = true;
    for (auto& s : sources) {
        IShellItem* src = nullptr;
        if (FAILED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_PPV_ARGS(&src))) || !src) {
            queued = false;
            continue;
        }
        HRESULT hr = move ? op->MoveItem(src, dst, nullptr, nullptr)
                          : op->CopyItem(src, dst, nullptr, nullptr);
        LogDebug(L"IFileOperation item='" + s + L"' hr=" + std::to_wstring((long long)hr));
        src->Release();
        if (FAILED(hr)) queued = false;
    }

    HRESULT hrPerform = queued ? op->PerformOperations() : E_FAIL;
    BOOL aborted = TRUE;
    op->GetAnyOperationsAborted(&aborted);

    LogDebug(L"IFileOperation perform hr=" + std::to_wstring((long long)hrPerform) +
             L" aborted=" + std::to_wstring(aborted ? 1 : 0));
    *performed = SUCCEEDED(hrPerform) && !aborted;
    dst->Release();
    op->Release();
    return true;
}

bool DeleteModern(HWND owner, const std::vector<std::wstring>& items, bool* performed) {
    *performed = false;

    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) || !op) {
        return false;
    }

    if (owner) op->SetOwnerWindow(owner);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION);

    bool queued = true;
    for (auto& path : items) {
        IShellItem* item = nullptr;
        HRESULT hrItem = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hrItem) || !item) {
            LogDebug(L"Delete item create '" + path + L"' hr=" + std::to_wstring((long long)hrItem));
            queued = false;
            continue;
        }
        HRESULT hr = op->DeleteItem(item, nullptr);
        LogDebug(L"Delete '" + path + L"' hr=" + std::to_wstring((long long)hr));
        item->Release();
        if (FAILED(hr)) queued = false;
    }

    HRESULT hrPerform = queued ? op->PerformOperations() : E_FAIL;
    BOOL aborted = TRUE;
    op->GetAnyOperationsAborted(&aborted);
    LogDebug(L"Delete perform hr=" + std::to_wstring((long long)hrPerform) + L" aborted=" + std::to_wstring(aborted ? 1 : 0));

    *performed = SUCCEEDED(hrPerform) && !aborted;
    op->Release();
    return true;
}

}  // namespace

bool ShellCopyMove(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                   bool move, bool silent) {
    if (sources.empty()) return false;

    if (!silent) {
        bool performed = false;
        if (CopyMoveModern(owner, sources, target, move, &performed)) return performed;
        // COM недоступен — работаем старым путём.
        return CopyMoveLegacy(owner, sources, target, move, false);
    }
    return CopyMoveLegacy(owner, sources, target, move, true);
}

bool RenamePath(const std::wstring& from, const std::wstring& to, DWORD& err) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
        err = 0;
        return true;
    }
    err = GetLastError();
    return false;
}

bool DeleteViaShell(HWND owner, const std::vector<std::wstring>& items, bool silent) {
    if (items.empty()) return false;

    if (!silent) {
        bool performed = false;
        if (DeleteModern(owner, items, &performed)) return performed;
        return DeleteLegacy(owner, items, false);
    }
    return DeleteLegacy(owner, items, true);
}

bool MakeDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
