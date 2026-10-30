// PAA (Arma / Bohemia Interactive texture) decoder.
// Written from the public format description; no third-party code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace paa {

enum class Format : uint16_t {
    DXT1 = 0xFF01,
    DXT2 = 0xFF02,
    DXT3 = 0xFF03,
    DXT4 = 0xFF04,
    DXT5 = 0xFF05,
    ARGB4444 = 0x4444,
    ARGB1555 = 0x1555,
    ARGB8888 = 0x8888,
    AI88 = 0x8080,
    P8 = 0x0000,  // indexed (.pac), no type word in the file
};

// One channel command of the SWIZ tag (TexConvert "channelSwizzle").
enum Swizzle : uint8_t {
    SW_A = 0, SW_R = 1, SW_G = 2, SW_B = 3,
    SW_INV_A = 4, SW_INV_R = 5, SW_INV_G = 6, SW_INV_B = 7,
    SW_ONE = 8, SW_ZERO = 9,
};

struct Mipmap {
    uint32_t width = 0;
    uint32_t height = 0;
    bool lzo = false;
    size_t offset = 0;  // start of the (possibly compressed) data in the file
    size_t size = 0;    // size of that data
};

struct Info {
    Format format = Format::DXT1;
    std::vector<Mipmap> mipmaps;
    std::vector<uint8_t> palette;  // BGR triplets (P8 only)
    bool hasAverage = false;
    uint8_t average[4] = {};       // B, G, R, A
    uint32_t alphaFlag = 0;        // 0 none, 1 interpolated, 2 binary
    bool hasSwizzle = false;
    uint8_t swizzle[4] = {SW_A, SW_R, SW_G, SW_B};  // file order: A, R, G, B

    bool hasAlpha() const;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // top-down, straight alpha, stride = width * 4
};

const char* formatName(Format f);

// Parses the header, tags and mipmap table. Pixel data is not touched.
bool parse(const uint8_t* data, size_t size, Info& info, std::string* error = nullptr);

// Decodes one mipmap to BGRA. applySwizzle undoes the SWIZ channel shuffle like TexView does.
bool decode(const uint8_t* data, size_t size, const Info& info, size_t mip, Image& out,
            bool applySwizzle = true, std::string* error = nullptr);

// Smallest mipmap whose largest side is still >= minSide (0 when none is big enough).
size_t pickMipmap(const Info& info, uint32_t minSide);

// Low-level codecs, exposed for tests.
bool lzoDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize);
bool lzssDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize,
                    bool* checksumOk = nullptr);

}  // namespace paa
