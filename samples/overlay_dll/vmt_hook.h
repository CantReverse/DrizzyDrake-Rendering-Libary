// Minimal VMT (virtual method table) hooking for the injectable overlay sample.
//
// A COM object (a swap chain, a command queue) begins with a pointer to its vtable - an array of function pointers
// shared by every object of that type in the process. Overwriting one entry redirects that method for all of them,
// including the ones the game creates later, and calling the saved original is an ordinary indirect call - no
// trampoline, no instruction-length decoding. This is the simplest reliable way to hook into your own game; it is
// not meant to defeat anti-cheat and does not try to.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

namespace vmt {

// One hooked vtable slot: remembers where it wrote and what was there, so it can be put back exactly.
class Hook {
public:
    // Replaces entry `index` of `object`'s vtable with `replacement`. Returns the original function pointer (also
    // kept, for Original<T>() and Remove()). Idempotent targets: hooking a slot already pointing at `replacement`
    // returns the previously stored original.
    void* Install(void* object, unsigned index, void* replacement) {
        if (!object) return nullptr;
        void** vtable = *reinterpret_cast<void***>(object);
        m_slot = &vtable[index];
        DWORD prot = 0;
        if (!VirtualProtect(m_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) {
            m_slot = nullptr;
            return nullptr;
        }
        if (*m_slot != replacement) m_original = *m_slot;  // don't capture our own hook as the "original"
        *m_slot = replacement;
        VirtualProtect(m_slot, sizeof(void*), prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), m_slot, sizeof(void*));
        return m_original;
    }

    void Remove() {
        if (!m_slot || !m_original) return;
        DWORD prot = 0;
        if (VirtualProtect(m_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) {
            if (*m_slot != m_original) *m_slot = m_original;  // leave a re-hook by someone else alone
            VirtualProtect(m_slot, sizeof(void*), prot, &prot);
            FlushInstructionCache(GetCurrentProcess(), m_slot, sizeof(void*));
        }
        m_slot = nullptr;
    }

    template <typename T>
    T Original() const {
        return reinterpret_cast<T>(m_original);
    }
    bool Installed() const { return m_slot != nullptr; }

private:
    void** m_slot = nullptr;
    void* m_original = nullptr;
};

} // namespace vmt
