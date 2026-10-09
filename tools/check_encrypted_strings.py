#!/usr/bin/env python3
# Verifies that drizzy's in-memory string encryption (DRIZZY_ENCRYPT_STRINGS, on by default) actually kept the
# library's own string literals out of the shipped binary. Build a Release artifact, then:
#
#     python tools/check_encrypted_strings.py build/Release/drizzy_overlay_dll.dll
#
# Exits non-zero if any library-owned plaintext is found. The markers below are strings the drizzy library defines
# itself (color/var names, serialization text, internal widget ids) and that the sample apps never type as their own
# literals, so a hit means encryption missed one -- not that a sample label leaked.
import sys

MARKERS = [
    # Color-slot names (src/ui/ui_style.cpp kColorNames)
    b"ScrollbarGrabHovered", b"ScrollbarGrabActive", b"FrameBgHovered", b"FrameBgActive",
    b"TitleBgActive", b"ModalDimBg", b"TableRowBgAlt", b"AccentText", b"TextSelectedBg",
    # Style-var names (kVarInfo / kExtraKeys)
    b"windowShadowSize", b"scrollbarRounding", b"buttonTextAlign", b"itemInnerSpacing", b"disabledAlpha",
    # Serialization text and error messages (LoadTheme / SaveTheme)
    b"# drizzy_renderer UI theme", b"# drizzy UI window layout",
    b"expected #RRGGBB or #RRGGBBAA", b"unknown setting", b"expected 'name = value'",
    # Internal widget ids / formats
    b"##MainMenuBar", b"##Popup_%08X", b"#SCROLLY", b"#COLLAPSE", b"##table_%08X",
    b"Click to restore the original color", b"include,-exclude",
]


def check(path):
    data = open(path, "rb").read()
    leaks = [m for m in MARKERS if m in data]
    print("%s (%d bytes): %s" % (path, len(data),
                                 "PASS" if not leaks else "FAIL (%d leaked)" % len(leaks)))
    for m in leaks:
        print("  LEAK: %s" % m.decode())
    return leaks


def main(argv):
    if len(argv) < 2:
        print("usage: check_encrypted_strings.py <binary> [<binary> ...]")
        return 2
    total = sum(len(check(p)) for p in argv[1:])
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
