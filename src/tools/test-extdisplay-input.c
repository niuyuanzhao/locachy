/*
 * Unit tests for extdisplay touchscreen and pen tablet receiver input:
 *   - welcome.input capability gating ("touch", "tablet") and tolerance of extras
 *   - additive tablet and multi-touch JSON wire formatters and range checks
 *   - sub-pixel coordinate preservation (no int truncation in raw direct path)
 *   - droppable rules (tablet axis and move-only touch frames may shed;
 *     proximity, tip, button, and touch down/up/cancel must never shed)
 *   - pointer_lend_match() acceptance of multi-touch-only panels and pen tablets
 *   - pointer_raw_direct() raw queue, coalescing, shedding, wakeup, and TUI parity
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../base/pointer.c"
#include "extdisplay-protocol.h"
#include <assert.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Minimal base/screen stubs matching test-pointer.c for compiling pointer.c directly. */
int control_fd(void) { return -1; }
int control_send(int sock, const char *msg, size_t len, const int *fds, int n)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)n; return -1; }
int control_send_fields(int sock, const char *const *fields)
{ (void)sock; (void)fields; return -1; }
void screen_pixels(int *w, int *h) { *w = 1920; *h = 1080; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20 - 1; }
void screen_cursor_move(int x, int y) { (void)x; (void)y; }
void screen_cursor_visible(int on) { (void)on; }
double screen_scale(void) { return 1.0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return -1; }

static struct json_object *parse_json(const char *s, int len)
{
    if (len <= 0 || (size_t)len != strlen(s)) return NULL;
    struct json_tokener *tok = json_tokener_new_ex(16);
    if (!tok) return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *o = json_tokener_parse_ex(tok, s, len);
    int ok = o && json_tokener_get_error(tok) == json_tokener_success &&
             json_tokener_get_parse_end(tok) == (size_t)len && json_object_is_type(o, json_type_object);
    json_tokener_free(tok);
    if (!ok) {
        json_object_put(o);
        return NULL;
    }
    return o;
}

static int parse_ctrl(const char *s, struct extdisplay_control *c)
{
    return extdisplay_control_parse((const unsigned char *)s, strlen(s), c);
}

static void set_bit(unsigned long *bits, int n)
{
    bits[n / (8 * sizeof *bits)] |= 1ul << (n % (8 * sizeof *bits));
}

static void test_capabilities(void)
{
    struct extdisplay_control c;
    /* Legacy peer: neither touch nor tablet advertised. */
    CHECK(parse_ctrl("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                     "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_WELCOME && c.input_caps == 0);
    /* Touch only. */
    CHECK(parse_ctrl("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                     "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"touch\"]}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_WELCOME && c.input_caps == EXTDISPLAY_INPUT_TOUCH);
    /* Tablet only. */
    CHECK(parse_ctrl("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                     "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"tablet\"]}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_WELCOME && c.input_caps == EXTDISPLAY_INPUT_TABLET);
    /* Both plus unknown future capabilities are tolerated. */
    CHECK(parse_ctrl("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                     "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"touch\",\"tablet\",\"future\"]}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_WELCOME &&
          c.input_caps == (EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET));
    /* Missing a required capability is still refused even if touch/tablet are present. */
    CHECK(parse_ctrl("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                     "\"input\":[\"key\",\"button\",\"pointer\",\"scroll\",\"touch\",\"tablet\"]}", &c) < 0);

    /* Legacy hello builders keep the 5-entry input list; hello_caps appends opted-in names. */
    char s[1024];
    CHECK(extdisplay_hello(s, sizeof s, 1920, 1080, 1.0, 1, 1) > 0 &&
          strstr(s, "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]") &&
          !strstr(s, "\"touch\"") && !strstr(s, "\"tablet\""));
    CHECK(extdisplay_hello_caps(s, sizeof s, 1920, 1080, 1.0, 1, 1, "yoga", "X1 Yoga",
                                EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET) > 0 &&
          strstr(s, "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"touch\",\"tablet\"]"));
    printf("%s: capability negotiation and hello/welcome parsing\n", failures ? "FAIL" : "PASS");
}

static void test_json_formatters(void)
{
    char s[1024];
    struct json_object *o, *v;

    /* Tablet proximity. */
    int n = extdisplay_tablet_proximity_json(s, sizeof s, 1, EXTDISPLAY_TOOL_PEN, 12345, 1991, 0.512345, 0.345678);
    CHECK(n > 0);
    o = parse_json(s, n);
    CHECK(o != NULL);
    if (o) {
        CHECK(json_object_object_get_ex(o, "type", &v) && strcmp(json_object_get_string(v), "tablet") == 0);
        CHECK(json_object_object_get_ex(o, "event", &v) && strcmp(json_object_get_string(v), "proximity") == 0);
        CHECK(json_object_object_get_ex(o, "in", &v) && json_object_get_boolean(v) == 1);
        CHECK(json_object_object_get_ex(o, "tool", &v) && strcmp(json_object_get_string(v), "pen") == 0);
        CHECK(json_object_object_get_ex(o, "id", &v) && json_object_get_int64(v) == 12345);
        CHECK(json_object_object_get_ex(o, "caps", &v) && json_object_get_int64(v) == 1991);
        CHECK(json_object_object_get_ex(o, "x", &v) && fabs(json_object_get_double(v) - 0.512345) < 1e-6);
        CHECK(json_object_object_get_ex(o, "y", &v) && fabs(json_object_get_double(v) - 0.345678) < 1e-6);
        json_object_put(o);
    }
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_ERASER), "eraser") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_BRUSH), "brush") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_PENCIL), "pencil") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_AIRBRUSH), "airbrush") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_LENS), "lens") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_MOUSE), "mouse") == 0);
    CHECK(strcmp(extdisplay_tablet_tool_name(EXTDISPLAY_TOOL_OTHER), "other") == 0);
    CHECK(extdisplay_tablet_proximity_json(s, sizeof s, 0, EXTDISPLAY_TOOL_ERASER, 0, 0, 0.0, 1.0) > 0 &&
          strstr(s, "\"in\":false") && strstr(s, "\"tool\":\"eraser\""));
    CHECK(extdisplay_tablet_proximity_json(s, sizeof s, 1, EXTDISPLAY_TOOL_PEN, 1, 0, -0.01, 0.5) < 0);
    CHECK(extdisplay_tablet_proximity_json(s, sizeof s, 1, EXTDISPLAY_TOOL_PEN, 1, 0, 0.5, 1.01) < 0);
    CHECK(extdisplay_tablet_proximity_json(s, 16, 1, EXTDISPLAY_TOOL_PEN, 1, 0, 0.5, 0.5) < 0);

    /* Tablet axis (sub-pixel precision, pressure, tilt, distance, rotation, buttons). */
    n = extdisplay_tablet_axis_json(s, sizeof s, 0.512437, 0.345719, 0.4821, 12.5, -8.0, -0.0, 0.0, 1);
    CHECK(n > 0 && !strstr(s, "-0.0"));
    o = parse_json(s, n);
    CHECK(o != NULL);
    if (o) {
        CHECK(json_object_object_get_ex(o, "type", &v) && strcmp(json_object_get_string(v), "tablet") == 0);
        CHECK(json_object_object_get_ex(o, "event", &v) && strcmp(json_object_get_string(v), "axis") == 0);
        CHECK(json_object_object_get_ex(o, "x", &v) && fabs(json_object_get_double(v) - 0.512437) < 1e-6);
        CHECK(json_object_object_get_ex(o, "y", &v) && fabs(json_object_get_double(v) - 0.345719) < 1e-6);
        CHECK(json_object_object_get_ex(o, "pressure", &v) && fabs(json_object_get_double(v) - 0.4821) < 1e-4);
        CHECK(json_object_object_get_ex(o, "tiltX", &v) && fabs(json_object_get_double(v) - 12.5) < 1e-3);
        CHECK(json_object_object_get_ex(o, "tiltY", &v) && fabs(json_object_get_double(v) - (-8.0)) < 1e-3);
        CHECK(json_object_object_get_ex(o, "distance", &v) && fabs(json_object_get_double(v) - 0.0) < 1e-4);
        CHECK(json_object_object_get_ex(o, "rotation", &v) && fabs(json_object_get_double(v) - 0.0) < 1e-3);
        CHECK(json_object_object_get_ex(o, "buttons", &v) && json_object_get_int64(v) == 1);
        json_object_put(o);
    }
    CHECK(extdisplay_tablet_axis_json(s, sizeof s, 0.5, 0.5, 1.1, 0, 0, 0, 0, 0) < 0);
    CHECK(extdisplay_tablet_axis_json(s, sizeof s, 0.5, 0.5, 0.5, 95.0, 0, 0, 0, 0) < 0);
    CHECK(extdisplay_tablet_axis_json(s, sizeof s, NAN, 0.5, 0.5, 0, 0, 0, 0, 0) < 0);

    /* Tablet tip. */
    n = extdisplay_tablet_tip_json(s, sizeof s, 1, 0.5124, 0.34571, 0.12, 1);
    CHECK(n > 0);
    o = parse_json(s, n);
    CHECK(o != NULL);
    if (o) {
        CHECK(json_object_object_get_ex(o, "event", &v) && strcmp(json_object_get_string(v), "tip") == 0);
        CHECK(json_object_object_get_ex(o, "down", &v) && json_object_get_boolean(v) == 1);
        CHECK(json_object_object_get_ex(o, "pressure", &v) && fabs(json_object_get_double(v) - 0.12) < 1e-4);
        CHECK(json_object_object_get_ex(o, "buttons", &v) && json_object_get_int64(v) == 1);
        json_object_put(o);
    }
    CHECK(extdisplay_tablet_tip_json(s, sizeof s, 2, 0.5, 0.5, 0.1, 1) < 0);

    /* Tablet button (BTN_STYLUS = 331). */
    n = extdisplay_tablet_button_json(s, sizeof s, BTN_STYLUS, 1, 0.5124, 0.34571, 2);
    CHECK(n > 0);
    o = parse_json(s, n);
    CHECK(o != NULL);
    if (o) {
        CHECK(json_object_object_get_ex(o, "event", &v) && strcmp(json_object_get_string(v), "button") == 0);
        CHECK(json_object_object_get_ex(o, "button", &v) && json_object_get_int64(v) == 331);
        CHECK(json_object_object_get_ex(o, "state", &v) && json_object_get_int64(v) == 1);
        CHECK(json_object_object_get_ex(o, "buttons", &v) && json_object_get_int64(v) == 2);
        json_object_put(o);
    }
    CHECK(extdisplay_tablet_button_json(s, sizeof s, 10, 1, 0.5, 0.5, 0) < 0); /* not a BTN_* code */
    CHECK(extdisplay_tablet_button_json(s, sizeof s, BTN_STYLUS, 2, 0.5, 0.5, 0) < 0);

    /* Multi-touch frame with two contacts (down + move) and mm resolution. */
    struct extdisplay_touch_frame f = { 0 };
    CHECK(extdisplay_touch_frame_add(&f, 0, EXTDISPLAY_TOUCH_DOWN, 0.5, 0.4, 12.3, 8.1) == 0);
    CHECK(extdisplay_touch_frame_add(&f, 1, EXTDISPLAY_TOUCH_MOVE, 0.52, 0.41, 13.0, 8.4) == 0);
    n = extdisplay_touch_frame_json(s, sizeof s, f.points, f.count);
    CHECK(n > 0);
    o = parse_json(s, n);
    CHECK(o != NULL);
    if (o) {
        struct json_object *pts, *p0, *p1;
        CHECK(json_object_object_get_ex(o, "type", &v) && strcmp(json_object_get_string(v), "touch") == 0);
        CHECK(json_object_object_get_ex(o, "phase", &v) && strcmp(json_object_get_string(v), "frame") == 0);
        CHECK(json_object_object_get_ex(o, "points", &pts) && json_object_array_length(pts) == 2);
        p0 = json_object_array_get_idx(pts, 0);
        p1 = json_object_array_get_idx(pts, 1);
        CHECK(json_object_object_get_ex(p0, "id", &v) && json_object_get_int64(v) == 0);
        CHECK(json_object_object_get_ex(p0, "state", &v) && strcmp(json_object_get_string(v), "down") == 0);
        CHECK(json_object_object_get_ex(p0, "xmm", &v) && fabs(json_object_get_double(v) - 12.3) < 1e-3);
        CHECK(json_object_object_get_ex(p0, "ymm", &v) && fabs(json_object_get_double(v) - 8.1) < 1e-3);
        CHECK(json_object_object_get_ex(p1, "id", &v) && json_object_get_int64(v) == 1);
        CHECK(json_object_object_get_ex(p1, "state", &v) && strcmp(json_object_get_string(v), "move") == 0);
        json_object_put(o);
    }
    /* 10-contact frame still fits comfortably inside EXTDISPLAY_IPC_TEXT (1024). */
    struct extdisplay_touch_frame ten = { 0 };
    for (int i = 0; i < 10; i++) {
        CHECK(extdisplay_touch_frame_add(&ten, i, EXTDISPLAY_TOUCH_MOVE, 0.1 * i, 0.05 * i, 100.5 + i, 200.2 + i) == 0);
    }
    CHECK(extdisplay_touch_frame_add(&ten, 0, EXTDISPLAY_TOUCH_MOVE, 0.5, 0.5, 0, 0) < 0); /* full */
    n = extdisplay_touch_frame_json(s, sizeof s, ten.points, ten.count);
    CHECK(n > 0 && n < 1024);
    CHECK(extdisplay_touch_frame_json(s, sizeof s, NULL, 0) < 0);
    printf("%s: tablet and touch JSON wire formatters\n", failures ? "FAIL" : "PASS");
}

static void test_droppable_rules(void)
{
    char s[1024];
    int n;

    /* Tablet: ONLY axis is droppable; proximity, tip, button are NEVER droppable. */
    n = extdisplay_tablet_axis_json(s, sizeof s, 0.5, 0.5, 0.5, 0, 0, 0, 0, 1);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 1);
    n = extdisplay_tablet_proximity_json(s, sizeof s, 1, EXTDISPLAY_TOOL_PEN, 1, 7, 0.5, 0.5);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);
    n = extdisplay_tablet_tip_json(s, sizeof s, 1, 0.5, 0.5, 0.2, 1);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);
    n = extdisplay_tablet_button_json(s, sizeof s, BTN_STYLUS, 1, 0.5, 0.5, 2);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);

    /* Touch: ONLY move-only frames are droppable; any down/up/cancel makes it non-droppable. */
    struct extdisplay_touch_point moves[2] = {
        { 0, EXTDISPLAY_TOUCH_MOVE, 0.1, 0.2, 10.0, 20.0 },
        { 1, EXTDISPLAY_TOUCH_MOVE, 0.3, 0.4, 30.0, 40.0 },
    };
    CHECK(extdisplay_touch_frame_droppable(moves, 2) == 1);
    n = extdisplay_touch_frame_json(s, sizeof s, moves, 2);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 1);

    struct extdisplay_touch_point mixed_down[2] = {
        { 0, EXTDISPLAY_TOUCH_MOVE, 0.1, 0.2, 10.0, 20.0 },
        { 1, EXTDISPLAY_TOUCH_DOWN, 0.3, 0.4, 30.0, 40.0 },
    };
    CHECK(extdisplay_touch_frame_droppable(mixed_down, 2) == 0);
    n = extdisplay_touch_frame_json(s, sizeof s, mixed_down, 2);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);

    struct extdisplay_touch_point mixed_up[2] = {
        { 0, EXTDISPLAY_TOUCH_MOVE, 0.1, 0.2, 10.0, 20.0 },
        { 1, EXTDISPLAY_TOUCH_UP,   0.3, 0.4, 30.0, 40.0 },
    };
    CHECK(extdisplay_touch_frame_droppable(mixed_up, 2) == 0);
    n = extdisplay_touch_frame_json(s, sizeof s, mixed_up, 2);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);

    struct extdisplay_touch_point cancelled[1] = {
        { 0, EXTDISPLAY_TOUCH_CANCEL, 0.1, 0.2, 10.0, 20.0 },
    };
    CHECK(extdisplay_touch_frame_droppable(cancelled, 1) == 0);
    n = extdisplay_touch_frame_json(s, sizeof s, cancelled, 1);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);

    /* Existing pointer/scroll/button/key/cancel rules agree. */
    n = extdisplay_pointer_json(s, sizeof s, 0.5, 0.5);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 1);
    n = extdisplay_scroll_json(s, sizeof s, 0, 10, EXTDISPLAY_PHASE_UPDATE, EXTDISPLAY_SOURCE_FINGER);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 1);
    n = extdisplay_scroll_json(s, sizeof s, 0, 0, EXTDISPLAY_PHASE_END, EXTDISPLAY_SOURCE_FINGER);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);
    n = extdisplay_button_json(s, sizeof s, BTN_LEFT, 1, 0.5, 0.5);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);
    n = extdisplay_cancel_json(s, sizeof s);
    CHECK(n > 0 && extdisplay_input_droppable(s, (size_t)n) == 0);
    printf("%s: droppable rules for tablet, touch, and legacy input\n", failures ? "FAIL" : "PASS");
}

static void test_pointer_lend_match(void)
{
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1];
    unsigned long rel[REL_MAX / (8 * sizeof(long)) + 1];
    unsigned long abs[ABS_MAX / (8 * sizeof(long)) + 1];

    /* 1. Multi-touch-only panel (ABS_MT_POSITION_X/Y without ABS_X/Y or BTN_TOUCH). */
    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(abs, ABS_MT_POSITION_X);
    set_bit(abs, ABS_MT_POSITION_Y);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    /* 2. Pure pen tablet with BTN_TOOL_PEN + ABS_X/Y (no BTN_TOUCH). */
    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, BTN_TOOL_PEN);
    set_bit(abs, ABS_X);
    set_bit(abs, ABS_Y);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    /* 3. Pure pen tablet with BTN_STYLUS + ABS_X/Y (no BTN_TOUCH). */
    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, BTN_STYLUS);
    set_bit(abs, ABS_X);
    set_bit(abs, ABS_Y);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    /* 4. Existing mouse, single-touch, and keyboard remain accepted. */
    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, BTN_LEFT); set_bit(rel, REL_X); set_bit(rel, REL_Y);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, BTN_TOUCH); set_bit(abs, ABS_X); set_bit(abs, ABS_Y);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, KEY_A); set_bit(keys, KEY_ENTER);
    CHECK(pointer_lend_match(keys, rel, abs) == 1);

    /* 5. Unrelated device (e.g. accelerometer with ABS_X/Y/Z only, or power button) stays rejected. */
    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(abs, ABS_X); set_bit(abs, ABS_Y); set_bit(abs, ABS_Z);
    CHECK(pointer_lend_match(keys, rel, abs) == 0);

    memset(keys, 0, sizeof keys); memset(rel, 0, sizeof rel); memset(abs, 0, sizeof abs);
    set_bit(keys, KEY_POWER);
    CHECK(pointer_lend_match(keys, rel, abs) == 0);
    printf("%s: pointer_lend_match accepts MT-only touchscreens and pure pen tablets without regressions\n",
           failures ? "FAIL" : "PASS");
}

static void test_raw_queue_and_subpixel(void)
{
    struct pointer_raw_event re;

    /* Raw mode off by default: events ignored, TUI path untouched. */
    CHECK(pointer_raw_pending() == 0);
    raw_touch_event(POINTER_RAW_TOUCH_DOWN, 0, 123.75, 456.25, 12.3, 45.6, 0, 0, 1920, 1080);
    CHECK(!pointer_raw_next(&re) && !pointer_next());

    /* Opt in to touch + tablet; returns previous setting (0). */
    CHECK(pointer_raw_direct(POINTER_RAW_TOUCH | POINTER_RAW_TABLET) == 0);

    /* Sub-pixel coordinates are preserved without int truncation. */
    raw_touch_event(POINTER_RAW_TOUCH_DOWN, 0, 960.375, 540.625, 120.4, 67.8, 0, 0, 1920, 1080);
    raw_touch_event(POINTER_RAW_TOUCH_FRAME, -1, 0, 0, 0, 0, 0, 0, 1920, 1080);
    /* Wakeup triggers pointer_next() once with GESTURE_RAW so key_poll_many wakes immediately. */
    CHECK(pointer_next() == 1 && pointer_last()->gesture == GESTURE_RAW);
    CHECK(pointer_next() == 0); /* while (pointer_next()) {} terminates cleanly */
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TOUCH_DOWN && re.slot == 0 &&
          fabs(re.x - 960.375) < 1e-9 && fabs(re.y - 540.625) < 1e-9 &&
          fabs(re.xmm - 120.4) < 1e-9 && fabs(re.ymm - 67.8) < 1e-9);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TOUCH_FRAME);
    CHECK(!pointer_raw_next(&re));

    /* Tablet lifecycle: proximity, coalescing axis, tip, barrel buttons, proximity out. */
    uint32_t caps = POINTER_TABLET_CAP_PRESSURE | POINTER_TABLET_CAP_TILT | POINTER_TABLET_CAP_STYLUS;
    raw_tablet_proximity(POINTER_TOOL_PEN, 12345, 99, caps, 1, 100.25, 200.75, 0, 0, 1920, 1080);
    raw_tablet_axis(POINTER_TOOL_PEN, 12345, 99, caps, 101.125, 201.875, 0.25, 5.0, -3.0, 0.1, 15.0, 0, 0, 1920, 1080);
    raw_tablet_axis(POINTER_TOOL_PEN, 12345, 99, caps, 102.625, 202.375, 0.50, 12.5, -8.0, 0.0, 30.0, 0, 0, 1920, 1080);
    raw_tablet_tip(POINTER_TOOL_PEN, 12345, 99, caps, 1, 102.625, 202.375, 0.12, 12.5, -8.0, 0.0, 30.0, 0, 0, 1920, 1080);
    raw_tablet_button(POINTER_TOOL_PEN, 12345, 99, caps, BTN_STYLUS, 1, 102.625, 202.375, 0, 0, 1920, 1080);
    raw_tablet_button(POINTER_TOOL_PEN, 12345, 99, caps, BTN_STYLUS, 0, 102.625, 202.375, 0, 0, 1920, 1080);
    raw_tablet_tip(POINTER_TOOL_PEN, 12345, 99, caps, 0, 102.625, 202.375, 0.0, 0.0, 0.0, 0.2, 0.0, 0, 0, 1920, 1080);
    raw_tablet_proximity(POINTER_TOOL_PEN, 12345, 99, caps, 0, 102.625, 202.375, 0, 0, 1920, 1080);

    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_PROXIMITY && re.proximity_in == 1 &&
          re.tool == POINTER_TOOL_PEN && re.serial == 12345 && re.tool_id == 99 && re.caps == caps &&
          fabs(re.x - 100.25) < 1e-9 && fabs(re.y - 200.75) < 1e-9);
    /* Consecutive tablet axis samples coalesce to the latest sub-pixel sample. */
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_AXIS &&
          fabs(re.x - 102.625) < 1e-9 && fabs(re.y - 202.375) < 1e-9 &&
          fabs(re.pressure - 0.50) < 1e-9 && fabs(re.tilt_x - 12.5) < 1e-9 &&
          fabs(re.tilt_y - (-8.0)) < 1e-9 && fabs(re.rotation - 30.0) < 1e-9 && re.buttons == 0);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_TIP && re.tip_down == 1 && re.buttons == 1);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_BUTTON && re.button == BTN_STYLUS &&
          re.button_state == 1 && re.buttons == 3);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_BUTTON && re.button == BTN_STYLUS &&
          re.button_state == 0 && re.buttons == 1);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_TIP && re.tip_down == 0 && re.buttons == 0);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_PROXIMITY && re.proximity_in == 0 && re.buttons == 0);
    CHECK(!pointer_raw_next(&re));

    /* Overflow shedding: move-only touch frames are shed to keep down/up transitions intact. */
    raw_touch_event(POINTER_RAW_TOUCH_DOWN, 0, 10.5, 20.5, 1.0, 2.0, 0, 0, 1920, 1080);
    raw_touch_event(POINTER_RAW_TOUCH_FRAME, -1, 0, 0, 0, 0, 0, 0, 1920, 1080);
    for (int i = 0; i < RAW_QUEUE; i++) {
        raw_touch_event(POINTER_RAW_TOUCH_MOVE, 0, 10.5 + i, 20.5, 1.0, 2.0, 0, 0, 1920, 1080);
        raw_touch_event(POINTER_RAW_TOUCH_FRAME, -1, 0, 0, 0, 0, 0, 0, 1920, 1080);
    }
    raw_touch_event(POINTER_RAW_TOUCH_UP, 0, 500.5, 20.5, 50.0, 2.0, 0, 0, 1920, 1080);
    raw_touch_event(POINTER_RAW_TOUCH_FRAME, -1, 0, 0, 0, 0, 0, 0, 1920, 1080);
    int saw_down = 0, saw_up = 0, saw_cancel = 0;
    while (pointer_raw_next(&re)) {
        if (re.kind == POINTER_RAW_TOUCH_DOWN) saw_down++;
        if (re.kind == POINTER_RAW_TOUCH_UP) saw_up++;
        if (re.kind == POINTER_RAW_CANCEL) saw_cancel++;
    }
    CHECK(saw_down == 1 && saw_up == 1 && saw_cancel == 0);

    /* Non-droppable overflow collapses cleanly to POINTER_RAW_CANCEL. */
    for (int i = 0; i < RAW_QUEUE + 10; i++) {
        raw_tablet_tip(POINTER_TOOL_PEN, 1, 1, caps, i & 1, 100.0, 100.0, 0.5, 0, 0, 0, 0, 0, 0, 1920, 1080);
    }
    saw_cancel = 0;
    while (pointer_raw_next(&re)) {
        if (re.kind == POINTER_RAW_CANCEL) saw_cancel++;
    }
    CHECK(saw_cancel >= 1);

    /* Selective gating: TOUCH-only ignores tablet raw calls; TABLET-only ignores touch raw calls. */
    CHECK(pointer_raw_direct(POINTER_RAW_TOUCH) == (POINTER_RAW_TOUCH | POINTER_RAW_TABLET));
    raw_tablet_proximity(POINTER_TOOL_PEN, 1, 1, caps, 1, 10.0, 20.0, 0, 0, 1920, 1080);
    CHECK(!pointer_raw_pending());
    raw_touch_event(POINTER_RAW_TOUCH_DOWN, 0, 10.5, 20.5, 1.0, 2.0, 0, 0, 1920, 1080);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TOUCH_DOWN);

    CHECK(pointer_raw_direct(POINTER_RAW_TABLET) == POINTER_RAW_TOUCH);
    raw_touch_event(POINTER_RAW_TOUCH_DOWN, 0, 10.5, 20.5, 1.0, 2.0, 0, 0, 1920, 1080);
    CHECK(!pointer_raw_pending());
    raw_tablet_proximity(POINTER_TOOL_PEN, 1, 1, caps, 1, 10.0, 20.0, 0, 0, 1920, 1080);
    CHECK(pointer_raw_next(&re) && re.kind == POINTER_RAW_TABLET_PROXIMITY);

    /* Disabling restores TUI behavior and flushes state. */
    CHECK(pointer_raw_direct(0) == POINTER_RAW_TABLET);
    CHECK(!pointer_raw_pending());
    touch_ppmm = 10.0;
    touch_down(0, 200, 300, 20, 30);
    touch_up(0);
    CHECK(pointer_next() && pointer_last()->button == BTN_LEFT && pointer_last()->pressed == 1);
    CHECK(pointer_next() && pointer_last()->button == BTN_LEFT && pointer_last()->pressed == 0);
    CHECK(!pointer_next());
    printf("%s: raw queue, sub-pixel preservation, shedding, and TUI parity\n", failures ? "FAIL" : "PASS");
}

int main(void)
{
    test_capabilities();
    test_json_formatters();
    test_droppable_rules();
    test_pointer_lend_match();
    test_raw_queue_and_subpixel();
    printf("%s: extdisplay touch/tablet input unit suite\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
