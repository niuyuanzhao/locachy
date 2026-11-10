#include "include/input-raw.h"
#include <string.h>
#define RAW_QUEUE 256u
static int enabled;
static struct base_raw_key queue[RAW_QUEUE];
static unsigned head, length;
static struct {
    uint32_t device;
    uint32_t symbols[BASE_RAW_CODES];
    unsigned char down[BASE_RAW_CODES], local[BASE_RAW_CODES];
} held[BASE_RAW_DEVICES];
int base_raw_enable(int on)
{
    int old = enabled;
    enabled = !!on;
    head = length = 0;
    memset(held, 0, sizeof held);
    return old;
}
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
    memset(held, 0, sizeof held);
    head = 0; length = 1;
    queue[0] = (struct base_raw_key){ .state = 3 };
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
    if (state == 1) {
        if (held[i].down[code]) { base_raw_cancel(); return; }
        held[i].down[code] = 1; held[i].symbols[code] = sym; held[i].local[code] = !!local;
    } else {
        /* Do not synthesize repeats/releases for keys pressed before focus. */
        if (!held[i].down[code]) return;
        sym = held[i].symbols[code]; local = held[i].local[code];
        if (!state) held[i].down[code] = 0;
    }
    queue[(head + length++) % RAW_QUEUE] = (struct base_raw_key){
        .device = device, .code = code, .keysym = sym, .modifiers = mods,
        .locks = locks, .state = state, .flags = local ? BASE_RAW_LOCAL : 0 };
}
