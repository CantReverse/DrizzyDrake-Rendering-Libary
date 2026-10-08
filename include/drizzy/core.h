// drizzy_renderer - core types shared by every layer.
//
// Everything under include/drizzy except the backend_*.h headers must stay free of OS and graphics API includes, so
// the core can be driven by any host (Unity, Unreal, a standalone app) without touching a windowing API.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#ifndef DZ_ASSERT
#include <cassert>
#define DZ_ASSERT(expr) assert(expr)
#endif

#if defined(_MSC_VER)
#define DZ_FORCEINLINE __forceinline
#define DZ_NOINLINE __declspec(noinline)
#else
#define DZ_FORCEINLINE inline __attribute__((always_inline))
#define DZ_NOINLINE __attribute__((noinline))
#endif

namespace drizzy {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
constexpr Vec2 operator*(float s, Vec2 a) { return {a.x * s, a.y * s}; }
constexpr Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }
constexpr Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
constexpr Vec2& operator+=(Vec2& a, Vec2 b) { a.x += b.x; a.y += b.y; return a; }
constexpr Vec2& operator-=(Vec2& a, Vec2 b) { a.x -= b.x; a.y -= b.y; return a; }
constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

constexpr float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr float Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
constexpr float LengthSq(Vec2 v) { return Dot(v, v); }
inline float Length(Vec2 v) { return std::sqrt(Dot(v, v)); }
constexpr float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr Vec2 Lerp(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
constexpr Vec2 MinV(Vec2 a, Vec2 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}; }
constexpr Vec2 MaxV(Vec2 a, Vec2 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}; }

// Axis-aligned rectangle in pixels. Origin is the top-left of the render target, +y points down.
struct Rect {
    Vec2 min, max;

    constexpr Rect() = default;
    constexpr Rect(Vec2 min_, Vec2 max_) : min(min_), max(max_) {}
    constexpr Rect(float x0, float y0, float x1, float y1) : min(x0, y0), max(x1, y1) {}
    static constexpr Rect FromPosSize(Vec2 pos, Vec2 size) { return {pos, pos + size}; }
    static constexpr Rect FromCenter(Vec2 center, Vec2 halfSize) { return {center - halfSize, center + halfSize}; }

    constexpr float Width() const { return max.x - min.x; }
    constexpr float Height() const { return max.y - min.y; }
    constexpr Vec2 Size() const { return max - min; }
    constexpr Vec2 Center() const { return (min + max) * 0.5f; }
    constexpr bool Contains(Vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
    constexpr bool Overlaps(const Rect& r) const {
        return r.min.x < max.x && r.max.x > min.x && r.min.y < max.y && r.max.y > min.y;
    }
    constexpr Rect Expanded(float amount) const {
        return {min.x - amount, min.y - amount, max.x + amount, max.y + amount};
    }
    constexpr Rect Translated(Vec2 d) const { return {min + d, max + d}; }
    // Returns the overlap of both rects; an empty (zero-area) rect if they do not overlap.
    constexpr Rect Intersect(const Rect& r) const {
        const Vec2 mn = MaxV(min, r.min);
        return {mn, MaxV(mn, MinV(max, r.max))};
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// 3D math (used only by the world-space debug drawing in debug_draw.h; the 2D UI never needs it)
// ---------------------------------------------------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(Vec3 v) {
    const float len2 = Dot(v, v);
    return len2 > 1e-12f ? v * (1.0f / std::sqrt(len2)) : Vec3{};
}

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// 4x4 matrix, row-major storage: m[row][col]. Transform treats points as column vectors (clip = M * v), which matches
// DirectXMath / HLSL mul(M, v). Feed a view*projection matrix. Use FromColumnMajor for engines that store transposed.
struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Mat4 FromRowMajor(const float* v) {
        Mat4 r;
        std::memcpy(r.m, v, sizeof(r.m));
        return r;
    }
    static Mat4 FromColumnMajor(const float* v) {
        Mat4 r;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) r.m[row][col] = v[col * 4 + row];
        return r;
    }
    Mat4 Transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i][j] = m[j][i];
        return r;
    }
};

constexpr Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w,
            a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w};
}
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

constexpr bool operator==(const Rect& a, const Rect& b) { return a.min == b.min && a.max == b.max; }

// ---------------------------------------------------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------------------------------------------------
// Packed 8-bit RGBA: sRGB-encoded color, straight (non-premultiplied) alpha. R is the low byte, so the in-memory byte
// order matches R8G8B8A8 texture formats.
using Color = uint32_t;

constexpr Color Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
// Hex(0xFF8800) -> orange. Alpha is passed separately.
constexpr Color Hex(uint32_t rgb, uint32_t a = 255) { return Rgba(rgb >> 16, rgb >> 8, rgb, a); }
inline Color RgbaF(float r, float g, float b, float a = 1.0f) {
    auto to8 = [](float v) { return uint32_t(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return Rgba(to8(r), to8(g), to8(b), to8(a));
}
constexpr uint32_t ColorAlpha(Color c) { return c >> 24; }
constexpr Color WithAlpha(Color c, uint32_t a) { return (c & 0x00FFFFFFu) | ((a & 0xFFu) << 24); }
inline Color ScaleAlpha(Color c, float scale) {
    return WithAlpha(c, uint32_t(float(ColorAlpha(c)) * Clamp(scale, 0.0f, 1.0f) + 0.5f));
}

namespace colors {
constexpr Color Transparent = 0;
constexpr Color White = Rgba(255, 255, 255);
constexpr Color Black = Rgba(0, 0, 0);
} // namespace colors

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------
// Opaque handle to a backend texture. The D3D11 backend stores an ID3D11ShaderResourceView*. 0 means "no texture".
using TextureId = uint64_t;
constexpr TextureId kNoTexture = 0;

// ---------------------------------------------------------------------------------------------------------------------
// PodArray: growable array for trivially copyable types.
// Never shrinks, so per-frame containers stop allocating once they reach their high-water mark. Append() hands out
// uninitialized storage for the hot paths that fill elements in place.
// ---------------------------------------------------------------------------------------------------------------------
template <typename T>
class PodArray {
    static_assert(std::is_trivially_copyable_v<T>, "PodArray only holds trivially copyable types");

public:
    PodArray() = default;
    ~PodArray() { std::free(m_data); }
    PodArray(const PodArray&) = delete;
    PodArray& operator=(const PodArray&) = delete;
    PodArray(PodArray&& o) noexcept : m_data(o.m_data), m_size(o.m_size), m_capacity(o.m_capacity) {
        o.m_data = nullptr;
        o.m_size = o.m_capacity = 0;
    }
    PodArray& operator=(PodArray&& o) noexcept {
        if (this != &o) {
            std::free(m_data);
            m_data = o.m_data;
            m_size = o.m_size;
            m_capacity = o.m_capacity;
            o.m_data = nullptr;
            o.m_size = o.m_capacity = 0;
        }
        return *this;
    }

    uint32_t Size() const { return m_size; }
    uint32_t Capacity() const { return m_capacity; }
    bool Empty() const { return m_size == 0; }
    T* Data() { return m_data; }
    const T* Data() const { return m_data; }
    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

    T& operator[](uint32_t i) { DZ_ASSERT(i < m_size); return m_data[i]; }
    const T& operator[](uint32_t i) const { DZ_ASSERT(i < m_size); return m_data[i]; }
    T& Back() { DZ_ASSERT(m_size > 0); return m_data[m_size - 1]; }
    const T& Back() const { DZ_ASSERT(m_size > 0); return m_data[m_size - 1]; }

    void Clear() { m_size = 0; }
    void PopBack() { DZ_ASSERT(m_size > 0); --m_size; }

    void Reserve(uint32_t capacity) {
        if (capacity <= m_capacity) return;
        T* data = static_cast<T*>(std::realloc(m_data, size_t(capacity) * sizeof(T)));
        DZ_ASSERT(data && "out of memory");
        m_data = data;
        m_capacity = capacity;
    }
    // New elements are uninitialized.
    void Resize(uint32_t size) {
        Reserve(size);
        m_size = size;
    }

    DZ_FORCEINLINE T& PushBack(const T& value) {
        if (m_size == m_capacity) {
            const T copy = value;  // value may live inside this array
            Grow(m_size + 1);
            m_data[m_size] = copy;
        } else {
            m_data[m_size] = value;
        }
        return m_data[m_size++];
    }
    // Appends `count` uninitialized elements and returns a pointer to the first one.
    DZ_FORCEINLINE T* Append(uint32_t count) {
        if (m_size + count > m_capacity) Grow(m_size + count);
        T* p = m_data + m_size;
        m_size += count;
        return p;
    }

private:
    void Grow(uint32_t minCapacity) {
        uint32_t capacity = m_capacity ? m_capacity * 2 : 64;
        if (capacity < minCapacity) capacity = minCapacity;
        Reserve(capacity);
    }

    T* m_data = nullptr;
    uint32_t m_size = 0;
    uint32_t m_capacity = 0;
};

} // namespace drizzy
