#include "include/base.h"
#include <string.h>
#define RAW_QUEUE 256u
#define RAW_MOD_MASK (KEYS_SHIFT | KEYS_CTRL | KEYS_ALT | KEYS_META | KEYS_ALTGR)
static int enabled;
static struct base_raw_key queue[RAW_QUEUE];
static unsigned head, length;
static unsigned mod_count[5]; /* the momentary KEYS_* bits, in bit order */
static struct {
    uint32_t device;
    uint32_t symbols[BASE_RAW_CODES];
    unsigned char down[BASE_RAW_CODES], local[BASE_RAW_CODES];
} held[BASE_RAW_DEVICES];

static void clear_held(void)
{
    memset(held, 0, sizeof held);
    memset(mod_count, 0, sizeof mod_count);
}

int base_raw_enable(int on)
{
    int old = enabled;
    enabled = on > 1 ? 2 : !!on;
    head = length = 0;
    clear_held();
    return old;
}

int base_raw_forward_home(void) { return enabled == 2; }
int base_raw_pending(void) { return enabled && length != 0; }

int base_raw_next(struct base_raw_key *out)
{
    if (!enabled || !length || !out) return 0;
    *out = queue[head]; head = (head + 1) % RAW_QUEUE; length--;
    return 1;
}

void base_raw_cancel(void)
{
    if (!enabled) return;
    clear_held();
    head = 0; length = 1;
    queue[0] = (struct base_raw_key){ .state = 3 };
}

static unsigned char sym_modifier(uint32_t sym)
{
    switch (sym) {
    case 0xffe1: case 0xffe2: return KEYS_SHIFT;
    case 0xffe3: case 0xffe4: return KEYS_CTRL;
    case 0xffe7: case 0xffe8: return KEYS_META;
    case 0xffe9: case 0xffea: return KEYS_ALT;
    case 0xffeb: case 0xffec: case 0xffed: case 0xffee: return KEYS_META;
    case 0xfe03: case 0xfe11: case 0xff7e: return KEYS_ALTGR;
    default: return 0;
    }
}

void base_raw_emit(uint32_t device, uint32_t code, uint32_t sym, uint32_t mods,
                   uint32_t locks, uint32_t state, int local)
{
    if (!enabled || !device || code >= BASE_RAW_CODES || state > 2) return;
    if (length == RAW_QUEUE) { base_raw_cancel(); return; }
    unsigned i;
    for (i = 0; i < BASE_RAW_DEVICES; i++) if (held[i].device == device) break;
    if (i == BASE_RAW_DEVICES) {
        for (i = 0; i < BASE_RAW_DEVICES; i++) if (!held[i].device) break;
        if (i == BASE_RAW_DEVICES) { base_raw_cancel(); return; }
        held[i].device = device;
    }
    /* Derive non-lock modifier bits strictly from keys currently held in the
     * raw session, so a prior base_raw_cancel() never leaks stale modifiers. */
    uint32_t active_mods = mods & ~RAW_MOD_MASK;
    for (unsigned bit = 0; bit < 5; bit++) {
        if (mod_count[bit]) active_mods |= 1u << bit;
    }
    if (state == 1) {
        if (held[i].down[code]) { base_raw_cancel(); return; }
        held[i].down[code] = 1; held[i].symbols[code] = sym; held[i].local[code] = !!local;
        unsigned char modifier = sym_modifier(sym);
        for (unsigned bit = 0; bit < 5; bit++) {
            if (modifier & (1u << bit)) mod_count[bit]++;
        }
    } else {
        /* Do not synthesize repeats/releases for keys pressed before focus. */
        if (!held[i].down[code]) return;
        sym = held[i].symbols[code]; local = held[i].local[code];
        if (!state) {
            unsigned char modifier = sym_modifier(sym);
            held[i].down[code] = 0;
            for (unsigned bit = 0; bit < 5; bit++) {
                if (modifier & (1u << bit) && mod_count[bit]) mod_count[bit]--;
            }
        }
    }
    queue[(head + length++) % RAW_QUEUE] = (struct base_raw_key){
        .device = device, .code = code, .keysym = sym, .modifiers = active_mods,
        .locks = locks, .state = state, .flags = local ? BASE_RAW_LOCAL : 0 };
}
