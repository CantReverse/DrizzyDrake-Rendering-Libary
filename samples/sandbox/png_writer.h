// Minimal dependency-free PNG writer for sandbox screenshots. Uses stored (uncompressed) deflate blocks: files are
// larger than a real encoder would produce, but the code is tiny and the output is valid PNG.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace sandbox {

inline uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

// rgba: 8-bit RGBA rows, `rowPitch` bytes apart.
inline bool WritePng(const char* path, uint32_t width, uint32_t height, const uint8_t* rgba, size_t rowPitch) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        const uint8_t* row = rgba + y * rowPitch;
        raw.insert(raw.end(), row, row + size_t(width) * 4);
    }

    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));

    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "wb") != 0) f = nullptr;
#else
    f = std::fopen(path, "wb");
#endif
    if (!f) return false;

    auto put32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
    };
    auto writeChunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> chunk;
        put32(chunk, uint32_t(data.size()));
        chunk.insert(chunk.end(), type, type + 4);
        chunk.insert(chunk.end(), data.begin(), data.end());
        put32(chunk, Crc32(chunk.data() + 4, chunk.size() - 4) ^ 0xFFFFFFFFu);
        std::fwrite(chunk.data(), 1, chunk.size(), f);
    };

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(signature, 1, sizeof(signature), f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit, RGBA, deflate, no filter, no interlace
    writeChunk("IHDR", ihdr);
    writeChunk("IDAT", z);
    writeChunk("IEND", {});
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

// Writes an RGBA8 image with every alpha forced to 255 (render targets keep blend results in alpha).
inline bool WriteOpaquePng(const char* path, uint32_t width, uint32_t height, const uint8_t* rgba, size_t rowPitch) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = rgba + y * rowPitch;
        uint8_t* dst = &pixels[size_t(y) * width * 4];
        for (uint32_t x = 0; x < width * 4; ++x) dst[x] = (x & 3) == 3 ? 255 : src[x];
    }
    return WritePng(path, width, height, pixels.data(), size_t(width) * 4);
}

} // namespace sandbox
