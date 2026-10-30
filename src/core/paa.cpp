#include "paa.h"

#include <cstring>

namespace paa {

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd24(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool isKnownFormat(uint16_t v) {
    switch (Format(v)) {
        case Format::DXT1: case Format::DXT2: case Format::DXT3: case Format::DXT4: case Format::DXT5:
        case Format::ARGB4444: case Format::ARGB1555: case Format::ARGB8888: case Format::AI88:
            return true;
        default:
            return false;
    }
}

bool isDxt(Format f) { return (uint16_t(f) & 0xFF00) == 0xFF00; }

size_t dxtBlockBytes(Format f) { return f == Format::DXT1 ? 8 : 16; }

size_t rawSize(Format f, uint32_t w, uint32_t h) {
    if (isDxt(f)) return size_t((w + 3) / 4) * ((h + 3) / 4) * dxtBlockBytes(f);
    switch (f) {
        case Format::ARGB8888: return size_t(w) * h * 4;
        case Format::P8: return size_t(w) * h;
        default: return size_t(w) * h * 2;
    }
}

uint8_t expand4(unsigned v) { return uint8_t(v * 17); }
uint8_t expand5(unsigned v) { return uint8_t((v << 3) | (v >> 2)); }
uint8_t expand6(unsigned v) { return uint8_t((v << 2) | (v >> 4)); }

// Decodes the 8-byte color part of a BC1/BC2/BC3 block into 16 BGRA pixels.
void decodeColorBlock(const uint8_t* b, bool dxt1, uint8_t out[16][4]) {
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    uint32_t bits = rd32(b + 4);
    uint8_t pal[4][4];
    pal[0][0] = expand5(c0 & 31); pal[0][1] = expand6((c0 >> 5) & 63); pal[0][2] = expand5(c0 >> 11); pal[0][3] = 255;
    pal[1][0] = expand5(c1 & 31); pal[1][1] = expand6((c1 >> 5) & 63); pal[1][2] = expand5(c1 >> 11); pal[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = uint8_t((pal[0][i] + pal[1][i] + 1) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

void decodeDxt(Format f, const uint8_t* src, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t blockBytes = dxtBlockBytes(f);
    uint8_t px[16][4];
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = src + (size_t(by) * bw + bx) * blockBytes;
            if (f == Format::DXT1) {
                decodeColorBlock(b, true, px);
            } else {
                decodeColorBlock(b + 8, false, px);
                if (f == Format::DXT2 || f == Format::DXT3) {
                    for (int i = 0; i < 16; ++i) px[i][3] = expand4((b[i / 2] >> (4 * (i & 1))) & 15);
                } else {
                    uint8_t a[8];
                    a[0] = b[0];
                    a[1] = b[1];
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
                    } else {
                        for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
                }
            }
            for (int y = 0; y < 4; ++y) {
                uint32_t py = by * 4 + y;
                if (py >= h) break;
                for (int x = 0; x < 4; ++x) {
                    uint32_t pxx = bx * 4 + x;
                    if (pxx >= w) break;
                    std::memcpy(dst + (size_t(py) * w + pxx) * 4, px[y * 4 + x], 4);
                }
            }
        }
    }
}

void decodeUncompressed(const Info& info, const uint8_t* src, uint32_t w, uint32_t h, uint8_t* dst) {
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* d = dst + i * 4;
        switch (info.format) {
            case Format::ARGB8888:
                std::memcpy(d, src + i * 4, 4);
                break;
            case Format::ARGB4444: {
                uint16_t v = rd16(src + i * 2);
                d[0] = expand4(v & 15); d[1] = expand4((v >> 4) & 15); d[2] = expand4((v >> 8) & 15); d[3] = expand4(v >> 12);
                break;
            }
            case Format::ARGB1555: {
                uint16_t v = rd16(src + i * 2);
                d[0] = expand5(v & 31); d[1] = expand5((v >> 5) & 31); d[2] = expand5((v >> 10) & 31); d[3] = (v & 0x8000) ? 255 : 0;
                break;
            }
            case Format::AI88:
                d[0] = d[1] = d[2] = src[i * 2];
                d[3] = src[i * 2 + 1];
                break;
            case Format::P8: {
                size_t idx = size_t(src[i]) * 3;
                if (idx + 3 <= info.palette.size()) std::memcpy(d, &info.palette[idx], 3);
                else d[0] = d[1] = d[2] = 0;
                d[3] = 255;
                break;
            }
            default:
                break;
        }
    }
}

// Undoes the TexConvert channel swizzle ("file (physical) = memory (logical)"). Each slot of the
// tag (A, R, G, B) names the source channel of that stored channel, so it is copied back there.
// Constant slots (1 / 0) carry no information and are left alone.
void unswizzle(const uint8_t sw[4], uint8_t* px, size_t count) {
    static const int slotToBgra[4] = {3, 2, 1, 0};  // A, R, G, B -> BGRA index
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = px + i * 4;
        uint8_t src[4];
        std::memcpy(src, p, 4);
        for (int slot = 0; slot < 4; ++slot) {
            uint8_t cmd = sw[slot];
            if (cmd == slot) continue;  // identity (A=0, R=1, G=2, B=3)
            uint8_t v = src[slotToBgra[slot]];
            if (cmd <= SW_INV_B) {
                if (cmd >= SW_INV_A) v = uint8_t(255 - v);
                p[slotToBgra[cmd & 3]] = v;
            }
        }
    }
}

}  // namespace

bool Info::hasAlpha() const {
    return alphaFlag != 0 || (format != Format::DXT1 && format != Format::P8);
}

const char* formatName(Format f) {
    switch (f) {
        case Format::DXT1: return "DXT1";
        case Format::DXT2: return "DXT2";
        case Format::DXT3: return "DXT3";
        case Format::DXT4: return "DXT4";
        case Format::DXT5: return "DXT5";
        case Format::ARGB4444: return "ARGB4444";
        case Format::ARGB1555: return "ARGB1555";
        case Format::ARGB8888: return "ARGB8888";
        case Format::AI88: return "AI88";
        case Format::P8: return "P8";
    }
    return "?";
}

bool lzoDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    size_t ip = 0, op = 0;
    unsigned state = 0;

    auto need = [&](size_t n) { return ip + n <= srcSize; };
    auto literals = [&](size_t n) {
        if (!need(n) || op + n > dstSize) return false;
        std::memcpy(dst + op, src + ip, n);
        ip += n;
        op += n;
        return true;
    };
    auto match = [&](size_t dist, size_t len) {
        if (dist == 0 || dist > op || op + len > dstSize) return false;
        for (size_t i = 0; i < len; ++i, ++op) dst[op] = dst[op - dist];  // may overlap
        return true;
    };
    // Length with zero-byte extension: base + (bits ? bits : mask + 255 * zeros + next).
    auto extLength = [&](unsigned bits, unsigned mask, size_t& len) {
        if (bits) { len = bits; return true; }
        size_t l = 0;
        for (;;) {
            if (!need(1)) return false;
            uint8_t b = src[ip++];
            if (b) { len = l + mask + b; return true; }
            l += 255;
        }
    };

    if (!need(1)) return false;
    unsigned x = src[ip++];
    if (x > 17) {
        size_t n = x - 17;
        if (!literals(n)) return false;
        state = n < 4 ? unsigned(n) : 4;
        if (!need(1)) return false;
        x = src[ip++];
    }

    for (;;) {
        size_t len = 0, dist = 0;
        unsigned next;
        if (x <= 15) {
            if (state == 0) {
                if (!extLength(x & 15, 15, len) || !literals(len + 3)) return false;
                state = 4;
                if (!need(1)) return false;
                x = src[ip++];
                continue;
            }
            if (!need(1)) return false;
            if (state < 4) { dist = (size_t(src[ip]) << 2) + (x >> 2) + 1; len = 2; }
            else { dist = (size_t(src[ip]) << 2) + (x >> 2) + 2049; len = 3; }
            ++ip;
            next = x & 3;
        } else if (x >= 64) {
            if (!need(1)) return false;
            dist = (size_t(src[ip++]) << 3) + ((x >> 2) & 7) + 1;
            len = x >= 128 ? 5 + ((x >> 5) & 3) : 3 + ((x >> 5) & 1);
            next = x & 3;
        } else if (x >= 32) {
            if (!extLength(x & 31, 31, len) || !need(2)) return false;
            len += 2;
            uint16_t v = rd16(src + ip);
            ip += 2;
            dist = (v >> 2) + 1;
            next = v & 3;
        } else {  // 16..31
            if (!extLength(x & 7, 7, len) || !need(2)) return false;
            len += 2;
            uint16_t v = rd16(src + ip);
            ip += 2;
            dist = 16384 + (size_t(x & 8) << 11) + (v >> 2);
            next = v & 3;
            if (dist == 16384) return op == dstSize;  // end of stream
        }
        if (!match(dist, len) || !literals(next)) return false;
        state = next;
        if (!need(1)) return false;
        x = src[ip++];
    }
}

bool lzssDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize, bool* checksumOk) {
    size_t ip = 0, op = 0;
    while (op < dstSize) {
        if (ip >= srcSize) return false;
        uint8_t flags = src[ip++];
        for (int bit = 0; bit < 8 && op < dstSize; ++bit) {
            if (flags & (1 << bit)) {
                if (ip >= srcSize) return false;
                dst[op++] = src[ip++];
            } else {
                if (ip + 2 > srcSize) return false;
                size_t back = src[ip] | (size_t(src[ip + 1] & 0xF0) << 4);
                size_t len = (src[ip + 1] & 0x0F) + 3;
                ip += 2;
                for (size_t i = 0; i < len && op < dstSize; ++i, ++op) {
                    // References before the start of the buffer read as spaces.
                    dst[op] = op >= back ? dst[op - back] : 0x20;
                }
            }
        }
    }
    if (checksumOk) {
        *checksumOk = false;
        if (ip + 4 <= srcSize) {
            int32_t sum = 0;
            for (size_t i = 0; i < dstSize; ++i) sum += int8_t(dst[i]);
            *checksumOk = uint32_t(sum) == rd32(src + ip);
        }
    }
    return true;
}

bool parse(const uint8_t* data, size_t size, Info& info, std::string* error) {
    info = Info();
    if (size < 4) return fail(error, "file too small");

    size_t p = 0;
    uint16_t type = rd16(data);
    if (isKnownFormat(type)) {
        info.format = Format(type);
        p = 2;
    } else {
        info.format = Format::P8;  // .pac files start directly with the tags or the palette
    }

    while (p + 12 <= size && std::memcmp(data + p, "GGAT", 4) == 0) {
        const char* name = reinterpret_cast<const char*>(data + p + 4);
        uint32_t len = rd32(data + p + 8);
        p += 12;
        if (p + len > size) return fail(error, "truncated tag");
        const uint8_t* t = data + p;
        if (std::memcmp(name, "CGVA", 4) == 0 && len >= 4) {
            info.hasAverage = true;
            std::memcpy(info.average, t, 4);
        } else if (std::memcmp(name, "GALF", 4) == 0 && len >= 4) {
            info.alphaFlag = rd32(t);
        } else if (std::memcmp(name, "ZIWS", 4) == 0 && len >= 4) {
            info.hasSwizzle = true;
            std::memcpy(info.swizzle, t, 4);
        }
        p += len;
    }

    if (p + 2 > size) return fail(error, "truncated palette");
    uint16_t colors = rd16(data + p);
    p += 2;
    if (p + size_t(colors) * 3 > size) return fail(error, "truncated palette");
    info.palette.assign(data + p, data + p + size_t(colors) * 3);
    p += size_t(colors) * 3;
    if (info.format == Format::P8 && colors == 0) return fail(error, "unknown texture type");

    for (;;) {
        if (p + 4 > size) break;  // tolerate a missing terminator
        uint16_t w = rd16(data + p), h = rd16(data + p + 2);
        if (w == 0 && h == 0) break;
        if (p + 7 > size) return fail(error, "truncated mipmap header");
        Mipmap m;
        m.lzo = isDxt(info.format) && (w & 0x8000);
        m.width = isDxt(info.format) ? (w & 0x7FFF) : w;
        m.height = h;
        m.size = rd24(data + p + 4);
        m.offset = p + 7;
        if (m.width == 0 || m.height == 0 || m.offset + m.size > size) return fail(error, "bad mipmap");
        info.mipmaps.push_back(m);
        p = m.offset + m.size;
    }
    if (info.mipmaps.empty()) return fail(error, "no mipmaps");
    return true;
}

bool decode(const uint8_t* data, size_t size, const Info& info, size_t mip, Image& out, bool applySwizzle,
            std::string* error) {
    if (mip >= info.mipmaps.size()) return fail(error, "mipmap index out of range");
    const Mipmap& m = info.mipmaps[mip];
    if (m.offset + m.size > size) return fail(error, "bad mipmap");

    const size_t expected = rawSize(info.format, m.width, m.height);
    const uint8_t* src = data + m.offset;
    std::vector<uint8_t> unpacked;

    if (isDxt(info.format)) {
        if (m.lzo) {
            unpacked.resize(expected);
            if (!lzoDecompress(src, m.size, unpacked.data(), expected)) return fail(error, "LZO data corrupt");
            src = unpacked.data();
        } else if (m.size < expected) {
            return fail(error, "DXT data too short");
        }
    } else {
        // Non-DXT mipmaps are always LZSS packed.
        unpacked.resize(expected);
        if (!lzssDecompress(src, m.size, unpacked.data(), expected)) return fail(error, "LZSS data corrupt");
        src = unpacked.data();
    }

    out.width = m.width;
    out.height = m.height;
    out.bgra.assign(size_t(m.width) * m.height * 4, 0);
    if (isDxt(info.format)) decodeDxt(info.format, src, m.width, m.height, out.bgra.data());
    else decodeUncompressed(info, src, m.width, m.height, out.bgra.data());

    if (applySwizzle && info.hasSwizzle) unswizzle(info.swizzle, out.bgra.data(), size_t(m.width) * m.height);
    return true;
}

size_t pickMipmap(const Info& info, uint32_t minSide) {
    size_t best = 0;
    for (size_t i = 0; i < info.mipmaps.size(); ++i) {
        const Mipmap& m = info.mipmaps[i];
        if ((m.width > m.height ? m.width : m.height) >= minSide) best = i;
    }
    return best;
}

}  // namespace paa
