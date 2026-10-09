// drizzy_renderer - compile-time string encryption for the labels YOUR game passes to drizzy.
//
// The library encrypts its own string literals in the binary (see DRIZZY_ENCRYPT_STRINGS). The labels your game hands
// to drizzy - window titles, button text, menu entries - live in YOUR binary, so `strings` on your game and a memory
// scan at rest reveal them unless you encrypt them too. Wrap such a literal in DZ_ENCRYPT and it is XORed against a
// per-site keystream at compile time (so the plaintext never appears in your binary image) and decrypted only for the
// duration of the call, into a small buffer that is wiped the moment it goes out of scope:
//
//     if (ui.Begin(DZ_ENCRYPT("Settings"))) {
//         if (ui.Button(DZ_ENCRYPT("Apply"))) Apply();
//         ui.Text(DZ_ENCRYPT("Teleport"));
//     }
//
// DZ_ENCRYPT yields a value convertible to both std::string_view and const char*, so it passes straight to any drizzy
// call that takes a label. Use it inline as an argument; never store the pointer (it is wiped at the end of the full
// expression). Pass only string LITERALS: text your game builds at runtime (std::format, a player name, typed input)
// cannot be encrypted at compile time, and your own code holds it in plaintext before it reaches drizzy - DZ_ENCRYPT
// cannot help there. The library itself no longer keeps a plaintext copy of a label after the call, so an encrypted
// literal stays encrypted at rest on both sides.
//
// This is obfuscation, not security: the key ships inside your binary, so it raises the cost of static reverse
// engineering and defeats casual memory scans, but a debugger on the decrypt site can still recover the text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(_MSC_VER)
#define DZ_FORCEINLINE __forceinline
#else
#define DZ_FORCEINLINE inline
#endif

namespace drizzy {
namespace detail {

// splitmix64: a cheap compile-time PRNG that turns a per-string seed into a keystream.
constexpr uint64_t ObfMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The i-th keystream byte for a seed. Always encrypts: this facility is opt-in by writing DZ_ENCRYPT, so there is no
// build switch that turns it into a no-op (unlike the library's own DRIZZY_ENCRYPT_STRINGS). Identical in the
// compile-time encrypt and the runtime decrypt, so one undoes the other.
constexpr unsigned char ObfKeyByteRaw(uint64_t seed, unsigned i) {
    uint64_t k = seed ^ 0xD1B54A32D192ED03ull;
    for (unsigned j = 0; j <= i; ++j) k = ObfMix(k);
    return static_cast<unsigned char>(k & 0xFFu);
}

// Secure wipe: volatile writes the optimizer may not elide. Keeps the plaintext window to the buffer's scope.
inline void ObfWipe(char* p, unsigned n) {
    volatile char* v = p;
    for (unsigned i = 0; i < n; ++i) v[i] = 0;
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

// Encrypt a plaintext literal at compile time, yielding a ciphertext ObfLiteral. Immediate (consteval), so the
// plaintext argument is consumed in the type system and never reaches a runtime symbol.
template <ObfLiteral Plain, uint64_t Seed>
consteval auto ObfEncrypt() {
    ObfLiteral<sizeof(Plain.data)> out = Plain;
    for (unsigned i = 0; i < sizeof(Plain.data); ++i)
        out.data[i] = static_cast<char>(static_cast<unsigned char>(Plain.data[i]) ^ ObfKeyByteRaw(Seed, i));
    return out;
}

// An inline, single-use encrypted literal. Decrypts on construction into its own buffer and wipes on destruction, so
// it is valid only for the full expression it appears in (pass it as an argument; never store the pointer).
// Convertible to both const char* and std::string_view.
template <ObfLiteral Cipher, uint64_t Seed>
struct Decryptor {
    static constexpr unsigned N = sizeof(Cipher.data);
    char buf_[N];

    DZ_FORCEINLINE Decryptor() {
        // Read the ciphertext through a volatile pointer so the compiler cannot fold the decrypt back into a stored
        // plaintext constant; the XOR must run at runtime against the real encrypted bytes.
        const volatile unsigned char* src = reinterpret_cast<const volatile unsigned char*>(Cipher.data);
        for (unsigned i = 0; i < N; ++i)
            buf_[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ ObfKeyByteRaw(Seed, i));
    }
    DZ_FORCEINLINE ~Decryptor() { ObfWipe(buf_, N); }
    Decryptor(const Decryptor&) = delete;
    Decryptor& operator=(const Decryptor&) = delete;

    DZ_FORCEINLINE operator const char*() const { return buf_; }
    DZ_FORCEINLINE operator std::string_view() const { return std::string_view(buf_, N - 1); }
    DZ_FORCEINLINE const char* c_str() const { return buf_; }
};

}  // namespace detail
}  // namespace drizzy

// A per-site seed. The file and build time fix a base; __LINE__ separates sites so a string's ciphertext differs by
// where it appears and changes build to build. It must NOT use __COUNTER__: the encrypt macro expands the seed twice
// (encrypt and decrypt) and the two must match, so the seed has to be stable within a line.
#ifndef DZ_OBF_FILE_SEED
#define DZ_OBF_FILE_SEED (::drizzy::detail::ObfMix(sizeof(__FILE__) * 0x100000001B3ull ^ sizeof(__DATE__ __TIME__)))
#endif
#ifndef DZ_OBF_SEED
#define DZ_OBF_SEED \
    (::drizzy::detail::ObfMix(DZ_OBF_FILE_SEED ^ (static_cast<uint64_t>(__LINE__) * 0x9E3779B1u)))
#endif

// Encrypt a string literal you pass to drizzy. Decrypts on use into an auto-wiped buffer; use inline, do not store the
// pointer. See the file header for the full contract. Pass only string literals.
#define DZ_ENCRYPT(s) \
    (::drizzy::detail::Decryptor<::drizzy::detail::ObfEncrypt<::drizzy::detail::ObfLiteral{s}, DZ_OBF_SEED>(), DZ_OBF_SEED>())
