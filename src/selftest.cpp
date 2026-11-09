#include "selftest.h"

#include "ftp.h"
#include "ops.h"
#include "util.h"

#include <winnetwk.h>

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

std::vector<char> ReadBytes(const std::wstring& path) {
    std::vector<char> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[65536];
    DWORD read = 0;
    while (ReadFile(h, buf, (DWORD)sizeof(buf), &read, nullptr) && read > 0) {
        out.insert(out.end(), buf, buf + read);
    }
    CloseHandle(h);
    return out;
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

    // Перемещение в отдельный подкаталог
    const std::wstring moved = base + L"\\moved";
    MakeDir(moved);
    const std::wstring toMove = dest + L"\\a.txt";
    WriteText(toMove, "move me");
    rep.Check(ShellCopyMove(nullptr, {toMove}, moved, true, true), L"ShellCopyMove: перемещение");
    rep.Check(!PathExists(toMove) && PathExists(moved + L"\\a.txt"), L"перемещение: файл на новом месте");

    rep.Check(DeleteViaShell(nullptr, {renamed}, true), L"DeleteViaShell: удаление");
    rep.Check(!PathExists(renamed), L"файл после удаления отсутствует");

    std::wstring deep = base;
    for (int i = 1; i <= 5; ++i) {
        deep += L"\\segment_" + std::to_wstring(i) + L"_padding_padding_padding_padding_padding";
        if (!MakeDir(WithLongPrefix(deep))) break;
    }
    rep.Check(deep.size() > 260 && PathExists(WithLongPrefix(deep)), L"длинный путь больше 260 символов",
              L"длина " + std::to_wstring(deep.size()));

    // --- сетевые пути ---
    // Разбор UNC-пути не должен зависеть от доступности сети.
    rep.Check(NormalizeDir(L"\\\\server\\share\\") == L"\\\\server\\share",
              L"NormalizeDir сохраняет UNC-путь", NormalizeDir(L"\\\\server\\share\\"));
    rep.Check(DriveRootOf(L"\\\\server\\share\\dir") == L"\\\\server\\share",
              L"DriveRootOf: корень общего ресурса", DriveRootOf(L"\\\\server\\share\\dir"));

    // --- переменные окружения в путях (как в TC: %TEMP%, %USERPROFILE%) ---
    {
        wchar_t buf[32768]{};
        ExpandEnvironmentStringsW(L"%TEMP%", buf, 32768);
        rep.Check(NormalizeDir(L"%TEMP%") == NormalizeDir(buf),
                  L"NormalizeDir раскрывает %TEMP%", NormalizeDir(L"%TEMP%"));
        ExpandEnvironmentStringsW(L"%SystemRoot%", buf, 32768);
        rep.Check(NormalizeDir(L"%SystemRoot%\\") == NormalizeDir(buf),
                  L"NormalizeDir раскрывает %SystemRoot% с обратной косой", NormalizeDir(L"%SystemRoot%\\"));
        // Неизвестную переменную ExpandEnvironmentStringsW оставляет текстом;
        // относительный путь затем достраивается до текущего каталога — это норма.
        const std::wstring unknown = NormalizeDir(L"%НЕТ_ТАКОЙ_ПЕРЕМЕННОЙ%");
        rep.Check(unknown.find(L"%НЕТ_ТАКОЙ_ПЕРЕМЕННОЙ%") != std::wstring::npos,
                  L"неизвестная переменная остаётся текстом пути", unknown);
    }

    // Чтение общего ресурса, если он доступен на этой машине.
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computerLen = MAX_COMPUTERNAME_LENGTH + 1;
    std::wstring share;
    if (GetComputerNameW(computer, &computerLen)) {
        const std::wstring candidate = std::wstring(L"\\\\") + computer + L"\\C$";
        DWORD probeErr = 0;
        std::vector<Entry> probe;
        if (ListDir(candidate, false, probe, probeErr)) share = candidate;
    }
    if (!share.empty()) {
        DWORD shareErr = 0;
        std::vector<Entry> items;
        const bool listed = ListDir(share, false, items, shareErr);
        rep.Check(listed, L"UNC: чтение общего ресурса",
                  share + L", записей " + std::to_wstring(items.size()) + L", код " + std::to_wstring(shareErr));
    } else {
        rep.Check(true, L"UNC: общий ресурс недоступен, проверка пропущена");
    }

    // --- FTP: чистые функции и разбор листинга (без сети) ---
    {
        rep.Check(IsFtpUrl(L"ftp://user@host:2121/dir/файл.txt"), L"FTP: URL распознан");
        rep.Check(!IsFtpUrl(L"E:\\work"), L"FTP: локальный путь не URL");

        FtpSite site;
        std::wstring path;
        rep.Check(FtpParseUrl(L"ftp://user@host:2121/dir/файл.txt", site, path), L"FTP: URL разобран");
        rep.Check(site.host == L"host" && site.port == 2121 && site.user == L"user" &&
                      path == L"/dir/файл.txt",
                  L"FTP: поля URL",
                  site.host + L" " + std::to_wstring(site.port) + L" " + site.user + L" " + path);

        const std::wstring made = FtpMakeUrl(site, path);
        rep.Check(made == L"ftp://user@host:2121/dir/файл.txt", L"FTP: URL собран", made);
        rep.Check(FtpJoinUrl(L"ftp://user@host:2121/dir", L"файл с пробелом.txt") ==
                      L"ftp://user@host:2121/dir/файл с пробелом.txt",
                  L"FTP: склейка URL");
        rep.Check(FtpParentUrl(L"ftp://user@host:2121/dir/sub") == L"ftp://user@host:2121/dir",
                  L"FTP: родительский URL");
        rep.Check(FtpParentUrl(L"ftp://user@host:2121/") == L"ftp://user@host:2121/",
                  L"FTP: родитель корня");
        rep.Check(FtpRootUrl(L"ftp://user@host:2121/dir/sub") == L"ftp://user@host:2121/",
                  L"FTP: корень URL");
        rep.Check(FtpLeafName(L"ftp://user@host:2121/dir/sub") == L"sub", L"FTP: имя записи URL");
        rep.Check(FtpLeafName(L"ftp://user@host:2121/") == L"host", L"FTP: имя корня — host");

        // Пароль из URL берётся для входа, но в собранный URL не попадает.
        FtpSite withPass;
        std::wstring passPath;
        rep.Check(FtpParseUrl(L"ftp://user:secret@host/dir", withPass, passPath) &&
                      withPass.user == L"user" && withPass.password == L"secret",
                  L"FTP: пароль из URL разобран");
        rep.Check(FtpMakeUrl(withPass, passPath).find(L"secret") == std::wstring::npos,
                  L"FTP: пароль не попадает в URL", FtpMakeUrl(withPass, passPath));

        // Поле «Сервер»: можно вставить как чистый адрес, так и готовую строку ftp://.
        FtpServerField serverField;
        std::wstring serverError;
        rep.Check(FtpParseServerField(L"194.37.254.233", serverField, serverError) &&
                      serverField.host == L"194.37.254.233" && serverField.port == 0,
                  L"FTP: поле сервера — чистый адрес", serverError);
        rep.Check(FtpParseServerField(L"ftp://admin@194.37.254.233/", serverField, serverError) &&
                      serverField.host == L"194.37.254.233" && serverField.port == 21 &&
                      serverField.user == L"admin",
                  L"FTP: поле сервера — готовая строка ftp://", serverError);
        rep.Check(!FtpParseServerField(L"ftps://194.37.254.233/", serverField, serverError) &&
                      serverError.find(L"FTPS") != std::wstring::npos,
                  L"FTP: ftps:// отклонён с понятным текстом", serverError);

        const std::wstring secret = L"пароль-123";
        const std::wstring obfuscated = FtpObfuscatePassword(secret);
        rep.Check(!obfuscated.empty() && obfuscated != secret, L"FTP: пароль обфусцирован");
        rep.Check(FtpDeobfuscatePassword(obfuscated) == secret, L"FTP: пароль восстановлен");
        rep.Check(FtpObfuscatePassword(L"").empty() && FtpDeobfuscatePassword(L"").empty(),
                  L"FTP: пустой пароль");

        FtpEntryInfo entry;
        bool skip = false;
        rep.Check(FtpParseListLine(L"type=file;size=1234;modify=20261109190633; тест.txt", entry, skip) &&
                      !skip && entry.name == L"тест.txt" && !entry.isDir && entry.size == 1234,
                  L"FTP: разбор MLSD (файл)");
        rep.Check(FtpParseListLine(L"-rw-r--r-- 1 owner group 1234 Nov  9 22:06 файл с пробелом.txt",
                                  entry, skip) &&
                      !skip && entry.name == L"файл с пробелом.txt" && entry.size == 1234,
                  L"FTP: разбор Unix LIST", entry.name);
        rep.Check(FtpParseListLine(L"11-09-26  10:06PM       <DIR>          папка", entry, skip) &&
                      !skip && entry.isDir && entry.name == L"папка",
                  L"FTP: разбор DOS LIST", entry.name);
        rep.Check(FtpParseListLine(L"type=cdir;modify=20261109190633; .", entry, skip) && skip,
                  L"FTP: MLSD служебная строка");
    }

    std::wstring report = L"POS Commander - самопроверка\r\n";
    report += L"каталог: " + base + L"\r\n\r\n";
    for (auto& line : rep.lines) report += line + L"\r\n";
    report += L"\r\nпровалено: " + std::to_wstring(rep.failed) + L" из " + std::to_wstring((int)rep.lines.size()) + L"\r\n";

    Emit(report, TempPath(L"posc2-selftest.txt"));
    RemoveTree(base);
    return rep.failed;
}

int RunFtpTest(const std::wstring& spec) {
    Report rep;
    const std::wstring base = TempPath(L"posc2-ftp-test");
    RemoveTree(base);
    MakeDir(base);

    std::wstring host, user, password;
    int port = 0;
    const size_t p1 = spec.find(L':');
    const size_t p2 = (p1 == std::wstring::npos) ? std::wstring::npos : spec.find(L':', p1 + 1);
    const size_t p3 = (p2 == std::wstring::npos) ? std::wstring::npos : spec.find(L':', p2 + 1);
    const bool parsed = (p1 != std::wstring::npos && p2 != std::wstring::npos && p3 != std::wstring::npos);
    if (parsed) {
        host = spec.substr(0, p1);
        port = _wtoi(spec.substr(p1 + 1, p2 - p1 - 1).c_str());
        user = spec.substr(p2 + 1, p3 - p2 - 1);
        password = spec.substr(p3 + 1);
    }

    auto finish = [&]() {
        std::wstring report = L"POS Commander - проверка FTP\r\n";
        report += L"сервер: " + host + L":" + std::to_wstring(port) + L", пользователь: " + user + L"\r\n";
        report += L"каталог: " + base + L"\r\n\r\n";
        for (auto& line : rep.lines) report += line + L"\r\n";
        report += L"\r\nпровалено: " + std::to_wstring(rep.failed) + L" из " +
                  std::to_wstring((int)rep.lines.size()) + L"\r\n";
        Emit(report, TempPath(L"posc2-ftp-test.txt"));
        RemoveTree(base);
        return rep.failed;
    };

    if (!parsed || host.empty() || port <= 0 || port > 65535) {
        rep.Check(false, L"FTP: параметры --ftp-test", L"ожидается host:port:user:pass");
        return finish();
    }

    FtpSite site;
    site.host = host;
    site.port = port;
    site.user = user;
    site.password = password;
    site.passive = true;

    std::unique_ptr<FtpConnection> conn;
    std::wstring err;
    bool ok = FtpConnect(site, conn, err);
    rep.Check(ok, L"FTP: подключение", err);
    if (!ok) return finish();

    std::vector<FtpEntryInfo> root;
    ok = FtpList(*conn, L"/", root, err);
    rep.Check(ok, L"FTP: список корня",
              err.empty() ? (L"записей " + std::to_wstring(root.size())) : err);

    auto findEntry = [](const std::vector<FtpEntryInfo>& list, const std::wstring& name) -> const FtpEntryInfo* {
        for (auto& e : list) {
            if (e.name == name) return &e;
        }
        return nullptr;
    };

    const std::wstring remoteDir = L"/posc2-тест-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                   std::to_wstring(GetTickCount64());
    rep.Check(FtpCreateDir(*conn, remoteDir, err), L"FTP: создание каталога", err);

    const std::wstring localFile = base + L"\\файл-тест.bin";
    std::string payload(256 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)((i * 37 + 11) & 0xFF);
    rep.Check(WriteText(localFile, payload), L"FTP: локальный файл создан",
              std::to_wstring(payload.size()) + L" байт");

    const std::wstring remoteFile = remoteDir + L"/файл-тест.bin";
    int uploadCalls = 0;
    ULONGLONG uploadDone = 0;
    auto uploadProgress = [&](ULONGLONG done, ULONGLONG total) {
        ++uploadCalls;
        uploadDone = done;
        (void)total;
        return true;
    };
    rep.Check(FtpUploadFile(*conn, localFile, remoteFile, uploadProgress, err), L"FTP: загрузка файла", err);
    rep.Check(uploadCalls > 0 && uploadDone == payload.size(), L"FTP: прогресс загрузки",
              std::to_wstring(uploadDone) + L" байт");

    std::vector<FtpEntryInfo> remote;
    rep.Check(FtpList(*conn, remoteDir, remote, err), L"FTP: список после загрузки", err);
    const FtpEntryInfo* uploaded = findEntry(remote, L"файл-тест.bin");
    rep.Check(uploaded && !uploaded->isDir && uploaded->size == payload.size(),
              L"FTP: имя и размер на сервере",
              uploaded ? (uploaded->name + L", " + std::to_wstring(uploaded->size)) : L"не найдено");

    ULONGLONG remoteDirSize = 0;
    std::wstring dirSizeError;
    const std::wstring remoteDirUrl = FtpMakeUrl(site, remoteDir);
    FtpRememberPassword(remoteDirUrl, password);
    const bool dirSizeOk = FtpDirectorySize(remoteDirUrl, remoteDirSize, dirSizeError);
    const ULONGLONG expectedDirSize = (ULONGLONG)payload.size();
    rep.Check(dirSizeOk && remoteDirSize == expectedDirSize,
              L"FTP: рекурсивный размер каталога",
              std::wstring(L"ok=") + (dirSizeOk ? L"1" : L"0") +
                  L", size=" + std::to_wstring(remoteDirSize) +
                  L", expected=" + std::to_wstring(expectedDirSize) +
                  (dirSizeError.empty() ? L"" : (L", " + dirSizeError)));

    const std::wstring downloaded = base + L"\\скачано.bin";
    int downloadCalls = 0;
    auto downloadProgress = [&](ULONGLONG done, ULONGLONG total) {
        ++downloadCalls;
        (void)done;
        (void)total;
        return true;
    };
    rep.Check(FtpDownloadFile(*conn, remoteFile, downloaded, downloadProgress, err), L"FTP: скачивание", err);
    const std::vector<char> got = ReadBytes(downloaded);
    rep.Check(got.size() == payload.size() && std::equal(payload.begin(), payload.end(), got.begin()),
              L"FTP: содержимое совпадает",
              std::to_wstring(got.size()) + L" байт" + (downloadCalls > 0 ? L", прогресс был" : L", прогресса не было"));

    const std::wstring renamed = remoteDir + L"/переименован.bin";
    rep.Check(FtpRename(*conn, remoteFile, renamed, err), L"FTP: переименование", err);
    remote.clear();
    rep.Check(FtpList(*conn, remoteDir, remote, err), L"FTP: список после переименования", err);
    rep.Check(findEntry(remote, L"переименован.bin") != nullptr && findEntry(remote, L"файл-тест.bin") == nullptr,
              L"FTP: новое имя есть, старое исчезло");

    rep.Check(FtpDeleteFile(*conn, renamed, err), L"FTP: удаление файла", err);
    remote.clear();
    rep.Check(FtpList(*conn, remoteDir, remote, err), L"FTP: список после удаления", err);
    rep.Check(findEntry(remote, L"переименован.bin") == nullptr, L"FTP: файл исчез из списка");

    rep.Check(FtpRemoveDir(*conn, remoteDir, err), L"FTP: удаление каталога", err);
    root.clear();
    rep.Check(FtpList(*conn, L"/", root, err), L"FTP: список корня после уборки", err);
    const std::wstring remoteLeaf = FtpLeafName(FtpMakeUrl(site, remoteDir));
    rep.Check(findEntry(root, remoteLeaf) == nullptr, L"FTP: временный каталог убран", remoteLeaf);

    FtpSite badSite = site;
    badSite.password += L"-неверный";
    std::unique_ptr<FtpConnection> bad;
    std::wstring badError;
    const bool badConnected = FtpConnect(badSite, bad, badError);
    rep.Check(!badConnected, L"FTP: неверный пароль отклонён");
    rep.Check(badError.find(badSite.password) == std::wstring::npos,
              L"FTP: пароль не попал в текст ошибки", badError);

    FtpDisconnect(conn);
    rep.Check(conn == nullptr, L"FTP: соединение закрыто");

    return finish();
}
