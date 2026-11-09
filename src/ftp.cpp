// FTP-клиент на Winsock: управляющее соединение, PASV/PORT, MLSD/LIST,
// RETR/STOR, файловые команды. Всё в UTF-8; WinINet не используем — он ANSI-only.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "ftp.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>

namespace {

constexpr int kConnectTimeoutMs = 10000;
constexpr int kControlTimeoutMs = 15000;
constexpr int kDataTimeoutMs = 60000;
constexpr size_t kMaxLine = 1024 * 1024;
constexpr size_t kMaxListing = 64 * 1024 * 1024;

// ---------- строки и кодировки ----------

std::wstring TrimWs(const std::wstring& text) {
    size_t a = 0;
    size_t b = text.size();
    while (a < b && iswspace(text[a])) ++a;
    while (b > a && iswspace(text[b - 1])) --b;
    return text.substr(a, b - a);
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), need, nullptr, nullptr);
    return out;
}

bool Utf8ToWide(const std::string& text, std::wstring& out) {
    out.clear();
    if (text.empty()) return true;
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return false;
    out.assign((size_t)need, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), need) > 0;
}

std::string WideToAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    const int need = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(), out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring AnsiToWide(const std::string& text) {
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

std::wstring DecodeServerText(const std::string& text, bool utf8) {
    std::wstring out;
    if (utf8 && Utf8ToWide(text, out)) return out;
    return AnsiToWide(text);
}

// ---------- Winsock ----------

std::wstring WsaErrorText(int code) {
    wchar_t* buf = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, (DWORD)code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      (LPWSTR)&buf, 0, nullptr);
    std::wstring out;
    if (len && buf) out.assign(buf, len);
    if (buf) LocalFree(buf);
    out = TrimWs(out);
    if (out.empty()) out = L"код " + std::to_wstring(code);
    return out;
}

bool EnsureWinsock(std::wstring& error) {
    static std::once_flag once;
    static bool ok = false;
    static std::wstring reason;
    std::call_once(once, [] {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        ok = (rc == 0);
        if (!ok) reason = L"WSAStartup: " + WsaErrorText(rc);
    });
    if (!ok) error = reason;
    return ok;
}

void SetTimeout(SOCKET sock, int ms) {
    if (sock == INVALID_SOCKET) return;
    DWORD value = (DWORD)ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&value, sizeof(value));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&value, sizeof(value));
}

void CloseSocket(SOCKET& sock) {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
}

bool ConnectTcp(const std::wstring& host, int port, int timeoutMs, SOCKET& out, std::wstring& error) {
    out = INVALID_SOCKET;
    if (!EnsureWinsock(error)) return false;

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* list = nullptr;
    const std::wstring portText = std::to_wstring(port);
    const int rc = GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &list);
    if (rc != 0) {
        error = L"Не удалось разрешить адрес «" + host + L"»: " + WsaErrorText(rc);
        return false;
    }

    std::wstring lastError = L"Не удалось подключиться к " + host + L":" + portText;
    for (ADDRINFOW* p = list; p; p = p->ai_next) {
        SOCKET sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        int cr = connect(sock, p->ai_addr, (int)p->ai_addrlen);
        bool connected = (cr == 0);
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr{}, ex{};
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(sock, &wr);
            FD_SET(sock, &ex);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            const int sel = select(0, nullptr, &wr, &ex, &tv);
            if (sel > 0 && FD_ISSET(sock, &wr)) {
                int err = 0;
                int len = (int)sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                if (err == 0) connected = true;
                else lastError = WsaErrorText(err);
            } else if (sel == 0) {
                lastError = L"Таймаут подключения к " + host + L":" + portText;
            } else {
                lastError = WsaErrorText(WSAGetLastError());
            }
        }

        if (connected) {
            mode = 0;
            ioctlsocket(sock, FIONBIO, &mode);
            SetTimeout(sock, kControlTimeoutMs);
            out = sock;
            FreeAddrInfoW(list);
            return true;
        }
        CloseSocket(sock);
    }

    FreeAddrInfoW(list);
    error = lastError;
    return false;
}

bool SendAll(SOCKET sock, const std::string& data, std::wstring& error) {
    size_t offset = 0;
    while (offset < data.size()) {
        const int chunk = (int)std::min<size_t>(data.size() - offset, 256 * 1024);
        const int sent = send(sock, data.data() + offset, chunk, 0);
        if (sent == SOCKET_ERROR) {
            error = L"Ошибка отправки команды: " + WsaErrorText(WSAGetLastError());
            return false;
        }
        if (sent == 0) {
            error = L"Сервер закрыл соединение при отправке команды";
            return false;
        }
        offset += (size_t)sent;
    }
    return true;
}

std::wstring NumericPeerHost(SOCKET sock) {
    sockaddr_storage addr{};
    int len = (int)sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) != 0) return {};
    wchar_t host[NI_MAXHOST]{};
    if (GetNameInfoW((const SOCKADDR*)&addr, len, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return host;
}

// ---------- пароли ----------

std::mutex g_passwordMutex;
std::map<std::wstring, std::wstring> g_passwords;

std::wstring LowerAscii(std::wstring text) {
    for (auto& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
    }
    return text;
}

std::wstring SiteKey(const FtpSite& site) {
    return LowerAscii(site.user) + L"@" + LowerAscii(site.host) + L":" + std::to_wstring(site.port);
}

// ---------- разбор путей и URL ----------

std::wstring NormalizeRemotePath(const std::wstring& path) {
    std::wstring in = path;
    for (auto& ch : in) {
        if (ch == L'\\') ch = L'/';
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= in.size()) {
        const size_t slash = in.find(L'/', start);
        const std::wstring part = in.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    std::wstring out;
    for (auto& p : parts) out += L"/" + p;
    return out.empty() ? L"/" : out;
}

std::wstring PercentDecode(const std::wstring& text) {
    if (text.find(L'%') == std::wstring::npos) return text;
    std::string bytes;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == L'%' && i + 2 < text.size() && iswxdigit(text[i + 1]) && iswxdigit(text[i + 2])) {
            wchar_t hex[3] = {text[i + 1], text[i + 2], 0};
            bytes.push_back((char)wcstoul(hex, nullptr, 16));
            i += 3;
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != L'%') ++j;
        bytes += WideToUtf8(text.substr(i, j - i));
        i = j;
    }
    std::wstring out;
    if (!Utf8ToWide(bytes, out)) return text;
    return out;
}

}  // namespace

// ---------- FTP-URL ----------

bool IsFtpUrl(const std::wstring& path) {
    if (path.size() < 6) return false;
    std::wstring head = LowerAscii(path.substr(0, 6));
    return head == L"ftp://";
}

bool FtpParseUrl(const std::wstring& url, FtpSite& site, std::wstring& path) {
    site = FtpSite{};
    path = L"/";
    if (!IsFtpUrl(url)) return false;

    std::wstring rest = url.substr(6);
    size_t slash = rest.find(L'/');
    std::wstring authority = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    std::wstring rawPath = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    if (authority.empty()) return false;

    const size_t at = authority.rfind(L'@');
    if (at != std::wstring::npos) {
        std::wstring userInfo = authority.substr(0, at);
        const size_t colon = userInfo.find(L':');
        if (colon != std::wstring::npos) {
            site.password = PercentDecode(userInfo.substr(colon + 1));  // только для ввода вручную
            userInfo = userInfo.substr(0, colon);
        }
        site.user = PercentDecode(userInfo);
        authority = authority.substr(at + 1);
    }

    std::wstring host;
    int port = 21;
    if (!authority.empty() && authority[0] == L'[') {
        const size_t close = authority.find(L']');
        if (close == std::wstring::npos) return false;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == L':') {
            const std::wstring p = authority.substr(close + 2);
            if (!p.empty()) port = _wtoi(p.c_str());
        }
    } else {
        const size_t colon = authority.rfind(L':');
        if (colon != std::wstring::npos && authority.find(L':') == colon) {
            const std::wstring p = authority.substr(colon + 1);
            bool digits = !p.empty();
            for (auto ch : p) {
                if (!iswdigit(ch)) digits = false;
            }
            if (digits) {
                port = _wtoi(p.c_str());
                host = authority.substr(0, colon);
            } else {
                host = authority;
            }
        } else {
            host = authority;
        }
    }

    if (host.empty()) return false;
    if (port <= 0 || port > 65535) port = 21;
    site.host = host;
    site.port = port;
    site.passive = true;
    path = NormalizeRemotePath(PercentDecode(rawPath));
    return true;
}

bool FtpParseServerField(const std::wstring& text, FtpServerField& out, std::wstring& error) {
    out = FtpServerField{};
    error.clear();
    const std::wstring input = TrimWs(text);
    if (input.empty()) {
        error = L"Укажите адрес FTP-сервера.";
        return false;
    }

    const auto allDigits = [](const std::wstring& value) {
        if (value.empty()) return false;
        for (auto ch : value) {
            if (!iswdigit(ch)) return false;
        }
        return true;
    };

    const std::wstring head = LowerAscii(input.substr(0, std::min<size_t>(8, input.size())));
    if (head.rfind(L"ftps://", 0) == 0) {
        error = L"FTPS не поддерживается. Укажите ftp:// или просто адрес сервера.";
        return false;
    }
    if (IsFtpUrl(input)) {
        FtpSite site;
        std::wstring path;
        if (!FtpParseUrl(input, site, path)) {
            error = L"Не удалось разобрать FTP-адрес: " + input;
            return false;
        }
        out.host = site.host;
        out.port = site.port;
        out.user = site.user;
        out.password = site.password;
        out.root = path;
        out.hasRoot = (path != L"/");
        return true;
    }
    if (input.find(L"://") != std::wstring::npos) {
        error = L"Поддерживается только ftp:// (без TLS) или адрес вида host[:port].";
        return false;
    }

    // host[:port][/path], допускаем user[:pass]@host.
    std::wstring rest = input;
    const size_t slash = rest.find(L'/');
    if (slash != std::wstring::npos) {
        out.root = NormalizeRemotePath(rest.substr(slash));
        out.hasRoot = true;
        rest = rest.substr(0, slash);
    }
    const size_t at = rest.rfind(L'@');
    if (at != std::wstring::npos) {
        std::wstring userInfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const size_t colon = userInfo.find(L':');
        if (colon != std::wstring::npos) {
            out.password = PercentDecode(userInfo.substr(colon + 1));
            userInfo = userInfo.substr(0, colon);
        }
        out.user = PercentDecode(userInfo);
    }

    std::wstring host = rest;
    int port = 0;
    if (!host.empty() && host[0] == L'[') {
        const size_t close = host.find(L']');
        if (close == std::wstring::npos) {
            error = L"Не закрыта квадратная скобка в адресе: " + input;
            return false;
        }
        const std::wstring inside = host.substr(1, close - 1);
        const std::wstring tail = host.substr(close + 1);
        host = inside;
        if (!tail.empty()) {
            if (tail[0] != L':' || !allDigits(tail.substr(1))) {
                error = L"Не удалось разобрать порт в адресе: " + input;
                return false;
            }
            port = _wtoi(tail.substr(1).c_str());
        }
    } else {
        const size_t colon = host.rfind(L':');
        if (colon != std::wstring::npos && host.find(L':') == colon) {
            const std::wstring portText = host.substr(colon + 1);
            if (!portText.empty() && allDigits(portText)) {
                port = _wtoi(portText.c_str());
                host = host.substr(0, colon);
            }
        }
    }

    if (host.empty()) {
        error = L"Укажите адрес FTP-сервера.";
        return false;
    }
    if (port < 0 || port > 65535) {
        error = L"Порт должен быть в диапазоне 1…65535.";
        return false;
    }
    out.host = host;
    out.port = port;
    return true;
}

std::wstring FtpMakeUrl(const FtpSite& site, const std::wstring& path) {
    std::wstring url = L"ftp://";
    if (!site.user.empty()) url += site.user + L"@";
    if (site.host.find(L':') != std::wstring::npos && site.host.find(L'[') == std::wstring::npos) {
        url += L"[" + site.host + L"]";
    } else {
        url += site.host;
    }
    if (site.port != 21 && site.port > 0) url += L":" + std::to_wstring(site.port);
    url += NormalizeRemotePath(path);
    return url;
}

std::wstring FtpJoinUrl(const std::wstring& baseUrl, const std::wstring& name) {
    FtpSite site;
    std::wstring base;
    if (!FtpParseUrl(baseUrl, site, base)) return baseUrl;
    std::wstring path = base;
    if (!path.empty() && path.back() != L'/') path += L'/';
    path += name;
    return FtpMakeUrl(site, path);
}

std::wstring FtpParentUrl(const std::wstring& url) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return url;
    if (path == L"/" || path.empty()) return FtpMakeUrl(site, L"/");
    const size_t slash = path.find_last_of(L'/');
    const std::wstring parent = (slash == std::wstring::npos || slash == 0) ? L"/" : path.substr(0, slash);
    return FtpMakeUrl(site, parent);
}

std::wstring FtpRootUrl(const std::wstring& url) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return url;
    return FtpMakeUrl(site, L"/");
}

std::wstring FtpLeafName(const std::wstring& url) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return url;
    if (path == L"/" || path.empty()) return site.host;
    const size_t slash = path.find_last_of(L'/');
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

void FtpRememberPassword(const std::wstring& url, const std::wstring& password) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return;
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    g_passwords[SiteKey(site)] = password;
}

bool FtpPasswordFor(const std::wstring& url, std::wstring& password) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return false;
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    auto it = g_passwords.find(SiteKey(site));
    if (it == g_passwords.end()) return false;
    password = it->second;
    return true;
}

void FtpForgetPassword(const std::wstring& url) {
    FtpSite site;
    std::wstring path;
    if (!FtpParseUrl(url, site, path)) return;
    std::lock_guard<std::mutex> lock(g_passwordMutex);
    g_passwords.erase(SiteKey(site));
}

std::wstring FtpObfuscatePassword(const std::wstring& password) {
    if (password.empty()) return {};
    static const unsigned char key[] = {0x50, 0x4F, 0x53, 0x43, 0x32, 0x46, 0x54, 0x50};
    std::string bytes = WideToUtf8(password);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (char)(bytes[i] ^ key[i % sizeof(key)]);
    static const wchar_t* hex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (unsigned char ch : bytes) {
        out.push_back(hex[ch >> 4]);
        out.push_back(hex[ch & 15]);
    }
    return out;
}

std::wstring FtpDeobfuscatePassword(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() % 2 != 0) return {};
    static const unsigned char key[] = {0x50, 0x4F, 0x53, 0x43, 0x32, 0x46, 0x54, 0x50};
    std::string bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        if (!iswxdigit(text[i]) || !iswxdigit(text[i + 1])) return {};
        wchar_t hex[3] = {text[i], text[i + 1], 0};
        bytes.push_back((char)wcstoul(hex, nullptr, 16));
    }
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = (char)(bytes[i] ^ key[i % sizeof(key)]);
    std::wstring out;
    if (!Utf8ToWide(bytes, out)) return {};
    return out;
}

// ---------- разбор строк листинга ----------

namespace {

bool IsAllDigits(const std::wstring& text) {
    if (text.empty()) return false;
    for (auto ch : text) {
        if (!iswdigit(ch)) return false;
    }
    return true;
}

int MonthFromName(const std::wstring& name) {
    static const wchar_t* months[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                      L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    const std::wstring low = LowerAscii(name.substr(0, std::min<size_t>(3, name.size())));
    for (int i = 0; i < 12; ++i) {
        if (low == months[i]) return i + 1;
    }
    return 0;
}

bool LooksLikeUnixPerm(const std::wstring& token) {
    if (token.size() < 10) return false;
    const wchar_t first = token[0];
    if (first != L'-' && first != L'd' && first != L'l' && first != L'p' && first != L'c' &&
        first != L'b' && first != L's' && first != L'D') {
        return false;
    }
    for (int i = 1; i < 10; ++i) {
        const wchar_t ch = token[i];
        if (ch != L'r' && ch != L'w' && ch != L'x' && ch != L'-' && ch != L's' && ch != L't' &&
            ch != L'S' && ch != L'T') {
            return false;
        }
    }
    return true;
}

bool LooksLikeDateDot(const std::wstring& token, int& y, int& m, int& d) {
    // 2026-11-09
    if (token.size() != 10 || token[4] != L'-' || token[7] != L'-') return false;
    if (!IsAllDigits(token.substr(0, 4)) || !IsAllDigits(token.substr(5, 2)) ||
        !IsAllDigits(token.substr(8, 2))) {
        return false;
    }
    y = _wtoi(token.substr(0, 4).c_str());
    m = _wtoi(token.substr(5, 2).c_str());
    d = _wtoi(token.substr(8, 2).c_str());
    return true;
}

FILETIME LocalSystemTimeToFileTime(const SYSTEMTIME& local) {
    SYSTEMTIME utc{};
    SYSTEMTIME src = local;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &src, &utc)) utc = src;
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    return ft;
}

FILETIME UtcSystemTimeToFileTime(const SYSTEMTIME& utc) {
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    return ft;
}

void FillCommon(FtpEntryInfo& out) {
    out.attr = out.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!out.name.empty() && out.name[0] == L'.') out.attr |= FILE_ATTRIBUTE_HIDDEN;
}

bool ParseMlsd(const std::wstring& line, FtpEntryInfo& out, bool& skip) {
    const size_t space = line.find(L' ');
    if (space == std::wstring::npos) return false;
    const std::wstring facts = line.substr(0, space);
    if (facts.find(L'=') == std::wstring::npos) return false;

    bool isDir = false;
    bool isFile = false;
    bool cdir = false;
    std::wstring modify;
    std::wstring size;

    size_t start = 0;
    while (start <= facts.size()) {
        const size_t semi = facts.find(L';', start);
        const std::wstring item = facts.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
        const size_t eq = item.find(L'=');
        if (eq != std::wstring::npos) {
            const std::wstring key = LowerAscii(item.substr(0, eq));
            const std::wstring value = item.substr(eq + 1);
            if (key == L"type") {
                if (value == L"dir") isDir = true;
                else if (value == L"file") isFile = true;
                else if (value == L"cdir" || value == L"pdir") cdir = true;
            } else if (key == L"size") {
                size = value;
            } else if (key == L"modify") {
                modify = value;
            }
        }
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }

    std::wstring name = TrimWs(line.substr(space + 1));
    if (name.empty()) return false;
    if (cdir) {
        skip = true;
        return true;
    }

    out = FtpEntryInfo{};
    out.name = name;
    out.isDir = isDir && !isFile;
    if (!size.empty() && IsAllDigits(size)) out.size = wcstoull(size.c_str(), nullptr, 10);

    if (modify.size() >= 14 && IsAllDigits(modify.substr(0, 14))) {
        SYSTEMTIME st{};
        st.wYear = (WORD)_wtoi(modify.substr(0, 4).c_str());
        st.wMonth = (WORD)_wtoi(modify.substr(4, 2).c_str());
        st.wDay = (WORD)_wtoi(modify.substr(6, 2).c_str());
        st.wHour = (WORD)_wtoi(modify.substr(8, 2).c_str());
        st.wMinute = (WORD)_wtoi(modify.substr(10, 2).c_str());
        st.wSecond = (WORD)_wtoi(modify.substr(12, 2).c_str());
        out.ft = UtcSystemTimeToFileTime(st);
    }
    FillCommon(out);
    return true;
}

bool ParseUnix(const std::wstring& line, FtpEntryInfo& out, bool& skip) {
    std::vector<std::wstring> tokens;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && line[start] == L' ') ++start;
        if (start >= line.size()) break;
        size_t end = line.find(L' ', start);
        if (end == std::wstring::npos) end = line.size();
        tokens.push_back(line.substr(start, end - start));
        start = end;
    }
    if (tokens.size() < 6 || !LooksLikeUnixPerm(tokens[0])) return false;

    // Ищем токен размера: число, за которым идёт месяц или дата.
    size_t sizeIndex = 0;
    for (size_t i = 1; i + 1 < tokens.size(); ++i) {
        if (!IsAllDigits(tokens[i])) continue;
        const bool monthNext = MonthFromName(tokens[i + 1]) != 0;
        int y = 0, m = 0, d = 0;
        const bool dateNext = LooksLikeDateDot(tokens[i + 1], y, m, d);
        if (monthNext || dateNext) {
            sizeIndex = i;
            break;
        }
    }
    if (sizeIndex == 0 || sizeIndex + 3 >= tokens.size()) return false;

    // Позиция имени: после трёх токенов даты.
    size_t namePos = 0;
    size_t seen = 0;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && line[pos] == L' ') ++pos;
        if (pos >= line.size()) break;
        const size_t end = line.find(L' ', pos);
        const size_t tokenEnd = (end == std::wstring::npos) ? line.size() : end;
        if (seen == sizeIndex + 3) {
            namePos = tokenEnd;
            while (namePos < line.size() && line[namePos] == L' ') ++namePos;
            break;
        }
        ++seen;
        pos = tokenEnd;
    }
    if (namePos == 0 || namePos >= line.size()) return false;
    std::wstring name = line.substr(namePos);
    const size_t arrow = name.find(L" -> ");
    if (arrow != std::wstring::npos) name = name.substr(0, arrow);
    name = TrimWs(name);
    if (name.empty()) return false;

    out = FtpEntryInfo{};
    out.name = name;
    out.isDir = (tokens[0][0] == L'd');
    out.size = wcstoull(tokens[sizeIndex].c_str(), nullptr, 10);

    SYSTEMTIME st{};
    bool haveDate = false;
    const int mon = MonthFromName(tokens[sizeIndex + 1]);
    if (mon > 0) {
        st.wMonth = (WORD)mon;
        st.wDay = (WORD)_wtoi(tokens[sizeIndex + 2].c_str());
        const std::wstring third = tokens[sizeIndex + 3];
        if (third.find(L':') != std::wstring::npos) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            st.wYear = now.wYear;
            st.wHour = (WORD)_wtoi(third.substr(0, third.find(L':')).c_str());
            st.wMinute = (WORD)_wtoi(third.substr(third.find(L':') + 1).c_str());
        } else if (IsAllDigits(third)) {
            st.wYear = (WORD)_wtoi(third.c_str());
        }
        haveDate = st.wYear != 0;
    }
    if (haveDate) out.ft = LocalSystemTimeToFileTime(st);
    FillCommon(out);
    return true;
}

bool ParseDos(const std::wstring& line, FtpEntryInfo& out, bool& skip) {
    // 11-09-26  10:06PM       <DIR>          имя
    std::wstring text = TrimWs(line);
    if (text.size() < 18) return false;
    const size_t firstSpace = text.find(L' ');
    if (firstSpace == std::wstring::npos) return false;
    const std::wstring date = text.substr(0, firstSpace);
    const size_t dash1 = date.find(L'-');
    const size_t dash2 = (dash1 == std::wstring::npos) ? std::wstring::npos : date.find(L'-', dash1 + 1);
    if (dash1 == std::wstring::npos || dash2 == std::wstring::npos) return false;
    if (!IsAllDigits(date.substr(0, dash1)) || !IsAllDigits(date.substr(dash1 + 1, dash2 - dash1 - 1)) ||
        !IsAllDigits(date.substr(dash2 + 1))) {
        return false;
    }

    size_t pos = firstSpace;
    while (pos < text.size() && text[pos] == L' ') ++pos;
    const size_t timeEnd = text.find(L' ', pos);
    if (timeEnd == std::wstring::npos) return false;
    const std::wstring time = text.substr(pos, timeEnd - pos);
    pos = timeEnd;
    while (pos < text.size() && text[pos] == L' ') ++pos;

    bool isDir = false;
    ULONGLONG size = 0;
    if (text.compare(pos, 5, L"<DIR>") == 0) {
        isDir = true;
        pos += 5;
    } else {
        const size_t sizeEnd = text.find(L' ', pos);
        if (sizeEnd == std::wstring::npos) return false;
        const std::wstring sizeText = text.substr(pos, sizeEnd - pos);
        if (!IsAllDigits(sizeText)) return false;
        size = wcstoull(sizeText.c_str(), nullptr, 10);
        pos = sizeEnd;
    }
    while (pos < text.size() && text[pos] == L' ') ++pos;
    if (pos >= text.size()) return false;

    const std::wstring name = TrimWs(text.substr(pos));
    if (name.empty()) return false;

    SYSTEMTIME st{};
    st.wMonth = (WORD)_wtoi(date.substr(0, dash1).c_str());
    st.wDay = (WORD)_wtoi(date.substr(dash1 + 1, dash2 - dash1 - 1).c_str());
    const std::wstring yearText = date.substr(dash2 + 1);
    int year = _wtoi(yearText.c_str());
    if (year < 100) year += (year >= 70) ? 1900 : 2000;
    st.wYear = (WORD)year;

    std::wstring t = time;
    bool pm = false;
    bool am = false;
    if (t.size() >= 2) {
        const std::wstring tail = LowerAscii(t.substr(t.size() - 2));
        if (tail == L"pm") {
            pm = true;
            t = t.substr(0, t.size() - 2);
        } else if (tail == L"am") {
            am = true;
            t = t.substr(0, t.size() - 2);
        }
    }
    const size_t colon = t.find(L':');
    if (colon == std::wstring::npos) return false;
    int hour = _wtoi(t.substr(0, colon).c_str());
    int minute = _wtoi(t.substr(colon + 1).c_str());
    if (pm && hour < 12) hour += 12;
    if (am && hour == 12) hour = 0;
    st.wHour = (WORD)hour;
    st.wMinute = (WORD)minute;

    out = FtpEntryInfo{};
    out.name = name;
    out.isDir = isDir;
    out.size = size;
    out.ft = LocalSystemTimeToFileTime(st);
    FillCommon(out);
    return true;
}

}  // namespace

bool FtpParseListLine(const std::wstring& line, FtpEntryInfo& out, bool& skip) {
    out = FtpEntryInfo{};
    skip = false;
    std::wstring text = line;
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    text = TrimWs(text);
    if (text.empty()) {
        skip = true;
        return true;
    }
    if (text.rfind(L"total ", 0) == 0) {
        skip = true;
        return true;
    }

    // MLSD: "type=file;size=123;modify=20261109190633; имя"
    bool parsed = false;
    if (ParseMlsd(text, out, skip)) parsed = true;
    else if (ParseUnix(text, out, skip)) parsed = true;
    else if (ParseDos(text, out, skip)) parsed = true;
    if (!parsed) return false;

    // Служебные строки текущего/родительского каталога в LIST -a.
    if (!skip && (out.name == L"." || out.name == L"..")) skip = true;
    return true;
}

// ---------- протокол ----------

struct FtpSession {
    SOCKET ctrl = INVALID_SOCKET;
    std::string buffer;
    bool utf8 = true;
    bool mlsd = false;
    bool passive = true;
    // PASV часто отдаёт внутренний IP (10.x/192.168.x) — по умолчанию берём
    // адрес управляющего соединения, иначе каждая команда ждёт таймаут.
    bool trustPasvIp = false;
    FtpSite site;
    std::wstring peerHost;

    ~FtpSession() { CloseSocket(ctrl); }
};

struct FtpReply {
    int code = 0;
    std::wstring text;
};

namespace {

bool ReadLine(FtpSession& s, std::wstring& line, std::wstring& error) {
    for (;;) {
        const size_t pos = s.buffer.find("\r\n");
        if (pos != std::string::npos) {
            const std::string raw = s.buffer.substr(0, pos);
            s.buffer.erase(0, pos + 2);
            line = DecodeServerText(raw, s.utf8);
            return true;
        }
        if (s.buffer.size() > kMaxLine) {
            error = L"Слишком длинная строка ответа FTP-сервера";
            return false;
        }
        char buf[4096];
        const int got = recv(s.ctrl, buf, (int)sizeof(buf), 0);
        if (got == 0) {
            error = L"FTP-сервер закрыл соединение";
            return false;
        }
        if (got == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            error = (code == WSAETIMEDOUT) ? L"Таймаут ожидания ответа FTP-сервера"
                                           : L"Ошибка чтения ответа: " + WsaErrorText(code);
            return false;
        }
        s.buffer.append(buf, (size_t)got);
    }
}

bool ReadReply(FtpSession& s, FtpReply& reply, std::wstring& error) {
    std::wstring first;
    if (!ReadLine(s, first, error)) return false;
    if (first.size() < 3 || !iswdigit(first[0]) || !iswdigit(first[1]) || !iswdigit(first[2])) {
        error = L"Некорректный ответ FTP-сервера: " + first;
        return false;
    }
    reply.code = _wtoi(first.substr(0, 3).c_str());
    reply.text.clear();
    if (first.size() > 4) reply.text = first.substr(4);

    if (first.size() > 3 && first[3] == L'-') {
        const std::wstring prefix = first.substr(0, 3);
        for (;;) {
            std::wstring line;
            if (!ReadLine(s, line, error)) return false;
            if (line.size() >= 4 && line.compare(0, 3, prefix) == 0 && line[3] == L' ') {
                if (line.size() > 4) {
                    if (!reply.text.empty()) reply.text += L"\n";
                    reply.text += line.substr(4);
                }
                break;
            }
            if (!line.empty()) {
                if (!reply.text.empty()) reply.text += L"\n";
                reply.text += line;
            }
            if (s.buffer.size() > kMaxLine) {
                error = L"Слишком длинный многострочный ответ FTP-сервера";
                return false;
            }
        }
    }
    return true;
}

std::wstring OneLine(const std::wstring& text) {
    std::wstring out = text;
    for (auto& ch : out) {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
    }
    out = TrimWs(out);
    if (out.size() > 300) out = out.substr(0, 300) + L"…";
    return out;
}

bool SendCommand(FtpSession& s, const std::wstring& command, FtpReply& reply, std::wstring& error) {
    if (command.find_first_of(L"\r\n") != std::wstring::npos) {
        error = L"Недопустимый символ перевода строки в команде FTP";
        return false;
    }
    const std::string bytes = s.utf8 ? WideToUtf8(command) : WideToAnsi(command);
    if (bytes.empty() && !command.empty()) {
        error = L"Не удалось закодировать команду FTP";
        return false;
    }
    if (!SendAll(s.ctrl, bytes + "\r\n", error)) return false;
    return ReadReply(s, reply, error);
}

bool Expect(FtpSession& s, const std::wstring& command, int expected, FtpReply& reply, std::wstring& error) {
    if (!SendCommand(s, command, reply, error)) return false;
    if (reply.code != expected) {
        error = L"Сервер отклонил «" + command + L"»: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }
    return true;
}

struct DataChannel {
    SOCKET data = INVALID_SOCKET;
    SOCKET listener = INVALID_SOCKET;
    bool active = false;
};

void CloseChannel(DataChannel& ch) {
    CloseSocket(ch.data);
    CloseSocket(ch.listener);
}

bool ParsePasv(const std::wstring& text, std::wstring& host, int& port) {
    size_t open = text.find(L'(');
    size_t close = (open == std::wstring::npos) ? std::wstring::npos : text.find(L')', open);
    std::wstring inside = (open != std::wstring::npos && close != std::wstring::npos)
                              ? text.substr(open + 1, close - open - 1)
                              : text;
    std::vector<int> nums;
    size_t start = 0;
    while (start <= inside.size()) {
        const size_t comma = inside.find(L',', start);
        const std::wstring item = TrimWs(inside.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
        if (!item.empty() && IsAllDigits(item)) nums.push_back(_wtoi(item.c_str()));
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    if (nums.size() < 6) return false;
    host = std::to_wstring(nums[0]) + L"." + std::to_wstring(nums[1]) + L"." + std::to_wstring(nums[2]) +
           L"." + std::to_wstring(nums[3]);
    port = nums[4] * 256 + nums[5];
    return port > 0;
}

bool ParseEpsv(const std::wstring& text, int& port) {
    const size_t open = text.find(L'(');
    const size_t close = (open == std::wstring::npos) ? std::wstring::npos : text.find(L')', open);
    if (open == std::wstring::npos || close == std::wstring::npos) return false;
    const std::wstring inside = text.substr(open + 1, close - open - 1);
    const size_t last = inside.find_last_of(L'|');
    if (last == std::wstring::npos) return false;
    const size_t prev = inside.find_last_of(L'|', last - 1);
    if (prev == std::wstring::npos) return false;
    const std::wstring p = inside.substr(prev + 1, last - prev - 1);
    if (!IsAllDigits(p)) return false;
    port = _wtoi(p.c_str());
    return port > 0;
}

bool PrepareData(FtpSession& s, DataChannel& ch, std::wstring& error) {
    CloseChannel(ch);
    if (s.passive) {
        FtpReply reply;
        if (!SendCommand(s, L"PASV", reply, error)) return false;
        std::wstring host;
        int port = 0;
        if (reply.code != 227 || !ParsePasv(reply.text, host, port)) {
            // Некоторые серверы поддерживают только расширенный пассивный режим.
            FtpReply epsv;
            if (!SendCommand(s, L"EPSV", epsv, error)) return false;
            if (epsv.code != 229 || !ParseEpsv(epsv.text, port)) {
                error = L"Сервер не открыл пассивный режим: " + OneLine(reply.text);
                return false;
            }
            host = s.peerHost;
        }
        if (!s.trustPasvIp || host == L"0.0.0.0" || host.empty()) host = s.peerHost;
        SOCKET data = INVALID_SOCKET;
        std::wstring connectError;
        if (!ConnectTcp(host, port, kConnectTimeoutMs, data, connectError)) {
            // NAT: сервер вернул внутренний адрес — пробуем адрес управляющего соединения.
            if (!s.peerHost.empty() && host != s.peerHost) {
                if (!ConnectTcp(s.peerHost, port, kConnectTimeoutMs, data, connectError)) {
                    error = L"Не удалось открыть канал данных: " + connectError;
                    return false;
                }
            } else {
                error = L"Не удалось открыть канал данных: " + connectError;
                return false;
            }
        }
        SetTimeout(data, kDataTimeoutMs);
        ch.data = data;
        return true;
    }

    // Активный режим: слушаем свой порт и отдаём его серверу командой PORT.
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = L"Не удалось создать слушающий сокет: " + WsaErrorText(WSAGetLastError());
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        error = L"Не удалось открыть порт для активного режима: " + WsaErrorText(WSAGetLastError());
        CloseSocket(listener);
        return false;
    }
    int len = (int)sizeof(addr);
    getsockname(listener, (sockaddr*)&addr, &len);

    sockaddr_in local{};
    int localLen = (int)sizeof(local);
    getsockname(s.ctrl, (sockaddr*)&local, &localLen);
    const unsigned char* ip = (const unsigned char*)&local.sin_addr.s_addr;
    wchar_t command[128];
    swprintf(command, 128, L"PORT %u,%u,%u,%u,%u,%u", ip[0], ip[1], ip[2], ip[3],
             (unsigned)addr.sin_port & 0xFF, ((unsigned)addr.sin_port >> 8) & 0xFF);
    FtpReply reply;
    if (!Expect(s, command, 200, reply, error)) {
        CloseSocket(listener);
        return false;
    }
    ch.listener = listener;
    ch.active = true;
    return true;
}

bool AcceptData(DataChannel& ch, SOCKET& out, std::wstring& error) {
    if (ch.data != INVALID_SOCKET) {
        out = ch.data;
        ch.data = INVALID_SOCKET;
        return true;
    }
    if (ch.listener == INVALID_SOCKET) {
        error = L"Канал данных не подготовлен";
        return false;
    }
    fd_set rd{};
    FD_ZERO(&rd);
    FD_SET(ch.listener, &rd);
    timeval tv{};
    tv.tv_sec = kConnectTimeoutMs / 1000;
    tv.tv_usec = (kConnectTimeoutMs % 1000) * 1000;
    const int sel = select(0, &rd, nullptr, nullptr, &tv);
    if (sel <= 0) {
        error = (sel == 0) ? L"Сервер не подключился к каналу данных (активный режим)"
                           : L"Ошибка ожидания канала данных: " + WsaErrorText(WSAGetLastError());
        CloseChannel(ch);
        return false;
    }
    out = accept(ch.listener, nullptr, nullptr);
    CloseSocket(ch.listener);
    if (out == INVALID_SOCKET) {
        error = L"Не удалось принять соединение данных: " + WsaErrorText(WSAGetLastError());
        return false;
    }
    SetTimeout(out, kDataTimeoutMs);
    return true;
}

bool ReadDataAll(SOCKET data, std::string& out, std::wstring& error) {
    out.clear();
    char buf[65536];
    ULONGLONG lastData = GetTickCount64();
    for (;;) {
        fd_set rd{};
        FD_ZERO(&rd);
        FD_SET(data, &rd);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;
        const int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) {
            error = L"Ошибка чтения списка: " + WsaErrorText(WSAGetLastError());
            return false;
        }
        if (sel == 0) {
            if (GetTickCount64() - lastData > (ULONGLONG)kDataTimeoutMs) {
                error = L"Таймаут получения списка каталога";
                return false;
            }
            continue;
        }
        const int got = recv(data, buf, (int)sizeof(buf), 0);
        if (got == 0) break;
        if (got == SOCKET_ERROR) {
            error = L"Ошибка чтения списка: " + WsaErrorText(WSAGetLastError());
            return false;
        }
        out.append(buf, (size_t)got);
        lastData = GetTickCount64();
        if (out.size() > kMaxListing) {
            error = L"Слишком большой список каталога";
            return false;
        }
    }
    return true;
}

void AbortTransfer(FtpSession& s, DataChannel& ch) {
    CloseChannel(ch);
    DWORD shortTimeout = 2000;
    setsockopt(s.ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char*)&shortTimeout, sizeof(shortTimeout));
    FtpReply reply;
    std::wstring ignored;
    // После ABOR сервер может прислать 426 и затем 226.
    if (SendCommand(s, L"ABOR", reply, ignored)) {
        for (int i = 0; i < 3 && reply.code != 226 && reply.code != 225 && reply.code != 426; ++i) {
            if (!ReadReply(s, reply, ignored)) break;
        }
    }
    DWORD normal = (DWORD)kControlTimeoutMs;
    setsockopt(s.ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char*)&normal, sizeof(normal));
}

enum class ListResult { Ok, Unsupported, Failed };

ListResult ListOnce(FtpSession& s, const std::wstring& command, std::vector<FtpEntryInfo>& out, std::wstring& error) {
    DataChannel ch;
    if (!PrepareData(s, ch, error)) return ListResult::Failed;

    FtpReply reply;
    if (!SendCommand(s, command, reply, error)) {
        CloseChannel(ch);
        return ListResult::Failed;
    }
    if (reply.code == 500 || reply.code == 502 || reply.code == 504) {
        CloseChannel(ch);
        error = OneLine(reply.text);
        return ListResult::Unsupported;
    }
    if (reply.code != 125 && reply.code != 150) {
        CloseChannel(ch);
        error = L"Сервер отклонил «" + command + L"»: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return ListResult::Failed;
    }

    SOCKET data = INVALID_SOCKET;
    if (!AcceptData(ch, data, error)) return ListResult::Failed;

    std::string raw;
    const bool readOk = ReadDataAll(data, raw, error);
    CloseSocket(data);
    if (!readOk) return ListResult::Failed;

    FtpReply final;
    if (!ReadReply(s, final, error)) return ListResult::Failed;
    if (final.code != 226 && final.code != 250) {
        error = L"Передача списка не завершена: " + OneLine(final.text) +
                L" (код " + std::to_wstring(final.code) + L")";
        return ListResult::Failed;
    }

    std::vector<FtpEntryInfo> parsed;
    size_t start = 0;
    int skipped = 0;
    int failedLines = 0;
    while (start <= raw.size()) {
        const size_t nl = raw.find('\n', start);
        std::string line = raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (nl == std::string::npos && line.empty()) break;
        const std::wstring wide = DecodeServerText(line, s.utf8);
        FtpEntryInfo entry;
        bool skip = false;
        if (FtpParseListLine(wide, entry, skip)) {
            if (skip) ++skipped;
            else parsed.push_back(entry);
        } else if (!TrimWs(wide).empty()) {
            ++failedLines;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    if (parsed.empty() && failedLines > 0) {
        error = L"Не удалось разобрать список каталога (строк: " + std::to_wstring(failedLines) + L")";
        return ListResult::Failed;
    }

    out = std::move(parsed);
    (void)skipped;
    return ListResult::Ok;
}

bool NormalizeServerPath(const std::wstring& path, std::wstring& out) {
    if (path.find_first_of(L"\r\n") != std::wstring::npos) return false;
    out = NormalizeRemotePath(path);
    return true;
}

}  // namespace

FtpConnection::FtpConnection() = default;
FtpConnection::~FtpConnection() = default;

bool FtpConnect(const FtpSite& site, std::unique_ptr<FtpConnection>& out, std::wstring& error) {
    out.reset();
    error.clear();
    if (site.host.empty()) {
        error = L"Не указан адрес FTP-сервера";
        return false;
    }

    // Поле «Сервер» может содержать готовую строку ftp://user@host[:port]/путь.
    FtpServerField field;
    std::wstring fieldError;
    if (!FtpParseServerField(site.host, field, fieldError)) {
        error = fieldError;
        return false;
    }
    FtpSite target = site;
    target.host = field.host;
    if (field.port > 0) target.port = field.port;
    if (!field.user.empty()) target.user = field.user;
    if (!field.password.empty()) target.password = field.password;
    if (site.root.empty() && field.root != L"/") target.root = field.root;
    if (target.host.empty()) {
        error = L"Не указан адрес FTP-сервера";
        return false;
    }
    if (!EnsureWinsock(error)) return false;

    auto conn = std::make_unique<FtpConnection>();
    conn->impl_ = std::make_unique<FtpSession>();
    FtpSession& s = *conn->impl_;
    s.site = target;
    s.passive = target.passive;

    if (!ConnectTcp(target.host, target.port, kConnectTimeoutMs, s.ctrl, error)) return false;
    s.peerHost = NumericPeerHost(s.ctrl);

    FtpReply reply;
    if (!ReadReply(s, reply, error)) return false;
    if (reply.code == 120) {
        if (!ReadReply(s, reply, error)) return false;
    }
    if (reply.code != 220) {
        error = L"FTP-сервер не готов: " + OneLine(reply.text) + L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }

    const std::wstring user = target.user.empty() ? L"anonymous" : target.user;
    const std::wstring password = target.user.empty() ? L"anonymous@" : target.password;
    if (!SendCommand(s, L"USER " + user, reply, error)) return false;
    if (reply.code == 331) {
        if (!SendCommand(s, L"PASS " + password, reply, error)) return false;
    }
    if (reply.code != 230) {
        error = L"Не удалось войти на FTP-сервер: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }

    // FEAT — необязательная команда: сервер может ответить 5xx, но не рвать связь.
    if (!SendCommand(s, L"FEAT", reply, error)) return false;
    if (reply.code == 211) {
        const std::wstring upper = Upper(reply.text);
        s.mlsd = upper.find(L"MLSD") != std::wstring::npos;
    }

    if (!SendCommand(s, L"OPTS UTF8 ON", reply, error)) return false;
    s.utf8 = (reply.code == 200);

    if (!Expect(s, L"TYPE I", 200, reply, error)) return false;

    out = std::move(conn);
    return true;
}

void FtpDisconnect(std::unique_ptr<FtpConnection>& conn) {
    conn.reset();
}

bool FtpList(FtpConnection& conn, const std::wstring& dir, std::vector<FtpEntryInfo>& out, std::wstring& error) {
    out.clear();
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    FtpSession& s = *conn.impl_;

    std::wstring remote;
    if (!NormalizeServerPath(dir, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpReply reply;
    if (!Expect(s, L"CWD " + remote, 250, reply, error)) return false;

    if (s.mlsd) {
        const ListResult r = ListOnce(s, L"MLSD", out, error);
        if (r == ListResult::Ok) return true;
        if (r == ListResult::Failed) return false;
        error.clear();
    }

    const ListResult r = ListOnce(s, L"LIST", out, error);
    return r == ListResult::Ok;
}

bool FtpDownloadFile(FtpConnection& conn, const std::wstring& remotePath, const std::wstring& localPath,
                     const std::function<bool(ULONGLONG, ULONGLONG)>& progress, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    FtpSession& s = *conn.impl_;
    std::wstring remote;
    if (!NormalizeServerPath(remotePath, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }

    ULONGLONG total = 0;
    std::wstring ignore;
    FtpFileSize(conn, remote, total, ignore);

    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Не удалось создать локальный файл: " + localPath + L"\n" + ErrText(GetLastError());
        return false;
    }

    DataChannel ch;
    if (!PrepareData(s, ch, error)) {
        CloseHandle(file);
        DeleteFileW(localPath.c_str());
        return false;
    }

    FtpReply reply;
    if (!SendCommand(s, L"RETR " + remote, reply, error)) {
        CloseChannel(ch);
        CloseHandle(file);
        DeleteFileW(localPath.c_str());
        return false;
    }
    if (reply.code != 125 && reply.code != 150) {
        error = L"Сервер отклонил скачивание: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        CloseChannel(ch);
        CloseHandle(file);
        DeleteFileW(localPath.c_str());
        return false;
    }

    SOCKET data = INVALID_SOCKET;
    if (!AcceptData(ch, data, error)) {
        CloseHandle(file);
        DeleteFileW(localPath.c_str());
        return false;
    }

    if (progress && !progress(0, total)) {
        AbortTransfer(s, ch);
        CloseHandle(file);
        DeleteFileW(localPath.c_str());
        error = L"Отменено";
        return false;
    }

    ULONGLONG done = 0;
    bool ok = true;
    bool cancelled = false;
    char buf[65536];
    ULONGLONG lastData = GetTickCount64();
    for (;;) {
        fd_set rd{};
        FD_ZERO(&rd);
        FD_SET(data, &rd);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;
        const int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) {
            error = L"Ошибка чтения файла: " + WsaErrorText(WSAGetLastError());
            ok = false;
            break;
        }
        if (sel == 0) {
            if (GetTickCount64() - lastData > (ULONGLONG)kDataTimeoutMs) {
                error = L"Таймаут получения файла";
                ok = false;
                break;
            }
            if (progress && !progress(done, total)) {
                cancelled = true;
                break;
            }
            continue;
        }
        const int got = recv(data, buf, (int)sizeof(buf), 0);
        if (got == 0) break;
        if (got == SOCKET_ERROR) {
            error = L"Ошибка чтения файла: " + WsaErrorText(WSAGetLastError());
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(file, buf, (DWORD)got, &written, nullptr) || written != (DWORD)got) {
            error = L"Ошибка записи файла: " + ErrText(GetLastError());
            ok = false;
            break;
        }
        done += (ULONGLONG)got;
        lastData = GetTickCount64();
        if (progress && !progress(done, total)) {
            cancelled = true;
            break;
        }
    }

    CloseSocket(data);
    CloseHandle(file);

    if (cancelled) {
        AbortTransfer(s, ch);
        DeleteFileW(localPath.c_str());
        error = L"Отменено";
        return false;
    }
    if (!ok) {
        AbortTransfer(s, ch);
        DeleteFileW(localPath.c_str());
        return false;
    }

    if (!ReadReply(s, reply, error)) {
        DeleteFileW(localPath.c_str());
        return false;
    }
    if (reply.code != 226 && reply.code != 250) {
        error = L"Скачивание не завершено: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        DeleteFileW(localPath.c_str());
        return false;
    }
    return true;
}

bool FtpUploadFile(FtpConnection& conn, const std::wstring& localPath, const std::wstring& remotePath,
                   const std::function<bool(ULONGLONG, ULONGLONG)>& progress, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    FtpSession& s = *conn.impl_;
    std::wstring remote;
    if (!NormalizeServerPath(remotePath, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }

    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Не удалось открыть локальный файл: " + localPath + L"\n" + ErrText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const ULONGLONG total = (ULONGLONG)size.QuadPart;

    DataChannel ch;
    if (!PrepareData(s, ch, error)) {
        CloseHandle(file);
        return false;
    }

    FtpReply reply;
    if (!SendCommand(s, L"STOR " + remote, reply, error)) {
        CloseChannel(ch);
        CloseHandle(file);
        return false;
    }
    if (reply.code != 125 && reply.code != 150) {
        error = L"Сервер отклонил загрузку: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        CloseChannel(ch);
        CloseHandle(file);
        return false;
    }

    SOCKET data = INVALID_SOCKET;
    if (!AcceptData(ch, data, error)) {
        CloseHandle(file);
        return false;
    }

    if (progress && !progress(0, total)) {
        AbortTransfer(s, ch);
        CloseHandle(file);
        FtpDeleteFile(conn, remote, error);
        error = L"Отменено";
        return false;
    }

    ULONGLONG done = 0;
    bool ok = true;
    bool cancelled = false;
    char buf[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buf, (DWORD)sizeof(buf), &got, nullptr)) {
            error = L"Ошибка чтения локального файла: " + ErrText(GetLastError());
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!SendAll(data, std::string(buf, got), error)) {
            ok = false;
            break;
        }
        done += (ULONGLONG)got;
        if (progress && !progress(done, total)) {
            cancelled = true;
            break;
        }
    }

    CloseSocket(data);
    CloseHandle(file);

    if (cancelled) {
        AbortTransfer(s, ch);
        std::wstring ignore;
        FtpDeleteFile(conn, remote, ignore);
        error = L"Отменено";
        return false;
    }
    if (!ok) {
        AbortTransfer(s, ch);
        std::wstring ignore;
        FtpDeleteFile(conn, remote, ignore);
        return false;
    }

    if (!ReadReply(s, reply, error)) return false;
    if (reply.code != 226 && reply.code != 250) {
        error = L"Загрузка не завершена: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }
    return true;
}

bool FtpCreateDir(FtpConnection& conn, const std::wstring& dir, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    std::wstring remote;
    if (!NormalizeServerPath(dir, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpReply reply;
    if (!SendCommand(*conn.impl_, L"MKD " + remote, reply, error)) return false;
    if (reply.code != 257 && reply.code != 250) {
        error = L"Сервер не создал каталог: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }
    return true;
}

bool FtpDeleteFile(FtpConnection& conn, const std::wstring& file, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    std::wstring remote;
    if (!NormalizeServerPath(file, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpReply reply;
    return Expect(*conn.impl_, L"DELE " + remote, 250, reply, error);
}

bool FtpRemoveDir(FtpConnection& conn, const std::wstring& dir, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    std::wstring remote;
    if (!NormalizeServerPath(dir, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpReply reply;
    if (!SendCommand(*conn.impl_, L"RMD " + remote, reply, error)) return false;
    if (reply.code != 250) {
        error = L"Сервер не удалил каталог: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }
    return true;
}

bool FtpRename(FtpConnection& conn, const std::wstring& from, const std::wstring& to, std::wstring& error) {
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    std::wstring src, dst;
    if (!NormalizeServerPath(from, src) || !NormalizeServerPath(to, dst)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpSession& s = *conn.impl_;
    FtpReply reply;
    if (!Expect(s, L"RNFR " + src, 350, reply, error)) return false;
    if (!Expect(s, L"RNTO " + dst, 250, reply, error)) return false;
    return true;
}

bool FtpFileSize(FtpConnection& conn, const std::wstring& file, ULONGLONG& size, std::wstring& error) {
    size = 0;
    error.clear();
    if (!conn.impl_) {
        error = L"Нет соединения с FTP-сервером";
        return false;
    }
    std::wstring remote;
    if (!NormalizeServerPath(file, remote)) {
        error = L"Недопустимый путь на FTP-сервере";
        return false;
    }
    FtpReply reply;
    if (!SendCommand(*conn.impl_, L"SIZE " + remote, reply, error)) return false;
    if (reply.code != 213) {
        error = L"Сервер не сообщил размер: " + OneLine(reply.text) +
                L" (код " + std::to_wstring(reply.code) + L")";
        return false;
    }
    const std::wstring digits = TrimWs(reply.text);
    if (digits.empty() || !IsAllDigits(digits)) {
        error = L"Некорректный размер в ответе сервера: " + OneLine(reply.text);
        return false;
    }
    size = wcstoull(digits.c_str(), nullptr, 10);
    return true;
}

namespace {

bool FtpDirSizeRec(FtpConnection& conn, const std::wstring& remote, int depth, ULONGLONG& total,
                   std::wstring& error) {
    if (depth > 32) {
        error = L"Слишком глубокая вложенность каталогов";
        return false;
    }
    std::vector<FtpEntryInfo> list;
    if (!FtpList(conn, remote, list, error)) return false;
    for (auto& e : list) {
        if (e.isDir) {
            const std::wstring child = NormalizeRemotePath(remote + L"/" + e.name);
            if (!FtpDirSizeRec(conn, child, depth + 1, total, error)) return false;
        } else {
            total += e.size;
        }
    }
    return true;
}

}  // namespace

bool FtpDirectorySize(const std::wstring& url, ULONGLONG& size, std::wstring& error) {
    size = 0;
    error.clear();
    FtpSite site;
    std::wstring remote;
    if (!FtpParseUrl(url, site, remote)) {
        error = L"Некорректный FTP-адрес: " + url;
        return false;
    }
    if (!site.password.empty()) FtpRememberPassword(url, site.password);
    if (site.password.empty() && !site.user.empty() && !FtpPasswordFor(url, site.password)) {
        error = L"Нет пароля для " + site.user + L"@" + site.host;
        return false;
    }
    std::unique_ptr<FtpConnection> conn;
    if (!FtpConnect(site, conn, error)) return false;
    const bool ok = FtpDirSizeRec(*conn, remote, 0, size, error);
    FtpDisconnect(conn);
    return ok;
}
