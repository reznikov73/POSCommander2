// Встроенный ZIP: свой inflate (RFC 1951) + разбор оглавления (включая zip64).
// Зависимостей нет: только Win32 и стандартная библиотека.
#include "zip.h"

#include "util.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

const unsigned long kLocalSig = 0x04034b50;
const unsigned long kCentralSig = 0x02014b50;
const unsigned long kEocdSig = 0x06054b50;
const unsigned long kEocd64LocSig = 0x07064b50;
const unsigned long kEocd64Sig = 0x06064b50;

unsigned short Rd16(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }
unsigned long Rd32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}
unsigned long long Rd64(const unsigned char* p) {
    return (unsigned long long)Rd32(p) | ((unsigned long long)Rd32(p + 4) << 32);
}

// ---------- CRC-32 ----------

unsigned long g_crcTable[256];
bool g_crcReady = false;

void InitCrc() {
    if (g_crcReady) return;
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320ul ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcReady = true;
}

unsigned long CrcUpdate(unsigned long crc, const unsigned char* data, size_t size) {
    InitCrc();
    unsigned long c = crc ^ 0xFFFFFFFFul;
    for (size_t i = 0; i < size; i++) c = g_crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFul;
}

// ---------- строки ----------

// Строго проверяем UTF-8: нужно, чтобы отличить современные имена от старых ANSI/OEM.
bool IsValidUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= text.size() + (extra ? 0 : 1)) return false;
        for (int k = 1; k <= extra; k++) {
            if (i + k >= text.size()) return false;
            if (((unsigned char)text[i + k] & 0xC0) != 0x80) return false;
        }
        i += 1 + extra;
    }
    return true;
}

std::wstring FromMultiByte(const std::string& text, UINT codepage) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(codepage, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(codepage, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need, nullptr, nullptr);
    return out;
}

// Насколько «осмысленно» выглядит имя: кириллица и ASCII — хорошо, псевдографика и
// Latin-1 — почти всегда следствие неверной кодировки.
int ScoreName(const std::wstring& text) {
    int score = 0;
    for (wchar_t ch : text) {
        if (ch >= 0x20 && ch < 0x7F) score += 2;
        else if ((ch >= 0x0410 && ch <= 0x044F) || ch == 0x0401 || ch == 0x0451) score += 3;
        else if (ch >= 0x2500 && ch <= 0x259F) score -= 3;   // псевдографика DOS
        else if (ch >= 0x0080 && ch <= 0x00FF) score -= 2;   // Latin-1 поверх кириллицы
        else if (ch < 0x20) score -= 4;
    }
    return score;
}

// Имя из архива: флаг UTF-8, иначе валидный UTF-8, иначе выбираем ANSI или OEM
// (7-Zip пишет имена в OEM, Проводник — в ANSI).
std::wstring DecodeName(const std::string& raw, unsigned short flags) {
    std::wstring name;
    bool ascii = true;
    for (unsigned char c : raw) {
        if (c >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        name.assign(raw.begin(), raw.end());
    } else if (flags & 0x0800) {
        name = FromMultiByte(raw, CP_UTF8);
    } else if (IsValidUtf8(raw)) {
        name = FromMultiByte(raw, CP_UTF8);
    } else {
        const std::wstring ansi = FromMultiByte(raw, CP_ACP);
        const std::wstring oem = FromMultiByte(raw, CP_OEMCP);
        name = (ScoreName(oem) > ScoreName(ansi)) ? oem : ansi;
    }
    for (auto& ch : name) {
        if (ch == L'/') ch = L'\\';
    }
    return name;
}

// ---------- каталоги ----------

void EnsureDirs(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring path;
    for (size_t i = 0; i < dir.size(); i++) {
        path.push_back(dir[i]);
        const bool last = (i + 1 == dir.size());
        if (dir[i] == L'\\' || last) {
            if (path.size() > 3 || (path.size() == 3 && path[1] != L':')) {
                std::wstring trimmed = path;
                if (!last && !trimmed.empty() && trimmed.back() == L'\\') trimmed.pop_back();
                if (!trimmed.empty()) CreateDirectoryW(trimmed.c_str(), nullptr);
            }
        }
    }
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// ---------- inflate ----------

struct BitIn {
    FILE* f = nullptr;
    unsigned char buf[1 << 16];
    size_t pos = 0;
    size_t len = 0;
    unsigned long bits = 0;
    int count = 0;

    bool Fill() {
        if (pos < len) return true;
        len = fread(buf, 1, sizeof(buf), f);
        pos = 0;
        return len > 0;
    }
    // need <= 24
    bool Get(int need, unsigned long& out) {
        if (need == 0) {
            out = 0;
            return true;
        }
        while (count < need) {
            if (!Fill()) return false;
            bits |= (unsigned long)buf[pos++] << count;
            count += 8;
        }
        out = bits & ((1ul << need) - 1);
        bits >>= need;
        count -= need;
        return true;
    }
    void Align() {
        bits = 0;
        count = 0;
    }
};

struct Huff {
    unsigned short count[16];
    unsigned short symbol[320];
};

bool BuildHuff(Huff& h, const unsigned char* lengths, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;          // пустое дерево допустимо

    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;            // переподписка кодов
    }
    unsigned short offs[16]{};
    for (int len = 1; len < 15; len++) offs[len + 1] = (unsigned short)(offs[len] + h.count[len]);
    for (int i = 0; i < n; i++) {
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = (unsigned short)i;
    }
    return true;
}

int DecodeSym(BitIn& in, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        unsigned long bit = 0;
        if (!in.Get(1, bit)) return -1;
        code |= (int)bit;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Распаковка raw deflate из in.f в out. Возвращает false при ошибке, текст — в error.
bool Inflate(BitIn& in, FILE* out, unsigned long long& written, unsigned long& crc, std::wstring& error) {
    static const unsigned short kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const unsigned char kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const unsigned short kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                                 33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                                 1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
    static const unsigned char kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                                 6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

    unsigned char outbuf[1 << 16];
    size_t outlen = 0;
    unsigned char win[1 << 15];
    size_t wpos = 0;

    auto flush = [&]() -> bool {
        if (outlen == 0) return true;
        if (fwrite(outbuf, 1, outlen, out) != outlen) return false;
        crc = CrcUpdate(crc, outbuf, outlen);
        written += outlen;
        outlen = 0;
        return true;
    };
    auto emit = [&](unsigned char b) -> bool {
        win[wpos & 32767] = b;
        ++wpos;
        outbuf[outlen++] = b;
        if (outlen == sizeof(outbuf)) return flush();
        return true;
    };

    for (;;) {
        unsigned long last = 0, type = 0;
        if (!in.Get(1, last) || !in.Get(2, type)) {
            error = L"Архив обрывается внутри сжатых данных.";
            return false;
        }

        if (type == 0) {                       // без сжатия
            in.Align();
            unsigned long len = 0, nlen = 0;
            if (!in.Get(16, len) || !in.Get(16, nlen)) {
                error = L"Повреждён блок без сжатия.";
                return false;
            }
            if ((len ^ 0xFFFFul) != nlen) {
                error = L"Повреждён блок без сжатия (длины не сходятся).";
                return false;
            }
            for (unsigned long i = 0; i < len; i++) {
                unsigned long b = 0;
                if (!in.Get(8, b) || !emit((unsigned char)b)) {
                    error = L"Повреждён блок без сжатия.";
                    return false;
                }
            }
        } else if (type == 1 || type == 2) {
            Huff lit{}, dist{};
            if (type == 1) {
                unsigned char lengths[288];
                for (int i = 0; i < 144; i++) lengths[i] = 8;
                for (int i = 144; i < 256; i++) lengths[i] = 9;
                for (int i = 256; i < 280; i++) lengths[i] = 7;
                for (int i = 280; i < 288; i++) lengths[i] = 8;
                BuildHuff(lit, lengths, 288);
                for (int i = 0; i < 30; i++) lengths[i] = 5;
                BuildHuff(dist, lengths, 30);
            } else {
                unsigned long hlit = 0, hdist = 0, hclen = 0;
                if (!in.Get(5, hlit) || !in.Get(5, hdist) || !in.Get(4, hclen)) {
                    error = L"Повреждены заголовки сжатого блока.";
                    return false;
                }
                const int nlit = (int)hlit + 257;
                const int ndist = (int)hdist + 1;
                const int nclen = (int)hclen + 4;
                static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                unsigned char clen[19]{};
                for (int i = 0; i < nclen; i++) {
                    unsigned long v = 0;
                    if (!in.Get(3, v)) {
                        error = L"Повреждено дерево кодов длин.";
                        return false;
                    }
                    clen[kOrder[i]] = (unsigned char)v;
                }
                Huff clh{};
                if (!BuildHuff(clh, clen, 19)) {
                    error = L"Повреждено дерево кодов длин.";
                    return false;
                }
                unsigned char lengths[320]{};
                int index = 0;
                while (index < nlit + ndist) {
                    const int sym = DecodeSym(in, clh);
                    if (sym < 0) {
                        error = L"Повреждено дерево сжатого блока.";
                        return false;
                    }
                    if (sym < 16) {
                        lengths[index++] = (unsigned char)sym;
                    } else {
                        unsigned char value = 0;
                        unsigned long rep = 0;
                        int extra = 0;
                        if (sym == 16) {
                            if (index == 0) {
                                error = L"Повреждены коды длин.";
                                return false;
                            }
                            value = lengths[index - 1];
                            extra = 2;
                            if (!in.Get(2, rep)) return false;
                            rep += 3;
                        } else if (sym == 17) {
                            extra = 3;
                            if (!in.Get(3, rep)) return false;
                            rep += 3;
                        } else {
                            extra = 7;
                            if (!in.Get(7, rep)) return false;
                            rep += 11;
                        }
                        (void)extra;
                        if (index + (int)rep > nlit + ndist) {
                            error = L"Повреждены коды длин.";
                            return false;
                        }
                        for (unsigned long k = 0; k < rep; k++) lengths[index++] = value;
                    }
                }
                if (!BuildHuff(lit, lengths, nlit) || !BuildHuff(dist, lengths + nlit, ndist)) {
                    error = L"Повреждено дерево сжатого блока.";
                    return false;
                }
            }

            for (;;) {
                const int sym = DecodeSym(in, lit);
                if (sym < 0) {
                    error = L"Повреждены сжатые данные.";
                    return false;
                }
                if (sym < 256) {
                    if (!emit((unsigned char)sym)) {
                        error = L"Не удалось записать распакованные данные.";
                        return false;
                    }
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li >= 29) {
                        error = L"Недопустимая длина в сжатых данных.";
                        return false;
                    }
                    unsigned long extra = 0;
                    if (!in.Get(kLenExtra[li], extra)) {
                        error = L"Повреждены сжатые данные.";
                        return false;
                    }
                    const size_t length = (size_t)kLenBase[li] + extra;
                    const int ds = DecodeSym(in, dist);
                    if (ds < 0 || ds >= 30) {
                        error = L"Недопустимое расстояние в сжатых данных.";
                        return false;
                    }
                    if (!in.Get(kDistExtra[ds], extra)) {
                        error = L"Повреждены сжатые данные.";
                        return false;
                    }
                    const size_t distance = (size_t)kDistBase[ds] + extra;
                    if (distance > wpos) {
                        error = L"Ссылка на данные до начала файла.";
                        return false;
                    }
                    for (size_t i = 0; i < length; i++) {
                        const unsigned char b = win[(wpos - distance) & 32767];
                        if (!emit(b)) {
                            error = L"Не удалось записать распакованные данные.";
                            return false;
                        }
                    }
                }
            }
        } else {
            error = L"Неизвестный тип сжатия в архиве.";
            return false;
        }

        if (last) break;
    }
    if (!flush()) {
        error = L"Не удалось записать файл.";
        return false;
    }
    return true;
}

// ---------- чтение ----------

struct FileCloser {
    void operator()(FILE* f) const {
        if (f) fclose(f);
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr OpenRead(const std::wstring& path) { return FilePtr(_wfopen(path.c_str(), L"rb")); }

bool ReadAt(FILE* f, unsigned long long offset, void* buf, size_t size) {
    if (_fseeki64(f, (long long)offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, size, f) == size;
}

// Разбор центрального каталога по уже прочитанным данным.
void ParseCentral(const std::vector<unsigned char>& cd, std::vector<ZipItem>& items) {
    size_t pos = 0;
    while (pos + 46 <= cd.size()) {
        const unsigned char* p = cd.data() + pos;
        if (Rd32(p) != kCentralSig) break;
        ZipItem item;
        item.flags = Rd16(p + 8);
        item.method = Rd16(p + 10);
        item.dosTime = Rd16(p + 12);
        item.dosDate = Rd16(p + 14);
        item.crc = Rd32(p + 16);
        unsigned long long csize = Rd32(p + 20);
        unsigned long long usize = Rd32(p + 24);
        const unsigned short nameLen = Rd16(p + 28);
        const unsigned short extraLen = Rd16(p + 30);
        const unsigned short commentLen = Rd16(p + 32);
        unsigned long long localOffset = Rd32(p + 42);

        // Дополнительное поле zip64: порядок — usize, csize, offset (только для 0xFFFFFFFF).
        size_t extraPos = pos + 46 + nameLen;
        const size_t extraEnd = extraPos + extraLen;
        while (extraPos + 4 <= extraEnd && extraPos + 4 <= cd.size()) {
            const unsigned short id = Rd16(cd.data() + extraPos);
            const unsigned short size = Rd16(cd.data() + extraPos + 2);
            const size_t dataPos = extraPos + 4;
            if (id == 0x0001) {
                size_t cur = dataPos;
                if (usize == 0xFFFFFFFFull && cur + 8 <= dataPos + size) {
                    usize = Rd64(cd.data() + cur);
                    cur += 8;
                }
                if (csize == 0xFFFFFFFFull && cur + 8 <= dataPos + size) {
                    csize = Rd64(cd.data() + cur);
                    cur += 8;
                }
                if (localOffset == 0xFFFFFFFFull && cur + 8 <= dataPos + size) {
                    localOffset = Rd64(cd.data() + cur);
                    cur += 8;
                }
            }
            extraPos += 4 + size;
        }

        const std::string rawName(reinterpret_cast<const char*>(cd.data() + pos + 46), nameLen);
        item.nameRaw = rawName;
        item.name = DecodeName(rawName, item.flags);
        item.size = usize;
        item.packed = csize;
        item.offset = localOffset;
        item.isDir = (!item.name.empty() && item.name.back() == L'\\');
        if (item.isDir) item.name.pop_back();

        if (!item.name.empty()) items.push_back(std::move(item));
        pos = extraEnd + commentLen;
    }
}

bool OpenIndex(FILE* f, unsigned long long fileSize, std::vector<ZipItem>& items, std::wstring& error) {
    if (fileSize < 22) {
        error = L"Файл слишком мал для zip.";
        return false;
    }
    // EOCD ищем с конца: комментарий к архиву бывает до 64 Кб.
    const size_t tailSize = (size_t)(fileSize < 66560ull ? fileSize : 66560ull);
    std::vector<unsigned char> tail(tailSize);
    if (!ReadAt(f, fileSize - tailSize, tail.data(), tailSize)) {
        error = L"Не удалось прочитать конец файла.";
        return false;
    }
    long long eocd = -1;
    for (long long i = (long long)tailSize - 22; i >= 0; i--) {
        if (Rd32(tail.data() + i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        error = L"Это не zip-архив (нет оглавления).";
        return false;
    }

    const unsigned char* e = tail.data() + eocd;
    unsigned long long count = Rd16(e + 10);
    unsigned long long cdSize = Rd32(e + 12);
    unsigned long long cdOffset = Rd32(e + 16);
    const unsigned long long eocdAbs = fileSize - tailSize + (unsigned long long)eocd;

    if (count == 0xFFFFull || cdSize == 0xFFFFFFFFull || cdOffset == 0xFFFFFFFFull) {
        // zip64: локатор находится непосредственно перед EOCD.
        unsigned char loc[20]{};
        if (eocdAbs >= 20 && ReadAt(f, eocdAbs - 20, loc, 20) && Rd32(loc) == kEocd64LocSig) {
            const unsigned long long eocd64 = Rd64(loc + 8);
            unsigned char rec[56]{};
            if (ReadAt(f, eocd64, rec, 56) && Rd32(rec) == kEocd64Sig) {
                count = Rd64(rec + 32);
                cdSize = Rd64(rec + 40);
                cdOffset = Rd64(rec + 48);
            }
        }
    }

    if (cdSize == 0 || cdOffset + cdSize > fileSize) {
        error = L"Повреждено оглавление архива.";
        return false;
    }
    std::vector<unsigned char> cd((size_t)cdSize);
    if (!ReadAt(f, cdOffset, cd.data(), cd.size())) {
        error = L"Не удалось прочитать оглавление архива.";
        return false;
    }
    ParseCentral(cd, items);
    if (items.empty() && count > 0) {
        error = L"Оглавление архива пустое или повреждено.";
        return false;
    }
    return true;
}

}  // namespace

bool ZipLooksLikeZip(const std::wstring& path) {
    FilePtr f = OpenRead(path);
    if (!f) return false;
    unsigned char sig[4]{};
    if (fread(sig, 1, 4, f.get()) != 4) return false;
    return Rd32(sig) == kLocalSig || Rd32(sig) == kEocdSig;
}

bool ZipReadIndex(const std::wstring& zipPath, std::vector<ZipItem>& items, std::wstring& error) {
    items.clear();
    error.clear();
    FilePtr f = OpenRead(zipPath);
    if (!f) {
        error = L"Не удалось открыть файл архива.";
        return false;
    }
    _fseeki64(f.get(), 0, SEEK_END);
    const long long size = _ftelli64(f.get());
    if (size < 0) {
        error = L"Не удалось определить размер архива.";
        return false;
    }
    return OpenIndex(f.get(), (unsigned long long)size, items, error);
}

namespace {

// Распаковка одной записи в уже открытый выходной файл.
bool ExtractEntry(FILE* zip, const ZipItem& item, FILE* out, std::wstring& error) {
    unsigned char header[30]{};
    if (!ReadAt(zip, item.offset, header, 30) || Rd32(header) != kLocalSig) {
        error = L"Повреждён локальный заголовок записи: " + item.name;
        return false;
    }
    const unsigned short nameLen = Rd16(header + 26);
    const unsigned short extraLen = Rd16(header + 28);
    const unsigned long long dataOffset = item.offset + 30 + nameLen + extraLen;
    if (_fseeki64(zip, (long long)dataOffset, SEEK_SET) != 0) {
        error = L"Не удалось перейти к данным записи: " + item.name;
        return false;
    }

    unsigned long long written = 0;
    unsigned long crc = 0;

    if (item.method == 0) {
        unsigned char buf[1 << 16];
        unsigned long long left = item.packed;
        while (left > 0) {
            const size_t want = (size_t)(left < sizeof(buf) ? left : sizeof(buf));
            const size_t got = fread(buf, 1, want, zip);
            if (got == 0) {
                error = L"Данные записи обрываются: " + item.name;
                return false;
            }
            if (fwrite(buf, 1, got, out) != got) {
                error = L"Не удалось записать файл: " + item.name;
                return false;
            }
            crc = CrcUpdate(crc, buf, got);
            written += got;
            left -= got;
        }
    } else if (item.method == 8) {
        BitIn in;
        in.f = zip;
        if (!Inflate(in, out, written, crc, error)) {
            if (!error.empty() && error[0] != L'П') error = L"Ошибка распаковки «" + item.name + L"»: " + error;
            return false;
        }
    } else {
        error = L"Метод сжатия " + std::to_wstring(item.method) + L" не поддерживается: " + item.name;
        return false;
    }

    if (written != item.size) {
        error = L"Размер после распаковки не совпал: " + item.name;
        return false;
    }
    if (item.crc != 0 && crc != item.crc) {
        error = L"Контрольная сумма не совпала: " + item.name;
        return false;
    }
    return true;
}

}  // namespace

bool ZipExtractOne(const std::wstring& zipPath, const std::wstring& name, const std::wstring& targetFile,
                   std::wstring& error) {
    error.clear();
    std::vector<ZipItem> items;
    if (!ZipReadIndex(zipPath, items, error)) return false;

    const ZipItem* found = nullptr;
    for (const auto& item : items) {
        if (SamePath(item.name, name)) {
            found = &item;
            break;
        }
    }
    if (!found) {
        error = L"В архиве нет записи: " + name;
        return false;
    }

    EnsureDirs(ParentPath(targetFile));
    FilePtr zip = OpenRead(zipPath);
    if (!zip) {
        error = L"Не удалось открыть архив.";
        return false;
    }
    FilePtr out(_wfopen(targetFile.c_str(), L"wb"));
    if (!out) {
        error = L"Не удалось создать файл: " + targetFile;
        return false;
    }
    return ExtractEntry(zip.get(), *found, out.get(), error);
}

bool ZipExtractTo(const std::wstring& zipPath, const std::vector<std::wstring>& names, const std::wstring& target,
                  std::wstring& error) {
    error.clear();
    std::vector<ZipItem> items;
    if (!ZipReadIndex(zipPath, items, error)) return false;

    FilePtr zip = OpenRead(zipPath);
    if (!zip) {
        error = L"Не удалось открыть архив.";
        return false;
    }

    const bool all = names.empty();
    for (const auto& item : items) {
        bool needed = all;
        if (!needed) {
            for (const auto& want : names) {
                if (SamePath(item.name, want) || item.name.rfind(want + L"\\", 0) == 0) {
                    needed = true;
                    break;
                }
            }
        }
        if (!needed) continue;

        const std::wstring full = JoinPath(target, item.name);
        if (item.isDir) {
            EnsureDirs(full);
            continue;
        }
        EnsureDirs(ParentPath(full));
        FilePtr out(_wfopen(full.c_str(), L"wb"));
        if (!out) {
            error = L"Не удалось создать файл: " + full;
            return false;
        }
        if (!ExtractEntry(zip.get(), item, out.get(), error)) return false;
    }
    return true;
}

namespace {

void DosFromFileTime(const FILETIME& ft, unsigned short& dosTime, unsigned short& dosDate) {
    FILETIME local{};
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    dosTime = (unsigned short)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    dosDate = (unsigned short)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

void Put16(std::vector<unsigned char>& out, unsigned short v) {
    out.push_back((unsigned char)(v & 0xFF));
    out.push_back((unsigned char)(v >> 8));
}
void Put32(std::vector<unsigned char>& out, unsigned long v) {
    for (int i = 0; i < 4; i++) out.push_back((unsigned char)((v >> (8 * i)) & 0xFF));
}

}  // namespace

namespace {

// ---------- сжатие (fixed Huffman + LZ77) ----------

struct BitWriter {
    std::vector<unsigned char>& out;
    unsigned long long bits = 0;
    int count = 0;
    explicit BitWriter(std::vector<unsigned char>& target) : out(target) {}

    void Put(unsigned value, int n) {                 // младшим битом вперёд
        bits |= (unsigned long long)value << count;
        count += n;
        while (count >= 8) {
            out.push_back((unsigned char)(bits & 0xFF));
            bits >>= 8;
            count -= 8;
        }
    }
    void PutCode(unsigned code, int n) {              // код Хаффмана — от старшего бита
        for (int i = n - 1; i >= 0; i--) Put((code >> i) & 1, 1);
    }
    void Flush() {
        if (count > 0) {
            out.push_back((unsigned char)(bits & 0xFF));
            bits = 0;
            count = 0;
        }
    }
};

const unsigned short kLenBaseW[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const unsigned char kLenExtraW[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short kDistBaseW[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                       33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                       1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const unsigned char kDistExtraW[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

void EmitSym(BitWriter& bw, int sym) {               // символ литерала/длины 0..287
    if (sym <= 143) bw.PutCode(0x30 + sym, 8);
    else if (sym <= 255) bw.PutCode(0x190 + (sym - 144), 9);
    else if (sym <= 279) bw.PutCode(sym - 256, 7);
    else bw.PutCode(0xC0 + (sym - 280), 8);
}

// Сжатие в raw deflate одним блоком fixed Huffman. Результат может быть больше входа —
// вызывающий решает, использовать ли его.
bool DeflateFixed(const std::vector<unsigned char>& src, std::vector<unsigned char>& out) {
    if (src.size() > 0xFFFFFFF0ull) return false;    // слишком большой кусок для одного блока

    BitWriter bw(out);
    bw.Put(1, 1);          // BFINAL
    bw.Put(1, 2);          // BTYPE = 01 — fixed Huffman

    const size_t kWindow = 32768;
    const int kMinMatch = 3;
    const int kMaxMatch = 258;
    const int kHashBits = 15;
    const size_t kHashSize = (size_t)1 << kHashBits;

    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(src.size(), -1);

    auto hash3 = [&](size_t i) -> size_t {
        return (((size_t)src[i] << 10) ^ ((size_t)src[i + 1] << 5) ^ (size_t)src[i + 2]) & (kHashSize - 1);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= src.size()) return;
        const size_t h = hash3(i);
        prev[i] = head[h];
        head[h] = (int)i;
    };

    size_t i = 0;
    while (i < src.size()) {
        int bestLen = 0;
        size_t bestDist = 0;
        if (i + kMinMatch <= src.size()) {
            int cand = head[hash3(i)];
            int chain = 48;
            const size_t maxLen = (src.size() - i) < (size_t)kMaxMatch ? (src.size() - i) : (size_t)kMaxMatch;
            while (cand >= 0 && chain-- > 0) {
                const size_t dist = i - (size_t)cand;
                if (dist == 0 || dist > kWindow) break;
                size_t len = 0;
                while (len < maxLen && src[(size_t)cand + len] == src[i + len]) ++len;
                if ((int)len > bestLen) {
                    bestLen = (int)len;
                    bestDist = dist;
                    if (bestLen >= 128) break;
                }
                cand = prev[(size_t)cand];
            }
        }

        if (bestLen >= kMinMatch) {
            int li = 0;
            while (li < 28 && kLenBaseW[li + 1] <= bestLen) ++li;
            EmitSym(bw, 257 + li);
            bw.Put((unsigned)(bestLen - kLenBaseW[li]), kLenExtraW[li]);
            int di = 0;
            while (di < 29 && kDistBaseW[di + 1] <= (int)bestDist) ++di;
            bw.PutCode((unsigned)di, 5);
            bw.Put((unsigned)(bestDist - kDistBaseW[di]), kDistExtraW[di]);
            for (int k = 0; k < bestLen; k++) insert(i + (size_t)k);
            i += (size_t)bestLen;
        } else {
            EmitSym(bw, src[i]);
            insert(i);
            ++i;
        }
    }
    EmitSym(bw, 256);      // конец блока
    bw.Flush();
    return true;
}

// ---------- запись новых записей ----------

struct OutRec {
    std::string nameRaw;
    unsigned short flags = 0;
    unsigned short method = 0;
    unsigned short dosTime = 0;
    unsigned short dosDate = 0;
    unsigned long crc = 0;
    unsigned long long csize = 0;
    unsigned long long usize = 0;
    unsigned long long offset = 0;
};

bool WriteLocalHeader(FILE* out, const OutRec& rec, unsigned long long& offset, std::wstring& error) {
    if (rec.csize > 0xFFFFFFFEull || rec.usize > 0xFFFFFFFEull) {
        error = L"Запись больше 4 ГБ: изменение такого архива без 7-Zip не поддерживается.";
        return false;
    }
    std::vector<unsigned char> h;
    Put32(h, kLocalSig);
    Put16(h, 20);
    Put16(h, rec.flags);
    Put16(h, rec.method);
    Put16(h, rec.dosTime);
    Put16(h, rec.dosDate);
    Put32(h, rec.crc);
    Put32(h, (unsigned long)rec.csize);
    Put32(h, (unsigned long)rec.usize);
    Put16(h, (unsigned short)rec.nameRaw.size());
    Put16(h, 0);
    h.insert(h.end(), rec.nameRaw.begin(), rec.nameRaw.end());
    if (fwrite(h.data(), 1, h.size(), out) != h.size()) {
        error = L"Не удалось записать архив (нет места?).";
        return false;
    }
    offset += h.size();
    return true;
}

// Записать новый файл в архив: сжимаем, если выгодно и файл не слишком большой (иначе store).
bool WriteNewFile(FILE* out, const std::wstring& path, const std::string& nameUtf8,
                  unsigned long long& offset, OutRec& rec, std::wstring& error) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    unsigned short dosTime = 0, dosDate = 0;
    unsigned long long size = 0;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        DosFromFileTime(fad.ftLastWriteTime, dosTime, dosDate);
        size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    const unsigned long long kMaxInMemory = 64ull * 1024 * 1024;
    std::vector<unsigned char> packed;
    unsigned long crc = 0;
    unsigned short method = 0;

    if (size <= kMaxInMemory) {
        std::vector<unsigned char> raw((size_t)size);
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) {
            error = L"Не удалось прочитать файл: " + path;
            return false;
        }
        const size_t got = raw.empty() ? 0 : fread(raw.data(), 1, raw.size(), f);
        fclose(f);
        if (got != raw.size()) {
            error = L"Файл изменился во время упаковки: " + path;
            return false;
        }
        if (!raw.empty()) crc = CrcUpdate(0, raw.data(), raw.size());

        std::vector<unsigned char> deflated;
        if (raw.size() >= 32 && DeflateFixed(raw, deflated) && deflated.size() < raw.size()) {
            packed.swap(deflated);
            method = 8;
        } else {
            packed.swap(raw);
            method = 0;
        }
    } else {
        // Большой файл: считаем CRC первым проходом, пишем потоком без сжатия.
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) {
            error = L"Не удалось прочитать файл: " + path;
            return false;
        }
        unsigned char buf[1 << 16];
        size_t got = 0;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) crc = CrcUpdate(crc, buf, got);
        fclose(f);
        method = 0;
    }

    rec.nameRaw = nameUtf8;
    rec.flags = 0x0800;                       // имена в UTF-8
    rec.method = method;
    rec.dosTime = dosTime;
    rec.dosDate = dosDate;
    rec.crc = crc;
    rec.csize = (method == 8) ? (unsigned long long)packed.size() : size;
    rec.usize = size;
    rec.offset = offset;
    if (!WriteLocalHeader(out, rec, offset, error)) return false;

    if (method == 8) {
        if (!packed.empty() && fwrite(packed.data(), 1, packed.size(), out) != packed.size()) {
            error = L"Не удалось записать архив (нет места?).";
            return false;
        }
        offset += packed.size();
        return true;
    }

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        error = L"Не удалось перечитать файл: " + path;
        return false;
    }
    unsigned char buf[1 << 16];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fclose(f);
            error = L"Не удалось записать архив (нет места?).";
            return false;
        }
        offset += got;
    }
    fclose(f);
    return true;
}

bool WriteCentral(FILE* out, const std::vector<OutRec>& recs, unsigned long long& offset, std::wstring& error) {
    if (recs.size() > 0xFFFF) {
        error = L"В архиве больше 65535 записей: изменение без 7-Zip не поддерживается.";
        return false;
    }
    const unsigned long long cdOffset = offset;
    std::vector<unsigned char> cd;
    for (const auto& rec : recs) {
        if (rec.offset > 0xFFFFFFFEull) {
            error = L"Архив больше 4 ГБ: изменение без 7-Zip не поддерживается.";
            return false;
        }
        Put32(cd, kCentralSig);
        Put16(cd, 20);
        Put16(cd, 20);
        Put16(cd, rec.flags);
        Put16(cd, rec.method);
        Put16(cd, rec.dosTime);
        Put16(cd, rec.dosDate);
        Put32(cd, rec.crc);
        Put32(cd, (unsigned long)rec.csize);
        Put32(cd, (unsigned long)rec.usize);
        Put16(cd, (unsigned short)rec.nameRaw.size());
        Put16(cd, 0);
        Put16(cd, 0);
        Put16(cd, 0);
        Put16(cd, 0);
        Put32(cd, 0);
        Put32(cd, (unsigned long)rec.offset);
        cd.insert(cd.end(), rec.nameRaw.begin(), rec.nameRaw.end());
    }
    if (!cd.empty() && fwrite(cd.data(), 1, cd.size(), out) != cd.size()) {
        error = L"Не удалось записать оглавление архива.";
        return false;
    }

    std::vector<unsigned char> eocd;
    Put32(eocd, kEocdSig);
    Put16(eocd, 0);
    Put16(eocd, 0);
    Put16(eocd, (unsigned short)recs.size());
    Put16(eocd, (unsigned short)recs.size());
    Put32(eocd, (unsigned long)cd.size());
    Put32(eocd, (unsigned long)cdOffset);
    Put16(eocd, 0);
    if (fwrite(eocd.data(), 1, eocd.size(), out) != eocd.size()) {
        error = L"Не удалось записать конец архива.";
        return false;
    }
    return true;
}

}  // namespace

bool ZipCreateFrom(const std::wstring& zipPath, const std::vector<ZipSource>& sources, std::wstring& error) {
    error.clear();
    if (sources.empty()) {
        error = L"Нечего упаковывать.";
        return false;
    }
    FilePtr out(_wfopen(zipPath.c_str(), L"wb"));
    if (!out) {
        error = L"Не удалось создать архив: " + zipPath;
        return false;
    }

    std::vector<OutRec> recs;
    unsigned long long offset = 0;
    for (const auto& source : sources) {
        OutRec rec;
        if (!WriteNewFile(out.get(), source.path, ToUtf8(source.inner), offset, rec, error)) return false;
        recs.push_back(rec);
    }
    return WriteCentral(out.get(), recs, offset, error);
}

bool ZipModify(const std::wstring& zipPath, const std::vector<std::wstring>& deleteNames,
               const std::vector<ZipSource>& addItems, std::wstring& error) {
    error.clear();
    std::vector<ZipItem> items;
    if (!ZipReadIndex(zipPath, items, error)) return false;

    auto matches = [](const std::wstring& name, const std::vector<std::wstring>& list) {
        for (const auto& it : list) {
            if (SamePath(name, it)) return true;
            if (!it.empty() && name.rfind(it + L"\\", 0) == 0) return true;
        }
        return false;
    };

    const std::wstring tempPath = zipPath + L".posfiles-tmp";
    FilePtr out(_wfopen(tempPath.c_str(), L"wb"));
    if (!out) {
        error = L"Не удалось создать временный файл рядом с архивом.\nПроверьте права на папку.";
        return false;
    }
    FilePtr src = OpenRead(zipPath);
    if (!src) {
        error = L"Не удалось открыть архив.";
        return false;
    }

    std::vector<OutRec> recs;
    unsigned long long offset = 0;
    unsigned char buf[1 << 16];

    for (const auto& item : items) {
        if (matches(item.name, deleteNames)) continue;
        bool replaced = false;
        for (const auto& add : addItems) {
            if (SamePath(item.name, add.inner)) {
                replaced = true;
                break;
            }
        }
        if (replaced) continue;

        OutRec rec;
        rec.nameRaw = item.nameRaw;
        rec.flags = (unsigned short)(item.flags & ~0x0008);   // без потокового дескриптора
        rec.method = item.isDir ? 0 : item.method;
        rec.dosTime = item.dosTime;
        rec.dosDate = item.dosDate;
        rec.crc = item.isDir ? 0 : item.crc;
        rec.csize = item.isDir ? 0 : item.packed;
        rec.usize = item.isDir ? 0 : item.size;
        rec.offset = offset;
        if (!WriteLocalHeader(out.get(), rec, offset, error)) return false;
        recs.push_back(rec);

        if (item.isDir || item.packed == 0) continue;

        unsigned char header[30]{};
        if (!ReadAt(src.get(), item.offset, header, 30) || Rd32(header) != kLocalSig) {
            error = L"Повреждён локальный заголовок записи: " + item.name;
            return false;
        }
        const unsigned long long dataOffset = item.offset + 30 + Rd16(header + 26) + Rd16(header + 28);
        if (_fseeki64(src.get(), (long long)dataOffset, SEEK_SET) != 0) {
            error = L"Не удалось перейти к данным записи: " + item.name;
            return false;
        }
        unsigned long long left = item.packed;
        while (left > 0) {
            const size_t want = (size_t)(left < sizeof(buf) ? left : sizeof(buf));
            const size_t got = fread(buf, 1, want, src.get());
            if (got != want) {
                error = L"Данные записи обрываются: " + item.name;
                return false;
            }
            if (fwrite(buf, 1, got, out.get()) != got) {
                error = L"Не удалось записать временный архив.";
                return false;
            }
            left -= got;
            offset += got;
        }
    }

    for (const auto& add : addItems) {
        std::wstring inner = add.inner;
        for (auto& ch : inner) {
            if (ch == L'/') ch = L'\\';
        }
        OutRec rec;
        if (!WriteNewFile(out.get(), add.path, ToUtf8(inner), offset, rec, error)) return false;
        recs.push_back(rec);
    }

    if (!WriteCentral(out.get(), recs, offset, error)) return false;

    out.reset();
    src.reset();
    if (!MoveFileExW(tempPath.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"Не удалось заменить архив (файл занят другой программой?).";
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}