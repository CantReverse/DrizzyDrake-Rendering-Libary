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
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
// secure_string.h has no such switch - its ObfKeystream always encrypts.
//
// XORs `n` bytes of `src` into `dst` with the keystream of `seed`: encrypts plaintext, decrypts ciphertext. Linear in
// `n` (one mix per eight bytes). `src` is read through a volatile pointer so the decrypt of a constant stays a runtime
// operation instead of being folded back into a stored plaintext.
inline void ObfXor(char* dst, const volatile unsigned char* src, size_t n, uint64_t seed) {
    if (!kObfEnabled) {
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<char>(src[i]);
        return;
    }
    ObfKeystream key(seed);
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ key.Next());
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
    ObfKeystream key(Seed);
    for (unsigned i = 0; i < sizeof(Plain.data); ++i) {
        const unsigned char k = key.Next();
        e.bytes[i] = static_cast<unsigned char>(static_cast<unsigned char>(Plain.data[i]) ^ (kObfEnabled ? k : 0u));
    }
    return e;
}

// Decode an entry into `out` (must hold len+1 bytes); returns the length.
inline unsigned ObfDecode(const ObfEntry& e, char* out) {
    const unsigned n = e.len;
    ObfXor(out, e.bytes, n, e.seed);
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
//
// Decoding never touches the heap: the memory a Plain decodes into is reserved next to the ciphertext when the text is
// assigned, so a frame that only reads (draws a title, a header, a notification) allocates nothing. Assigning the text
// it already holds (a table header set up every frame) is a no-op. Not thread-safe, like the Ui that owns it, and it
// must not be assigned, moved or destroyed while a Plain of it is alive.
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
    SecureString(const SecureString& o) { copyFrom(o); }
    SecureString(SecureString&& o) noexcept : buf_(std::move(o.buf_)), size_(o.size_), key_(o.key_) {
        assert(o.live_ == 0 && "SecureString moved while decoded");
        o.size_ = 0;
        o.key_ = 0;
    }
    SecureString& operator=(const SecureString& o) {
        if (this != &o) {
            wipe();
            copyFrom(o);
        }
        return *this;
    }
    SecureString& operator=(SecureString&& o) noexcept {
        if (this != &o) {
            assert(live_ == 0 && o.live_ == 0 && "SecureString moved while decoded");
            wipe();
            buf_ = std::move(o.buf_);
            size_ = o.size_;
            key_ = o.key_;
            o.size_ = 0;
            o.key_ = 0;
        }
        return *this;
    }
    ~SecureString() {
        assert(live_ == 0 && "SecureString destroyed while decoded");
        wipe();
    }

    void assign(std::string_view s) {
        assert(live_ == 0 && "SecureString assigned while decoded");
        if (equals(s)) return;  // the text it already holds: nothing to re-encrypt
        wipe();
        size_ = s.size();
        // The ciphertext, then the room a Plain decodes into (and its null). Zero-filled by wipe() or the resize, and
        // reused without reallocating while the text does not grow.
        buf_.resize(size_ ? size_ * 2 + 1 : 0);
        // A disabled build keeps the plaintext (key 0, a copy), so a memory scan shows it, matching DZ_STR off.
        key_ = kObfEnabled ? ObfRuntimeKey() : 0;
        ObfXor(buf_.data(), reinterpret_cast<const unsigned char*>(s.data()), size_, key_);
    }
    SecureString& operator=(std::string_view s) { assign(s); return *this; }

    // Whether it holds `s`, found by encrypting `s` against the stored ciphertext: no plaintext copy is made.
    bool equals(std::string_view s) const {
        if (s.size() != size_) return false;
        if (!kObfEnabled) return size_ == 0 || std::memcmp(buf_.data(), s.data(), size_) == 0;
        ObfKeystream key(key_);
        unsigned char diff = 0;
        for (size_t i = 0; i < size_; ++i) {
            diff |= static_cast<unsigned char>(static_cast<unsigned char>(buf_[i]) ^ static_cast<unsigned char>(s[i]) ^
                                               key.Next());
        }
        return diff == 0;
    }

    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    void clear() {
        assert(live_ == 0 && "SecureString cleared while decoded");
        wipe();
        buf_.clear();
        size_ = 0;
        key_ = 0;
    }

    // A scoped plaintext decode of the string. Wiped as soon as the last Plain of the string leaves scope: use it inline
    // (pass view() / c_str() as an argument), do not persist the pointer or the view beyond the Plain's lifetime.
    // Plains of the same string share one decode, so nesting them is fine.
    class Plain {
    public:
        explicit Plain(const SecureString& s) : s_(&s) {
            if (s.live_++ == 0 && s.size_) {
                ObfXor(s.Scratch(), reinterpret_cast<const unsigned char*>(s.buf_.data()), s.size_, s.key_);
            }
        }
        Plain(Plain&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
        Plain(const Plain&) = delete;
        Plain& operator=(const Plain&) = delete;
        Plain& operator=(Plain&&) = delete;
        ~Plain() {
            if (s_ && --s_->live_ == 0 && s_->size_) ObfWipe(s_->Scratch(), static_cast<unsigned>(s_->size_));
        }

        std::string_view view() const { return std::string_view(c_str(), s_->size_); }
        const char* c_str() const { return s_->size_ ? s_->Scratch() : ""; }

    private:
        const SecureString* s_;
    };

    Plain decode() const { return Plain(*this); }

private:
    char* Scratch() const { return buf_.data() + size_; }
    void copyFrom(const SecureString& o) {  // the ciphertext only, never a live decode
        buf_.assign(o.buf_.size(), '\0');
        if (o.size_) std::memcpy(buf_.data(), o.buf_.data(), o.size_);
        size_ = o.size_;
        key_ = o.key_;
    }
    void wipe() {
        if (!buf_.empty()) ObfWipe(buf_.data(), static_cast<unsigned>(buf_.size()));
    }
    mutable std::vector<char> buf_;  // [ciphertext | decode scratch | null]; Plain writes the scratch
    size_t size_ = 0;
    uint64_t key_ = 0;
    mutable uint32_t live_ = 0;  // Plains alive
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
