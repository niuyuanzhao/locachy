/* FreeType content fonts, independent of screen.c's system text and bar. */
#include "font.h"
#include "base.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

struct unicode_range { uint32_t first, last; };

/* Unicode 17.0.0 general categories M/Cf and assigned East Asian W/F.
 * Hangul medial/final jamo and emoji skin-tone modifiers also combine.
 * Generated from the Unicode Character Database; independent of LC_CTYPE. */
static const struct unicode_range zero_width[] = {
    { 0xAD, 0xAD }, { 0x300, 0x36F }, { 0x483, 0x489 },
    { 0x591, 0x5BD }, { 0x5BF, 0x5BF }, { 0x5C1, 0x5C2 },
    { 0x5C4, 0x5C5 }, { 0x5C7, 0x5C7 }, { 0x600, 0x605 },
    { 0x610, 0x61A }, { 0x61C, 0x61C }, { 0x64B, 0x65F },
    { 0x670, 0x670 }, { 0x6D6, 0x6DD }, { 0x6DF, 0x6E4 },
    { 0x6E7, 0x6E8 }, { 0x6EA, 0x6ED }, { 0x70F, 0x70F },
    { 0x711, 0x711 }, { 0x730, 0x74A }, { 0x7A6, 0x7B0 },
    { 0x7EB, 0x7F3 }, { 0x7FD, 0x7FD }, { 0x816, 0x819 },
    { 0x81B, 0x823 }, { 0x825, 0x827 }, { 0x829, 0x82D },
    { 0x859, 0x85B }, { 0x890, 0x891 }, { 0x897, 0x89F },
    { 0x8CA, 0x903 }, { 0x93A, 0x93C }, { 0x93E, 0x94F },
    { 0x951, 0x957 }, { 0x962, 0x963 }, { 0x981, 0x983 },
    { 0x9BC, 0x9BC }, { 0x9BE, 0x9C4 }, { 0x9C7, 0x9C8 },
    { 0x9CB, 0x9CD }, { 0x9D7, 0x9D7 }, { 0x9E2, 0x9E3 },
    { 0x9FE, 0x9FE }, { 0xA01, 0xA03 }, { 0xA3C, 0xA3C },
    { 0xA3E, 0xA42 }, { 0xA47, 0xA48 }, { 0xA4B, 0xA4D },
    { 0xA51, 0xA51 }, { 0xA70, 0xA71 }, { 0xA75, 0xA75 },
    { 0xA81, 0xA83 }, { 0xABC, 0xABC }, { 0xABE, 0xAC5 },
    { 0xAC7, 0xAC9 }, { 0xACB, 0xACD }, { 0xAE2, 0xAE3 },
    { 0xAFA, 0xAFF }, { 0xB01, 0xB03 }, { 0xB3C, 0xB3C },
    { 0xB3E, 0xB44 }, { 0xB47, 0xB48 }, { 0xB4B, 0xB4D },
    { 0xB55, 0xB57 }, { 0xB62, 0xB63 }, { 0xB82, 0xB82 },
    { 0xBBE, 0xBC2 }, { 0xBC6, 0xBC8 }, { 0xBCA, 0xBCD },
    { 0xBD7, 0xBD7 }, { 0xC00, 0xC04 }, { 0xC3C, 0xC3C },
    { 0xC3E, 0xC44 }, { 0xC46, 0xC48 }, { 0xC4A, 0xC4D },
    { 0xC55, 0xC56 }, { 0xC62, 0xC63 }, { 0xC81, 0xC83 },
    { 0xCBC, 0xCBC }, { 0xCBE, 0xCC4 }, { 0xCC6, 0xCC8 },
    { 0xCCA, 0xCCD }, { 0xCD5, 0xCD6 }, { 0xCE2, 0xCE3 },
    { 0xCF3, 0xCF3 }, { 0xD00, 0xD03 }, { 0xD3B, 0xD3C },
    { 0xD3E, 0xD44 }, { 0xD46, 0xD48 }, { 0xD4A, 0xD4D },
    { 0xD57, 0xD57 }, { 0xD62, 0xD63 }, { 0xD81, 0xD83 },
    { 0xDCA, 0xDCA }, { 0xDCF, 0xDD4 }, { 0xDD6, 0xDD6 },
    { 0xDD8, 0xDDF }, { 0xDF2, 0xDF3 }, { 0xE31, 0xE31 },
    { 0xE34, 0xE3A }, { 0xE47, 0xE4E }, { 0xEB1, 0xEB1 },
    { 0xEB4, 0xEBC }, { 0xEC8, 0xECE }, { 0xF18, 0xF19 },
    { 0xF35, 0xF35 }, { 0xF37, 0xF37 }, { 0xF39, 0xF39 },
    { 0xF3E, 0xF3F }, { 0xF71, 0xF84 }, { 0xF86, 0xF87 },
    { 0xF8D, 0xF97 }, { 0xF99, 0xFBC }, { 0xFC6, 0xFC6 },
    { 0x102B, 0x103E }, { 0x1056, 0x1059 }, { 0x105E, 0x1060 },
    { 0x1062, 0x1064 }, { 0x1067, 0x106D }, { 0x1071, 0x1074 },
    { 0x1082, 0x108D }, { 0x108F, 0x108F }, { 0x109A, 0x109D },
    { 0x1160, 0x11FF }, { 0x135D, 0x135F }, { 0x1712, 0x1715 },
    { 0x1732, 0x1734 }, { 0x1752, 0x1753 }, { 0x1772, 0x1773 },
    { 0x17B4, 0x17D3 }, { 0x17DD, 0x17DD }, { 0x180B, 0x180F },
    { 0x1885, 0x1886 }, { 0x18A9, 0x18A9 }, { 0x1920, 0x192B },
    { 0x1930, 0x193B }, { 0x1A17, 0x1A1B }, { 0x1A55, 0x1A5E },
    { 0x1A60, 0x1A7C }, { 0x1A7F, 0x1A7F }, { 0x1AB0, 0x1ADD },
    { 0x1AE0, 0x1AEB }, { 0x1B00, 0x1B04 }, { 0x1B34, 0x1B44 },
    { 0x1B6B, 0x1B73 }, { 0x1B80, 0x1B82 }, { 0x1BA1, 0x1BAD },
    { 0x1BE6, 0x1BF3 }, { 0x1C24, 0x1C37 }, { 0x1CD0, 0x1CD2 },
    { 0x1CD4, 0x1CE8 }, { 0x1CED, 0x1CED }, { 0x1CF4, 0x1CF4 },
    { 0x1CF7, 0x1CF9 }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F },
    { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x2066, 0x206F },
    { 0x20D0, 0x20F0 }, { 0x2CEF, 0x2CF1 }, { 0x2D7F, 0x2D7F },
    { 0x2DE0, 0x2DFF }, { 0x302A, 0x302F }, { 0x3099, 0x309A },
    { 0xA66F, 0xA672 }, { 0xA674, 0xA67D }, { 0xA69E, 0xA69F },
    { 0xA6F0, 0xA6F1 }, { 0xA802, 0xA802 }, { 0xA806, 0xA806 },
    { 0xA80B, 0xA80B }, { 0xA823, 0xA827 }, { 0xA82C, 0xA82C },
    { 0xA880, 0xA881 }, { 0xA8B4, 0xA8C5 }, { 0xA8E0, 0xA8F1 },
    { 0xA8FF, 0xA8FF }, { 0xA926, 0xA92D }, { 0xA947, 0xA953 },
    { 0xA980, 0xA983 }, { 0xA9B3, 0xA9C0 }, { 0xA9E5, 0xA9E5 },
    { 0xAA29, 0xAA36 }, { 0xAA43, 0xAA43 }, { 0xAA4C, 0xAA4D },
    { 0xAA7B, 0xAA7D }, { 0xAAB0, 0xAAB0 }, { 0xAAB2, 0xAAB4 },
    { 0xAAB7, 0xAAB8 }, { 0xAABE, 0xAABF }, { 0xAAC1, 0xAAC1 },
    { 0xAAEB, 0xAAEF }, { 0xAAF5, 0xAAF6 }, { 0xABE3, 0xABEA },
    { 0xABEC, 0xABED }, { 0xD7B0, 0xD7FF }, { 0xFB1E, 0xFB1E },
    { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },
    { 0xFFF9, 0xFFFB }, { 0x101FD, 0x101FD }, { 0x102E0, 0x102E0 },
    { 0x10376, 0x1037A }, { 0x10A01, 0x10A03 }, { 0x10A05, 0x10A06 },
    { 0x10A0C, 0x10A0F }, { 0x10A38, 0x10A3A }, { 0x10A3F, 0x10A3F },
    { 0x10AE5, 0x10AE6 }, { 0x10D24, 0x10D27 }, { 0x10D69, 0x10D6D },
    { 0x10EAB, 0x10EAC }, { 0x10EFA, 0x10EFF }, { 0x10F46, 0x10F50 },
    { 0x10F82, 0x10F85 }, { 0x11000, 0x11002 }, { 0x11038, 0x11046 },
    { 0x11070, 0x11070 }, { 0x11073, 0x11074 }, { 0x1107F, 0x11082 },
    { 0x110B0, 0x110BA }, { 0x110BD, 0x110BD }, { 0x110C2, 0x110C2 },
    { 0x110CD, 0x110CD }, { 0x11100, 0x11102 }, { 0x11127, 0x11134 },
    { 0x11145, 0x11146 }, { 0x11173, 0x11173 }, { 0x11180, 0x11182 },
    { 0x111B3, 0x111C0 }, { 0x111C9, 0x111CC }, { 0x111CE, 0x111CF },
    { 0x1122C, 0x11237 }, { 0x1123E, 0x1123E }, { 0x11241, 0x11241 },
    { 0x112DF, 0x112EA }, { 0x11300, 0x11303 }, { 0x1133B, 0x1133C },
    { 0x1133E, 0x11344 }, { 0x11347, 0x11348 }, { 0x1134B, 0x1134D },
    { 0x11357, 0x11357 }, { 0x11362, 0x11363 }, { 0x11366, 0x1136C },
    { 0x11370, 0x11374 }, { 0x113B8, 0x113C0 }, { 0x113C2, 0x113C2 },
    { 0x113C5, 0x113C5 }, { 0x113C7, 0x113CA }, { 0x113CC, 0x113D0 },
    { 0x113D2, 0x113D2 }, { 0x113E1, 0x113E2 }, { 0x11435, 0x11446 },
    { 0x1145E, 0x1145E }, { 0x114B0, 0x114C3 }, { 0x115AF, 0x115B5 },
    { 0x115B8, 0x115C0 }, { 0x115DC, 0x115DD }, { 0x11630, 0x11640 },
    { 0x116AB, 0x116B7 }, { 0x1171D, 0x1172B }, { 0x1182C, 0x1183A },
    { 0x11930, 0x11935 }, { 0x11937, 0x11938 }, { 0x1193B, 0x1193E },
    { 0x11940, 0x11940 }, { 0x11942, 0x11943 }, { 0x119D1, 0x119D7 },
    { 0x119DA, 0x119E0 }, { 0x119E4, 0x119E4 }, { 0x11A01, 0x11A0A },
    { 0x11A33, 0x11A39 }, { 0x11A3B, 0x11A3E }, { 0x11A47, 0x11A47 },
    { 0x11A51, 0x11A5B }, { 0x11A8A, 0x11A99 }, { 0x11B60, 0x11B67 },
    { 0x11C2F, 0x11C36 }, { 0x11C38, 0x11C3F }, { 0x11C92, 0x11CA7 },
    { 0x11CA9, 0x11CB6 }, { 0x11D31, 0x11D36 }, { 0x11D3A, 0x11D3A },
    { 0x11D3C, 0x11D3D }, { 0x11D3F, 0x11D45 }, { 0x11D47, 0x11D47 },
    { 0x11D8A, 0x11D8E }, { 0x11D90, 0x11D91 }, { 0x11D93, 0x11D97 },
    { 0x11EF3, 0x11EF6 }, { 0x11F00, 0x11F01 }, { 0x11F03, 0x11F03 },
    { 0x11F34, 0x11F3A }, { 0x11F3E, 0x11F42 }, { 0x11F5A, 0x11F5A },
    { 0x13430, 0x13440 }, { 0x13447, 0x13455 }, { 0x1611E, 0x1612F },
    { 0x16AF0, 0x16AF4 }, { 0x16B30, 0x16B36 }, { 0x16F4F, 0x16F4F },
    { 0x16F51, 0x16F87 }, { 0x16F8F, 0x16F92 }, { 0x16FE4, 0x16FE4 },
    { 0x16FF0, 0x16FF1 }, { 0x1BC9D, 0x1BC9E }, { 0x1BCA0, 0x1BCA3 },
    { 0x1CF00, 0x1CF2D }, { 0x1CF30, 0x1CF46 }, { 0x1D165, 0x1D169 },
    { 0x1D16D, 0x1D182 }, { 0x1D185, 0x1D18B }, { 0x1D1AA, 0x1D1AD },
    { 0x1D242, 0x1D244 }, { 0x1DA00, 0x1DA36 }, { 0x1DA3B, 0x1DA6C },
    { 0x1DA75, 0x1DA75 }, { 0x1DA84, 0x1DA84 }, { 0x1DA9B, 0x1DA9F },
    { 0x1DAA1, 0x1DAAF }, { 0x1E000, 0x1E006 }, { 0x1E008, 0x1E018 },
    { 0x1E01B, 0x1E021 }, { 0x1E023, 0x1E024 }, { 0x1E026, 0x1E02A },
    { 0x1E08F, 0x1E08F }, { 0x1E130, 0x1E136 }, { 0x1E2AE, 0x1E2AE },
    { 0x1E2EC, 0x1E2EF }, { 0x1E4EC, 0x1E4EF }, { 0x1E5EE, 0x1E5EF },
    { 0x1E6E3, 0x1E6E3 }, { 0x1E6E6, 0x1E6E6 }, { 0x1E6EE, 0x1E6EF },
    { 0x1E6F5, 0x1E6F5 }, { 0x1E8D0, 0x1E8D6 }, { 0x1E944, 0x1E94A },
    { 0x1F3FB, 0x1F3FF }, { 0xE0001, 0xE0001 }, { 0xE0020, 0xE007F },
    { 0xE0100, 0xE01EF },
};
static const struct unicode_range double_width[] = {
    { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A },
    { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
    { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2630, 0x2637 },
    { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x268A, 0x268F },
    { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB },
    { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE },
    { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 },
    { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
    { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 },
    { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 },
    { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 },
    { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 },
    { 0x2B55, 0x2B55 }, { 0x2E80, 0x2E99 }, { 0x2E9B, 0x2EF3 },
    { 0x2F00, 0x2FD5 }, { 0x2FF0, 0x303E }, { 0x3041, 0x3096 },
    { 0x3099, 0x30FF }, { 0x3105, 0x312F }, { 0x3131, 0x318E },
    { 0x3190, 0x31E5 }, { 0x31EF, 0x321E }, { 0x3220, 0x3247 },
    { 0x3250, 0xA48C }, { 0xA490, 0xA4C6 }, { 0xA960, 0xA97C },
    { 0xAC00, 0xD7A3 }, { 0xF900, 0xFA6D }, { 0xFA70, 0xFAD9 },
    { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 }, { 0xFE54, 0xFE66 },
    { 0xFE68, 0xFE6B }, { 0xFF01, 0xFF60 }, { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FE4 }, { 0x16FF0, 0x16FF6 }, { 0x17000, 0x18CD5 },
    { 0x18CFF, 0x18D1E }, { 0x18D80, 0x18DF2 }, { 0x1AFF0, 0x1AFF3 },
    { 0x1AFF5, 0x1AFFB }, { 0x1AFFD, 0x1AFFE }, { 0x1B000, 0x1B122 },
    { 0x1B132, 0x1B132 }, { 0x1B150, 0x1B152 }, { 0x1B155, 0x1B155 },
    { 0x1B164, 0x1B167 }, { 0x1B170, 0x1B2FB }, { 0x1D300, 0x1D356 },
    { 0x1D360, 0x1D376 }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
    { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F202 },
    { 0x1F210, 0x1F23B }, { 0x1F240, 0x1F248 }, { 0x1F250, 0x1F251 },
    { 0x1F260, 0x1F265 }, { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 },
    { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA },
    { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 },
    { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC },
    { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 },
    { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 },
    { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D8 }, { 0x1F6DC, 0x1F6DF },
    { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB },
    { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 },
    { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FA7C }, { 0x1FA80, 0x1FA8A },
    { 0x1FA8E, 0x1FAC6 }, { 0x1FAC8, 0x1FAC8 }, { 0x1FACD, 0x1FADC },
    { 0x1FADF, 0x1FAEA }, { 0x1FAEF, 0x1FAF8 }, { 0x20000, 0x2A6DF },
    { 0x2A700, 0x2B81D }, { 0x2B820, 0x2CEAD }, { 0x2CEB0, 0x2EBE0 },
    { 0x2EBF0, 0x2EE5D }, { 0x2F800, 0x2FA1D }, { 0x30000, 0x3134A },
    { 0x31350, 0x33479 },
};

static int in_ranges(uint32_t cp, const struct unicode_range *ranges, size_t count)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) hi = mid;
        else if (cp > ranges[mid].last) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int content_font_char_cells(uint32_t cp)
{
    if (in_ranges(cp, zero_width, sizeof zero_width / sizeof *zero_width)) return 0;
    return in_ranges(cp, double_width, sizeof double_width / sizeof *double_width) ? 2 : 1;
}

/* Never read beyond the first NUL, even for a truncated multibyte sequence. */
static uint32_t next_cp(const unsigned char **text)
{
    const unsigned char *p = *text;
    uint32_t cp = *p;
    *text = p + 1;
    if (cp < 0x80) return cp;
    int bytes = cp >= 0xC2 && cp <= 0xDF ? 2 : cp >= 0xE0 && cp <= 0xEF ? 3 :
                cp >= 0xF0 && cp <= 0xF4 ? 4 : 0;
    if (!bytes) return 0xFFFD;
    cp &= (1u << (7 - bytes)) - 1;
    for (int i = 1; i < bytes; i++) {
        if (!p[i] || (p[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) ||
        (bytes == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0xFFFD;
    *text = p + bytes;
    return cp;
}

int content_font_text_cells(const char *utf8)
{
    if (!utf8) return 0;
    const unsigned char *p = (const unsigned char *)utf8;
    int cells = 0;
    while (*p) {
        int n = content_font_char_cells(next_cp(&p));
        if (cells > INT_MAX - n) return INT_MAX;
        cells += n;
    }
    return cells;
}

#ifdef CONFIG_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

struct content_font {
    FT_Library library;
    FT_Face faces[4];                 /* regular, bold, oblique, bold oblique */
    FT_Face cjk[2];                   /* regular and real bold, no fake oblique */
    int size_px, cell_w, cell_h, baseline;
    uint32_t *pixels;
    size_t capacity;
};

static const char *const regular_paths[] = {
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
};
static const char *const cjk_paths[2][2] = {
    { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc" },
    { "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc" }
};

static FT_Face open_face(FT_Library library, const char *path, FT_Long index, int mono)
{
    FT_Face face = NULL;
    if (FT_New_Face(library, path, index, &face)) return NULL;
    if ((mono && !FT_IS_FIXED_WIDTH(face)) || FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        FT_Done_Face(face);
        return NULL;
    }
    return face;
}

static FT_Face open_cjk(FT_Library library, const char *path)
{
    FT_Face first = open_face(library, path, 0, 0);
    if (!first) return NULL;
    if (first->family_name && strstr(first->family_name, " SC")) return first;
    for (FT_Long i = 1; i < first->num_faces; i++) {
        FT_Face face = open_face(library, path, i, 0);
        if (!face) continue;
        if (face->family_name && strstr(face->family_name, " SC")) {
            FT_Done_Face(first);
            return face;
        }
        FT_Done_Face(face);
    }
    return first;
}

static int pixels_up(FT_Pos value) { return (int)((value + 63) / 64); }

static void measure(struct content_font *font)
{
    int ascent = 0, descent = 0, height = 0, width = 0;
    for (int i = 0; i < 6; i++) {
        FT_Face face = i < 4 ? font->faces[i] : font->cjk[i - 4];
        if (!face) continue;
        FT_Size_Metrics *m = &face->size->metrics;
        int a = pixels_up(m->ascender), d = pixels_up(-m->descender);
        int h = pixels_up(m->height), w = pixels_up(m->max_advance);
        if (a > ascent) ascent = a;
        if (d > descent) descent = d;
        if (h > height) height = h;
        if (i < 4 && w > width) width = w;
    }
    if (height < ascent + descent) height = ascent + descent;
    font->cell_w = width > 0 ? width : 1;
    font->cell_h = height > 0 ? height : 1;
    font->baseline = ascent + (font->cell_h - ascent - descent) / 2;
}

int content_font_set_size_px(struct content_font *font, int size_px)
{
    if (!font) return -1;
    if (size_px < CONTENT_FONT_MIN_PX) size_px = CONTENT_FONT_MIN_PX;
    if (size_px > CONTENT_FONT_MAX_PX) size_px = CONTENT_FONT_MAX_PX;
    if (font->size_px == size_px) return 0;
    for (int i = 0; i < 6; i++) {
        FT_Face face = i < 4 ? font->faces[i] : font->cjk[i - 4];
        if (!face || !FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size_px)) continue;
        /* Every face remains at the old size if the change cannot be made. */
        for (int j = 0; j < 6; j++) {
            FT_Face old = j < 4 ? font->faces[j] : font->cjk[j - 4];
            if (old && font->size_px) FT_Set_Pixel_Sizes(old, 0, (FT_UInt)font->size_px);
        }
        return -1;
    }
    font->size_px = size_px;
    measure(font);
    return 0;
}

struct content_font *content_font_new(const char *path, int size_px)
{
    struct content_font *font = calloc(1, sizeof *font);
    if (!font) return NULL;
    if (FT_Init_FreeType(&font->library)) {
        free(font);
        return NULL;
    }
    if (path && *path) font->faces[0] = open_face(font->library, path, 0, 1);
    else {
        for (size_t i = 0; i < sizeof regular_paths / sizeof *regular_paths; i++) {
            font->faces[0] = open_face(font->library, regular_paths[i], 0, 1);
            if (font->faces[0]) { path = regular_paths[i]; break; }
        }
    }
    if (!font->faces[0]) { content_font_free(font); return NULL; }
    /* Sibling style files are part of the same packaged DejaVu family. Do
     * not silently mix an explicitly selected other family with DejaVu. */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcmp(base, "DejaVuSansMono.ttf")) {
        static const char *const names[] = {
            "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-BoldOblique.ttf"
        };
        size_t prefix = (size_t)(base - path);
        char *sibling = malloc(prefix + sizeof "DejaVuSansMono-BoldOblique.ttf");
        if (sibling) {
            memcpy(sibling, path, prefix);
            for (int i = 0; i < 3; i++) {
                strcpy(sibling + prefix, names[i]);
                font->faces[i + 1] = open_face(font->library, sibling, 0, 1);
            }
            free(sibling);
        }
    }
    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < sizeof cjk_paths[i] / sizeof *cjk_paths[i]; j++) {
            font->cjk[i] = open_cjk(font->library, cjk_paths[i][j]);
            if (font->cjk[i]) break;
        }
    }
    if (content_font_set_size_px(font, size_px)) { content_font_free(font); return NULL; }
    return font;
}

void content_font_free(struct content_font *font)
{
    if (!font) return;
    for (int i = 0; i < 4; i++) if (font->faces[i]) FT_Done_Face(font->faces[i]);
    for (int i = 0; i < 2; i++) if (font->cjk[i]) FT_Done_Face(font->cjk[i]);
    FT_Done_FreeType(font->library);
    free(font->pixels);
    free(font);
}

int content_font_size_px(const struct content_font *font) { return font ? font->size_px : 0; }

void content_font_metrics(const struct content_font *font, int *cell_w, int *cell_h, int *baseline)
{
    if (cell_w) *cell_w = font ? font->cell_w : 0;
    if (cell_h) *cell_h = font ? font->cell_h : 0;
    if (baseline) *baseline = font ? font->baseline : 0;
}

static FT_Face style_face(const struct content_font *font, unsigned style)
{
    unsigned index = style & (CONTENT_FONT_BOLD | CONTENT_FONT_ITALIC);
    if (font->faces[index]) return font->faces[index];
    if ((index & CONTENT_FONT_BOLD) && font->faces[CONTENT_FONT_BOLD])
        return font->faces[CONTENT_FONT_BOLD];
    if ((index & CONTENT_FONT_ITALIC) && font->faces[CONTENT_FONT_ITALIC])
        return font->faces[CONTENT_FONT_ITALIC];
    return font->faces[0];
}

static FT_Face glyph_face(const struct content_font *font, FT_Face selected,
                          uint32_t cp, unsigned style, FT_UInt *glyph)
{
    *glyph = FT_Get_Char_Index(selected, cp);
    if (*glyph) return selected;
    if (selected != font->faces[0]) {
        *glyph = FT_Get_Char_Index(font->faces[0], cp);
        if (*glyph) return font->faces[0];
    }
    int bold = (style & CONTENT_FONT_BOLD) != 0;
    FT_Face cjk = font->cjk[bold] ? font->cjk[bold] : font->cjk[0];
    if (cjk) {
        *glyph = FT_Get_Char_Index(cjk, cp);
        if (*glyph) return cjk;
    }
    return selected;                   /* glyph 0 is its real .notdef */
}

static uint32_t blend(uint32_t fg, uint32_t bg, unsigned alpha)
{
    unsigned inv = 255 - alpha;
    unsigned r = (((fg >> 16) & 255) * alpha + ((bg >> 16) & 255) * inv + 127) / 255;
    unsigned g = (((fg >> 8) & 255) * alpha + ((bg >> 8) & 255) * inv + 127) / 255;
    unsigned b = ((fg & 255) * alpha + (bg & 255) * inv + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

static unsigned coverage(const FT_Bitmap *bitmap, unsigned x, unsigned y)
{
    /* A negative pitch means the first stored row is the bottom row. */
    size_t pitch = bitmap->pitch < 0 ? (size_t)-(int64_t)bitmap->pitch : (size_t)bitmap->pitch;
    const unsigned char *row = bitmap->buffer +
        (bitmap->pitch < 0 ? bitmap->rows - 1 - y : y) * pitch;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) return (row[x / 8] & (0x80u >> (x % 8))) ? 255 : 0;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY && bitmap->num_grays > 1)
        return (unsigned)row[x] * 255 / (bitmap->num_grays - 1);
    return 0;
}

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

void content_font_draw(struct content_font *font, int x, int y, const char *utf8,
                       int clip_x, int clip_y, int clip_w, int clip_h,
                       uint32_t fg, uint32_t bg, unsigned style)
{
    if (!font || !utf8 || clip_w <= 0 || clip_h <= 0) return;
    int cells = content_font_text_cells(utf8), screen_w = 0, screen_h = 0;
    if (!cells) return;
    screen_pixels(&screen_w, &screen_h);
    int64_t left = max64(max64(x, clip_x), 0);
    int64_t top = max64(max64(y, clip_y), 0);
    int64_t right = min64(min64((int64_t)x + (int64_t)cells * font->cell_w,
                               (int64_t)clip_x + clip_w), screen_w);
    int64_t bottom = min64(min64((int64_t)y + font->cell_h, (int64_t)clip_y + clip_h), screen_h);
    if (right <= left || bottom <= top) return;
    int width = (int)(right - left), height = (int)(bottom - top);
    screen_fill_px((int)left, (int)top, width, height, bg);
    if ((size_t)width > SIZE_MAX / sizeof *font->pixels / (size_t)height) return;
    size_t count = (size_t)width * (size_t)height;
    if (font->capacity < count) {
        uint32_t *pixels = realloc(font->pixels, count * sizeof *pixels);
        if (!pixels) return;
        font->pixels = pixels;
        font->capacity = count;
    }
    for (size_t i = 0; i < count; i++) font->pixels[i] = bg & 0xFFFFFF;
    FT_Face selected = style_face(font, style);
    int64_t pen = x, previous = x;
    int previous_cells = 1, have_previous = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        uint32_t cp = next_cp(&p);
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) cp = 0xFFFD;
        int advance = content_font_char_cells(cp);
        int64_t origin = advance ? pen : previous;
        if (advance) {
            previous = pen;
            previous_cells = advance;
            have_previous = 1;
            pen += (int64_t)advance * font->cell_w;
        } else if (!have_previous) continue;
        /* Keep one cell of overhang at either clip edge for real italics. */
        if (origin >= right + font->cell_w) break;
        if (origin + (int64_t)previous_cells * font->cell_w < left - font->cell_w) continue;
        FT_UInt glyph;
        FT_Face face = glyph_face(font, selected, cp, style, &glyph);
        if (!advance && !glyph) continue; /* format/missing marks are not tofu */
        if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) continue;
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap *bitmap = &slot->bitmap;
        int64_t gx = origin + slot->bitmap_left;
        /* FreeType's zero-advance accents have an origin after their base. */
        if (!advance && !slot->advance.x) gx += (int64_t)previous_cells * font->cell_w;
        if (advance == 2) {
            int64_t delta = ((int64_t)2 * font->cell_w - pixels_up(slot->advance.x)) / 2;
            gx += delta;
        }
        int64_t gy = (int64_t)y + font->baseline - slot->bitmap_top;
        int64_t bx0 = max64(left, gx), by0 = max64(top, gy);
        int64_t bx1 = min64(right, gx + bitmap->width), by1 = min64(bottom, gy + bitmap->rows);
        for (int64_t py = by0; py < by1; py++) {
            for (int64_t px = bx0; px < bx1; px++) {
                unsigned alpha = coverage(bitmap, (unsigned)(px - gx), (unsigned)(py - gy));
                size_t at = (size_t)(py - top) * (size_t)width + (size_t)(px - left);
                if (alpha) font->pixels[at] = blend(fg, font->pixels[at], alpha);
            }
        }
    }
    if (style & CONTENT_FONT_UNDERLINE) {
        int offset = pixels_up(-FT_MulFix(selected->underline_position, selected->size->metrics.y_scale));
        int thickness = pixels_up(FT_MulFix(selected->underline_thickness, selected->size->metrics.y_scale));
        if (thickness < 1) thickness = 1;
        int64_t uy = min64((int64_t)y + font->baseline + offset, (int64_t)y + font->cell_h - thickness);
        int64_t uy0 = max64(top, uy), uy1 = min64(bottom, uy + thickness);
        for (int64_t py = uy0; py < uy1; py++)
            for (int px = 0; px < width; px++)
                font->pixels[(size_t)(py - top) * (size_t)width + (size_t)px] = fg & 0xFFFFFF;
    }
    screen_blit((int)left, (int)top, width, height, font->pixels, width);
}

#else

struct content_font { int unavailable; };
struct content_font *content_font_new(const char *path, int size_px)
{ (void)path; (void)size_px; return NULL; }
void content_font_free(struct content_font *font) { (void)font; }
int content_font_set_size_px(struct content_font *font, int size_px)
{ (void)font; (void)size_px; return -1; }
int content_font_size_px(const struct content_font *font) { (void)font; return 0; }
void content_font_metrics(const struct content_font *font, int *cell_w, int *cell_h, int *baseline)
{
    (void)font;
    if (cell_w) *cell_w = 0;
    if (cell_h) *cell_h = 0;
    if (baseline) *baseline = 0;
}
void content_font_draw(struct content_font *font, int x, int y, const char *utf8,
                       int clip_x, int clip_y, int clip_w, int clip_h,
                       uint32_t fg, uint32_t bg, unsigned style)
{
    (void)font; (void)x; (void)y; (void)utf8;
    (void)clip_x; (void)clip_y; (void)clip_w; (void)clip_h;
    (void)fg; (void)bg; (void)style;
}
#endif
