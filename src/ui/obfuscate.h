// Internal: compile-time string obfuscation. Every library-owned string literal is XORed against a per-site
// pseudo-random keystream at compile time, so the plaintext never appears in the binary image, and is decrypted on
// use into a short-lived buffer that is wiped as soon as it goes out of scope. This defeats `strings` on the DLL and
// at-rest memory scans; it is obfuscation, not security (the key ships in the binary, so a debugger on the decrypt
// site still recovers the text). Toggle with DRIZZY_ENCRYPT_STRINGS (default on). Not part of the public API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef DRIZZY_ENCRYPT_STRINGS
#define DRIZZY_ENCRYPT_STRINGS 1
#endif

namespace drizzy {
namespace detail {

inline constexpr bool kObfEnabled = DRIZZY_ENCRYPT_STRINGS != 0;

// Largest decoded length (including the null) an ObfEntry table cell can hold. Table names are short identifiers.
inline constexpr unsigned kObfCap = 40;

// splitmix64: a cheap compile-time PRNG used to turn a per-string seed into a keystream.
constexpr uint64_t ObfMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The i-th keystream byte for a given seed. Identical in the compile-time encrypt and the runtime decrypt, so one
// undoes the other. Disabled builds use a zero keystream, leaving the plaintext readable for debugging.
constexpr unsigned char ObfKeyByte(uint64_t seed, unsigned i) {
    if (!kObfEnabled) return 0;
    uint64_t k = seed ^ 0xD1B54A32D192ED03ull;
    for (unsigned j = 0; j <= i; ++j) k = ObfMix(k);
    return static_cast<unsigned char>(k & 0xFFu);
}

// A string literal usable as a (structural) non-type template argument, so the encryption can run entirely in the
// type system and the plaintext stays out of any runtime symbol.
template <unsigned N>
struct ObfLiteral {
    char data[N]{};
    consteval ObfLiteral(const char (&s)[N]) {
        for (unsigned i = 0; i < N; ++i) data[i] = s[i];
    }
};

// Secure wipe: volatile writes the optimizer may not elide. Keeps the plaintext window to the buffer's scope.
inline void ObfWipe(char* p, unsigned n) {
    volatile char* v = p;
    for (unsigned i = 0; i < n; ++i) v[i] = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// DZ_STR: an inline, single-use encrypted literal. Decrypts on construction into its own buffer and wipes on
// destruction, so it is valid only for the full expression it appears in (pass it as an argument; never store the
// pointer). Convertible to both const char* and std::string_view.
// ---------------------------------------------------------------------------------------------------------------------
template <ObfLiteral Cipher, uint64_t Seed>
struct Decryptor {
    static constexpr unsigned N = sizeof(Cipher.data);
    char buf_[N];

    __forceinline Decryptor() {
        // Read the ciphertext through a volatile pointer so the compiler cannot fold the decrypt back into a stored
        // plaintext constant; the XOR must run at runtime against the real encrypted bytes.
        const volatile unsigned char* src = reinterpret_cast<const volatile unsigned char*>(Cipher.data);
        for (unsigned i = 0; i < N; ++i)
            buf_[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ ObfKeyByte(Seed, i));
    }
    __forceinline ~Decryptor() { ObfWipe(buf_, N); }
    Decryptor(const Decryptor&) = delete;
    Decryptor& operator=(const Decryptor&) = delete;

    __forceinline operator const char*() const { return buf_; }
    __forceinline operator std::string_view() const { return std::string_view(buf_, N - 1); }
    __forceinline const char* c_str() const { return buf_; }
};

// Encrypt a plaintext literal at compile time, yielding a ciphertext ObfLiteral. Immediate (consteval), so the
// plaintext argument is consumed in the type system and never reaches a runtime symbol.
template <ObfLiteral Plain, uint64_t Seed>
consteval auto ObfEncrypt() {
    ObfLiteral<sizeof(Plain.data)> out = Plain;
    for (unsigned i = 0; i < sizeof(Plain.data); ++i)
        out.data[i] = static_cast<char>(static_cast<unsigned char>(Plain.data[i]) ^ ObfKeyByte(Seed, i));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// ObfEntry: an encrypted literal that can live in a constant table (color/theme/var names). Decode with ObfEq (compare
// against a view) or ObfGet (a const char* valid until the next ~64 ObfGet calls, via a wiped thread-local ring).
// ---------------------------------------------------------------------------------------------------------------------
struct ObfEntry {
    unsigned char bytes[kObfCap];
    unsigned char len;  // decoded length, excluding the null
    uint64_t seed;
};

template <ObfLiteral Plain, uint64_t Seed>
consteval ObfEntry ObfMakeEntry() {
    static_assert(sizeof(Plain.data) <= kObfCap, "string too long for an ObfEntry table cell");
    ObfEntry e{};
    e.seed = Seed;
    e.len = static_cast<unsigned char>(sizeof(Plain.data) - 1);
    for (unsigned i = 0; i < sizeof(Plain.data); ++i)
        e.bytes[i] = static_cast<unsigned char>(static_cast<unsigned char>(Plain.data[i]) ^ ObfKeyByte(Seed, i));
    return e;
}

// Decode an entry into `out` (must hold len+1 bytes); returns the length. Volatile reads keep it a runtime decrypt.
inline unsigned ObfDecode(const ObfEntry& e, char* out) {
    const volatile unsigned char* src = e.bytes;
    const unsigned n = e.len;
    for (unsigned i = 0; i < n; ++i)
        out[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ ObfKeyByte(e.seed, i));
    out[n] = '\0';
    return n;
}

inline bool ObfEq(const ObfEntry& e, std::string_view s) {
    if (s.size() != e.len) return false;
    char buf[kObfCap];
    ObfDecode(e, buf);
    bool equal = true;
    for (unsigned i = 0; i < e.len; ++i) equal &= (buf[i] == s[i]);
    ObfWipe(buf, e.len);
    return equal;
}

// Decode into a rotating thread-local ring so several results can be live at once (e.g. building a list of names).
// The ring is small and continuously overwritten, so the full dictionary is never resident in plaintext.
inline const char* ObfGet(const ObfEntry& e) {
    static constexpr unsigned kSlots = 64;
    thread_local char ring[kSlots][kObfCap];
    thread_local unsigned next = 0;
    char* slot = ring[next];
    next = (next + 1) % kSlots;
    ObfDecode(e, slot);
    return slot;
}

}  // namespace detail
}  // namespace drizzy

// A per-site seed. The file and build time fix a base; __LINE__ separates sites so a string's ciphertext differs by
// where it appears and changes build to build. It must NOT use __COUNTER__: DZ_STR expands the seed twice (encrypt
// and decrypt) and the two must match, so the seed has to be stable within a line.
#define DZ_OBF_FILE_SEED (::drizzy::detail::ObfMix(sizeof(__FILE__) * 0x100000001B3ull ^ sizeof(__DATE__ __TIME__)))
#define DZ_OBF_SEED \
    (::drizzy::detail::ObfMix(DZ_OBF_FILE_SEED ^ (static_cast<uint64_t>(__LINE__) * 0x9E3779B1u)))

#if DRIZZY_ENCRYPT_STRINGS
// Inline encrypted literal (decrypt-on-use, auto-wiped). Use as an argument, do not store the pointer.
#define DZ_STR(s) (::drizzy::detail::Decryptor<::drizzy::detail::ObfEncrypt<::drizzy::detail::ObfLiteral{s}, DZ_OBF_SEED>(), DZ_OBF_SEED>())
#else
#define DZ_STR(s) (s)
#endif

// Encrypted table cell (for constant name tables). Decode with ObfEq / ObfGet.
#define DZ_OBF(s) (::drizzy::detail::ObfMakeEntry<::drizzy::detail::ObfLiteral{s}, DZ_OBF_SEED>())
