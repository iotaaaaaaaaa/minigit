#include "util.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <zlib.h>

std::string read_file(const fs::path& p) {
    std::string out;
    if (!try_read_file(p, out)) throw Error("cannot read " + p.string());
    return out;
}

bool try_read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void write_file_atomic(const fs::path& p, const std::string& data) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".tmp" + std::to_string(getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot write " + tmp.string());
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) throw Error("write failed for " + tmp.string());
    }
    fs::rename(tmp, p);
}

std::string to_hex(const std::string& raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (hex_val(c) < 0) return false;
    return true;
}

std::string from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0 || !is_hex(hex)) throw Error("bad hex string: " + hex);
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<char>(hex_val(hex[i]) * 16 + hex_val(hex[i + 1])));
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl + 1;
        lines.emplace_back(text, start, end - start);
        start = end;
    }
    return lines;
}

std::string zlib_compress(const std::string& data) {
    uLongf cap = compressBound(data.size());
    std::string out(cap, '\0');
    if (compress(reinterpret_cast<Bytef*>(&out[0]), &cap,
                 reinterpret_cast<const Bytef*>(data.data()), data.size()) != Z_OK)
        throw Error("zlib compress failed");
    out.resize(cap);
    return out;
}

std::string zlib_decompress(const std::string& data) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw Error("zlib init failed");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    char buf[1 << 15];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            throw Error("corrupt zlib stream");
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    }
    inflateEnd(&zs);
    return out;
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4). The message is padded to a multiple of 64 bytes, then
// each 64-byte block is mixed into five 32-bit state words over 80 rounds.
// ---------------------------------------------------------------------------

static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    // Padding: a 1 bit, zeros, then the message length in bits as 64-bit
    // big-endian, so the total is a multiple of 64 bytes.
    std::string msg = data;
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; i--) msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xff));

    for (size_t off = 0; off < msg.size(); off += 64) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + off;
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string out(20, '\0');
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<char>((h[i] >> (24 - 8 * j)) & 0xff);
    return out;
}
