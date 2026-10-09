// Internal: compile-time string obfuscation for the library's OWN literals, plus a runtime store (SecureString) for
// the copies of caller text the library has to retain. Every library-owned string literal is XORed against a per-site
// pseudo-random keystream at compile time, so the plaintext never appears in the binary image, and is decrypted on
// use into a short-lived buffer that is wiped as soon as it goes out of scope. This defeats `strings` on the DLL and
// at-rest memory scans; it is obfuscation, not security (the key ships in the binary, so a debugger on the decrypt
// site still recovers the text). Toggle with DRIZZY_ENCRYPT_STRINGS (default on). Not part of the public API.
//
// The compile-time primitive (ObfMix, ObfLiteral, Decryptor, ObfEncrypt, ObfWipe, the DZ_*_SEED macros) lives in the
// PUBLIC header drizzy/secure_string.h, because the game also uses it (via DZ_ENCRYPT) to encrypt the labels it passes
// in. This header adds what stays internal: the DRIZZY_ENCRYPT_STRINGS toggle, the ObfEntry constant-table machinery,
// the runtime SecureString, and the DZ_STR / DZ_OBF macros.
#pragma once

#include "drizzy/secure_string.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#ifndef DRIZZY_ENCRYPT_STRINGS
#define DRIZZY_ENCRYPT_STRINGS 1
#endif

namespace drizzy {
namespace detail {

inline constexpr bool kObfEnabled = DRIZZY_ENCRYPT_STRINGS != 0;

// Largest decoded length (including the null) an ObfEntry table cell can hold. Table names are short identifiers.
inline constexpr unsigned kObfCap = 40;

// The library's own literals honour DRIZZY_ENCRYPT_STRINGS: a disabled build uses a zero keystream, leaving them
// readable for debugging (and so tools/check_encrypted_strings.py sees the markers reappear). The public facility in
// secure_string.h has no such switch - ObfKeyByteRaw always encrypts.
constexpr unsigned char ObfKeyByte(uint64_t seed, unsigned i) {
    if (!kObfEnabled) return 0;
    return ObfKeyByteRaw(seed, i);
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

// ---------------------------------------------------------------------------------------------------------------------
// SecureString: the runtime counterpart to DZ_STR / DZ_ENCRYPT, for the copies of caller-provided text the library has
// to keep across frames (window titles, table headers, notifications). It stores its bytes XOR-encrypted against a
// per-instance keystream (keyed from a process-random seed, so the bytes differ run to run) and hands out a plaintext
// copy only through a scoped Plain value that wipes itself the moment it goes out of scope. So a caller's label - even
// an encrypted DZ_ENCRYPT literal - never sits in the library's own memory as readable text between uses.
// ---------------------------------------------------------------------------------------------------------------------

// A per-process, per-string seed. Mixing in the address of a function-local static adds ASLR entropy, so the stored
// ciphertext is not reproducible across runs.
inline uint64_t ObfRuntimeKey() {
    static std::atomic<uint64_t> counter{0x243F6A8885A308D3ull};
    const uint64_t c = counter.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed);
    return ObfMix(c ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&counter)));
}

class SecureString {
public:
    SecureString() = default;
    SecureString(std::string_view s) { assign(s); }
    SecureString(const SecureString& o) : enc_(o.enc_), key_(o.key_) {}
    SecureString(SecureString&& o) noexcept : enc_(std::move(o.enc_)), key_(o.key_) { o.key_ = 0; }
    SecureString& operator=(const SecureString& o) {
        if (this != &o) { wipe(); enc_ = o.enc_; key_ = o.key_; }
        return *this;
    }
    SecureString& operator=(SecureString&& o) noexcept {
        if (this != &o) { wipe(); enc_ = std::move(o.enc_); key_ = o.key_; o.key_ = 0; }
        return *this;
    }
    ~SecureString() { wipe(); }

    void assign(std::string_view s) {
        wipe();
        enc_.resize(s.size());
        if (!kObfEnabled) {  // debugging build: store plaintext so a memory scan shows the text, matching DZ_STR off
            for (size_t i = 0; i < s.size(); ++i) enc_[i] = static_cast<unsigned char>(s[i]);
            key_ = 0;
            return;
        }
        key_ = ObfRuntimeKey();
        uint64_t k = key_ ^ 0xD1B54A32D192ED03ull;
        for (size_t i = 0; i < s.size(); ++i) {
            k = ObfMix(k);
            enc_[i] = static_cast<unsigned char>(static_cast<unsigned char>(s[i]) ^ static_cast<unsigned char>(k & 0xFFu));
        }
    }
    SecureString& operator=(std::string_view s) { assign(s); return *this; }

    bool empty() const { return enc_.empty(); }
    size_t size() const { return enc_.size(); }
    void clear() { wipe(); enc_.clear(); key_ = 0; }

    // A scoped plaintext decode of the string. Wiped as soon as it leaves scope: use it inline (pass view() / c_str()
    // as an argument), do not persist the pointer or the view beyond the Plain's lifetime.
    class Plain {
    public:
        explicit Plain(const SecureString& s) : n_(s.enc_.size()), buf_(new char[s.enc_.size() + 1]) {
            if (!kObfEnabled) {
                for (size_t i = 0; i < n_; ++i) buf_[i] = static_cast<char>(s.enc_[i]);
            } else {
                uint64_t k = s.key_ ^ 0xD1B54A32D192ED03ull;
                for (size_t i = 0; i < n_; ++i) {
                    k = ObfMix(k);
                    buf_[i] = static_cast<char>(static_cast<unsigned char>(s.enc_[i]) ^ static_cast<unsigned char>(k & 0xFFu));
                }
            }
            buf_[n_] = '\0';
        }
        Plain(Plain&& o) noexcept : n_(o.n_), buf_(std::move(o.buf_)) { o.n_ = 0; }
        Plain(const Plain&) = delete;
        Plain& operator=(const Plain&) = delete;
        Plain& operator=(Plain&&) = delete;
        ~Plain() {
            if (buf_) ObfWipe(buf_.get(), static_cast<unsigned>(n_ + 1));
        }

        std::string_view view() const { return std::string_view(buf_.get(), n_); }
        const char* c_str() const { return buf_.get(); }

    private:
        size_t n_;
        std::unique_ptr<char[]> buf_;
    };

    Plain decode() const { return Plain(*this); }

private:
    void wipe() {
        if (!enc_.empty()) ObfWipe(reinterpret_cast<char*>(enc_.data()), static_cast<unsigned>(enc_.size()));
    }
    std::vector<unsigned char> enc_;
    uint64_t key_ = 0;
};

}  // namespace detail
}  // namespace drizzy

#if DRIZZY_ENCRYPT_STRINGS
// Inline encrypted literal (decrypt-on-use, auto-wiped). Use as an argument, do not store the pointer.
#define DZ_STR(s) (::drizzy::detail::Decryptor<::drizzy::detail::ObfEncrypt<::drizzy::detail::ObfLiteral{s}, DZ_OBF_SEED>(), DZ_OBF_SEED>())
#else
#define DZ_STR(s) (s)
#endif

// Encrypted table cell (for constant name tables). Decode with ObfEq / ObfGet.
#define DZ_OBF(s) (::drizzy::detail::ObfMakeEntry<::drizzy::detail::ObfLiteral{s}, DZ_OBF_SEED>())
