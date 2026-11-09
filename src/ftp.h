// FTP-клиент: свой протокол на Winsock, UTF-8. Без WinINet (он ANSI-only)
// и без внешних библиотек.
#pragma once

#include "common.h"

#include <functional>
#include <memory>

// Параметры подключения. Пароль в URL не попадает.
struct FtpSite {
    std::wstring name;                 // подпись сохранённого сайта
    std::wstring host;
    int port = 21;
    std::wstring user;
    std::wstring password;
    std::wstring root = L"/";         // каталог, в который входит панель
    bool passive = true;
};

// Разобранное поле «Сервер»: host, host:port, user@host или ftp://user@host[:port]/path.
struct FtpServerField {
    std::wstring host;
    int port = 0;                      // 0 — порт в поле не указан
    std::wstring user;
    std::wstring password;
    std::wstring root = L"/";
    bool hasRoot = false;              // путь был указан явно
};

bool FtpParseServerField(const std::wstring& text, FtpServerField& out, std::wstring& error);

// Запись удалённого каталога.
struct FtpEntryInfo {
    std::wstring name;
    bool isDir = false;
    ULONGLONG size = 0;
    FILETIME ft{};
    DWORD attr = 0;
};

// Открытое управляющее соединение. Копировать нельзя, только unique_ptr.
struct FtpSession;

class FtpConnection {
public:
    FtpConnection();
    ~FtpConnection();
    FtpConnection(const FtpConnection&) = delete;
    FtpConnection& operator=(const FtpConnection&) = delete;

private:
    std::unique_ptr<FtpSession> impl_;
    friend bool FtpConnect(const FtpSite&, std::unique_ptr<FtpConnection>&, std::wstring&);
    friend bool FtpList(FtpConnection&, const std::wstring&, std::vector<FtpEntryInfo>&, std::wstring&);
    friend bool FtpDownloadFile(FtpConnection&, const std::wstring&, const std::wstring&,
                                const std::function<bool(ULONGLONG, ULONGLONG)>&, std::wstring&);
    friend bool FtpUploadFile(FtpConnection&, const std::wstring&, const std::wstring&,
                              const std::function<bool(ULONGLONG, ULONGLONG)>&, std::wstring&);
    friend bool FtpCreateDir(FtpConnection&, const std::wstring&, std::wstring&);
    friend bool FtpDeleteFile(FtpConnection&, const std::wstring&, std::wstring&);
    friend bool FtpRemoveDir(FtpConnection&, const std::wstring&, std::wstring&);
    friend bool FtpRename(FtpConnection&, const std::wstring&, const std::wstring&, std::wstring&);
    friend bool FtpFileSize(FtpConnection&, const std::wstring&, ULONGLONG&, std::wstring&);
};

bool FtpConnect(const FtpSite& site, std::unique_ptr<FtpConnection>& out, std::wstring& error);
void FtpDisconnect(std::unique_ptr<FtpConnection>& conn);

// dir — абсолютный путь от корня сервера, всегда с ведущим '/'.
bool FtpList(FtpConnection& conn, const std::wstring& dir, std::vector<FtpEntryInfo>& out, std::wstring& error);
// progress возвращает false — отмена. total может быть 0, если сервер не сообщил размер.
bool FtpDownloadFile(FtpConnection& conn, const std::wstring& remotePath, const std::wstring& localPath,
                     const std::function<bool(ULONGLONG, ULONGLONG)>& progress, std::wstring& error);
bool FtpUploadFile(FtpConnection& conn, const std::wstring& localPath, const std::wstring& remotePath,
                   const std::function<bool(ULONGLONG, ULONGLONG)>& progress, std::wstring& error);
bool FtpCreateDir(FtpConnection& conn, const std::wstring& dir, std::wstring& error);
bool FtpDeleteFile(FtpConnection& conn, const std::wstring& file, std::wstring& error);
bool FtpRemoveDir(FtpConnection& conn, const std::wstring& dir, std::wstring& error);
bool FtpRename(FtpConnection& conn, const std::wstring& from, const std::wstring& to, std::wstring& error);
bool FtpFileSize(FtpConnection& conn, const std::wstring& file, ULONGLONG& size, std::wstring& error);

// --- FTP-URL (панель хранит URL без пароля) ---

bool IsFtpUrl(const std::wstring& path);
bool FtpParseUrl(const std::wstring& url, FtpSite& site, std::wstring& path);
std::wstring FtpMakeUrl(const FtpSite& site, const std::wstring& path);
std::wstring FtpJoinUrl(const std::wstring& baseUrl, const std::wstring& name);
std::wstring FtpParentUrl(const std::wstring& url);
std::wstring FtpRootUrl(const std::wstring& url);
// Подпись для вкладки/строки состояния: последний компонент или host.
std::wstring FtpLeafName(const std::wstring& url);

// Пароли в памяти: ключ user@host:port. В URL и в логах пароля нет.
void FtpRememberPassword(const std::wstring& url, const std::wstring& password);
bool FtpPasswordFor(const std::wstring& url, std::wstring& password);
void FtpForgetPassword(const std::wstring& url);

// Обратимая обфускация для INI (НЕ криптография, только чтобы не читать глазами).
std::wstring FtpObfuscatePassword(const std::wstring& password);
std::wstring FtpDeobfuscatePassword(const std::wstring& text);

// Разбор одной строки листинга (MLSD, Unix, DOS). skip — служебная строка.
bool FtpParseListLine(const std::wstring& line, FtpEntryInfo& out, bool& skip);
