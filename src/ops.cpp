#include "ops.h"

#include "util.h"

#include <shellapi.h>

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

}  // namespace

bool ShellCopyMove(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& target,
                   bool move, bool silent) {
    if (sources.empty()) return false;

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

bool MakeDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
