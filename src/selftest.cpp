#include "selftest.h"

#include "ops.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring TempPath(const wchar_t* leaf) {
    wchar_t tmp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmp)) return leaf;
    return std::wstring(tmp) + leaf;
}

bool WriteText(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == bytes.size();
}

std::string ReadText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096]{};
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    return std::string(buf, read);
}

std::wstring WithLongPrefix(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    return L"\\\\?\\" + path;
}

// Полное удаление без корзины: тестовый мусор не должен попадать в корзину.
void RemoveTree(const std::wstring& dir) {
    std::wstring base = WithLongPrefix(dir);
    std::wstring pattern = base + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring child = base + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(base.c_str());
}

struct Report {
    std::vector<std::wstring> lines;
    int failed = 0;

    void Check(bool ok, const std::wstring& name, const std::wstring& detail = L"") {
        std::wstring line = (ok ? L"[PASS] " : L"[FAIL] ") + name;
        if (!detail.empty()) line += L" - " + detail;
        lines.push_back(line);
        if (!ok) ++failed;
    }
};

void Emit(const std::wstring& text, const std::wstring& file) {
    // файл — гарантированный канал
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            std::string utf8((size_t)need, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), need, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
        CloseHandle(h);
    }
    // консоль — если родитель её предоставил
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(out, L"\r\n", 2, &written, nullptr);
            WriteConsoleW(out, text.c_str(), (DWORD)text.size(), &written, nullptr);
        }
    }
}

}  // namespace

int RunSelfTest() {
    Report rep;
    const std::wstring base = TempPath(L"posc2-test");
    const std::wstring work = base + L"\\work";
    const std::wstring dest = base + L"\\dest";

    RemoveTree(base);
    rep.Check(!PathExists(base), L"старый тестовый каталог удалён", base);
    rep.Check(MakeDir(base), L"базовый каталог создан", base);
    rep.Check(MakeDir(work) && MakeDir(dest), L"каталоги work и dest созданы");

    const std::wstring a = work + L"\\a.txt";
    const std::wstring b = work + L"\\b.bin";
    const std::wstring hidden = work + L"\\скрытый.txt";
    rep.Check(WriteText(a, "hello selftest"), L"записан a.txt");
    rep.Check(WriteText(b, std::string(8192, 'x')), L"записан b.bin", L"8192 байт");
    rep.Check(WriteText(hidden, "hidden"), L"записан скрытый файл");
    SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN);

    std::vector<Entry> all, visible;
    DWORD err = 0;
    rep.Check(ListDir(work, true, all, err), L"ListDir со скрытыми", L"код " + std::to_wstring(err));
    rep.Check(all.size() == 3, L"со скрытыми видно 3 записи", L"получено " + std::to_wstring(all.size()));
    rep.Check(ListDir(work, false, visible, err), L"ListDir без скрытых");
    rep.Check(visible.size() == 2, L"без скрытых видно 2 записи", L"получено " + std::to_wstring(visible.size()));

    rep.Check(FmtSize(512).find(L"байт") != std::wstring::npos, L"FmtSize: байты", FmtSize(512));
    rep.Check(FmtSize(1024) == L"1,0 Кб", L"FmtSize: 1,0 Кб", FmtSize(1024));
    rep.Check(FmtSize(5ull * 1024 * 1024) == L"5,0 Мб", L"FmtSize: 5,0 Мб", FmtSize(5ull * 1024 * 1024));

    // Естественная сортировка: числа по значению, регистр не важен.
    rep.Check(CompareNatural(L"file_2.txt", L"file_10.txt") < 0, L"сортировка: file_2 < file_10");
    rep.Check(CompareNatural(L"file_10.txt", L"file_2.txt") > 0, L"сортировка: file_10 > file_2");
    rep.Check(CompareNatural(L"File_3.txt", L"file_3.txt") == 0, L"сортировка: регистр не важен");
    rep.Check(CompareNatural(L"a.txt", L"b.txt") < 0, L"сортировка: a < b");

    rep.Check(MatchMask(L"a.txt", L"*.txt"), L"MatchMask: *.txt совпадает");
    rep.Check(!MatchMask(L"a.bin", L"*.txt"), L"MatchMask: *.txt не совпадает");
    rep.Check(SplitMasks(L"*.txt;*.bin").size() == 2, L"SplitMasks: две маски");

    rep.Check(ShellCopyMove(nullptr, {a}, dest, false, true), L"ShellCopyMove: копирование");
    const std::wstring copy = dest + L"\\a.txt";
    rep.Check(ReadText(copy) == "hello selftest", L"копия совпадает по содержимому");

    DWORD renameErr = 0;
    const std::wstring renamed = dest + L"\\a-renamed.txt";
    rep.Check(RenamePath(copy, renamed, renameErr), L"RenamePath", L"код " + std::to_wstring(renameErr));
    rep.Check(PathExists(renamed), L"переименованный файл на месте");

    rep.Check(DeleteViaShell(nullptr, {renamed}, true), L"DeleteViaShell: удаление");
    rep.Check(!PathExists(renamed), L"файл после удаления отсутствует");

    std::wstring deep = base;
    for (int i = 1; i <= 5; ++i) {
        deep += L"\\segment_" + std::to_wstring(i) + L"_padding_padding_padding_padding_padding";
        if (!MakeDir(WithLongPrefix(deep))) break;
    }
    rep.Check(deep.size() > 260 && PathExists(WithLongPrefix(deep)), L"длинный путь больше 260 символов",
              L"длина " + std::to_wstring(deep.size()));

    std::wstring report = L"POSCommander 2 - самопроверка\r\n";
    report += L"каталог: " + base + L"\r\n\r\n";
    for (auto& line : rep.lines) report += line + L"\r\n";
    report += L"\r\nпровалено: " + std::to_wstring(rep.failed) + L" из " + std::to_wstring((int)rep.lines.size()) + L"\r\n";

    Emit(report, TempPath(L"posc2-selftest.txt"));
    RemoveTree(base);
    return rep.failed;
}
