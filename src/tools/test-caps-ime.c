/* Actual keytable-generated US/JIS input through keys.c and real Rime.
 * Only the two keyboards' identity ioctls and event queues are supplied. */
#define main keytable_suite_main
#define keys_release direct_keys_release
#define keys_take direct_keys_take
#include "test-keytable.c"
#undef keys_release
#undef keys_take
#undef main
#include "ime.h"

static char result[128];
static size_t result_len;
static int type_key(int device, int code, int value)
{
    int key = on(device, code, value);
    if (key <= 0) return key;
    int taken = ime_process_key(key);
    const char *commit = ime_get_commit();
    if (commit) {
        size_t n = strlen(commit);
        assert(result_len + n < sizeof result);
        memcpy(result + result_len, commit, n + 1);
        result_len += n;
    }
    if (!taken && key >= 32 && key < 127) {
        assert(result_len + 1 < sizeof result);
        result[result_len++] = (char)key;
        result[result_len] = '\0';
    }
    return key;
}
static void stroke(int device, int code)
{
    type_key(device, code, 1);
    type_key(device, code, 0);
}
static void lock_key(int device)
{
    if (device == 0) type_key(0, KEY_LEFTSHIFT, 1);
    stroke(device, KEY_CAPSLOCK);
    if (device == 0) type_key(0, KEY_LEFTSHIFT, 0);
}
static void clear_result(void) { result[0] = '\0'; result_len = 0; }

int main(int argc, char **argv)
{
    assert(argc == 5);
    keytable = argv[1];
    snprintf(dir, sizeof dir, "%s", argv[2]);
    assert(mkdir(dir, 0700) == 0);
    assert(make("us") == 0);
    snprintf(tables_dir, sizeof tables_dir, "%s/keymaps", dir);
    snprintf(devices_path, sizeof devices_path, "%s/keyboards.conf", dir);
    assert(mkdir(tables_dir, 0700) == 0);
    char jis[600];
    snprintf(jis, sizeof jis, "%s/jp106.table", tables_dir);
    assert(run_keytable("jp106", jis) == 0);
    for (int i = 0; i < 2; i++) {
        assert(pipe(board[i]) == 0);
        fake_fd[i] = board[i][0];
    }
    direct_keys_take(fake_fd, 2);
    assert(keys_set_keyboard("1:1:AT_Translated_Set_2_keyboard", "jp106") == 0);
    keys_reload();
    assert(!strcmp(keys_device_layout(fake_fd[0], "layout"), "jp"));
    assert(!strcmp(keys_device_layout(fake_fd[1], "layout"), "us"));
    assert(ime_init(argv[3], argv[4]) == 0);
    for (int mode = IME_MODE_RAW; mode <= IME_MODE_EN; mode++) {
        ime_set_mode(mode);
        if (mode != IME_MODE_RAW) {
            stroke(0, KEY_N);
            char preedit[128], candidates[128];
            assert(ime_get_overlay(preedit, candidates, sizeof preedit));
        }
        lock_key(0); /* generated JIS Shift+Eisu */
        clear_result();
        stroke(0, KEY_A); stroke(1, KEY_B);
        assert(!strcmp(result, "AB"));
        lock_key(1); /* the US keyboard unlocks the same lock */
        clear_result();
        if (mode == IME_MODE_RAW) {
            stroke(0, KEY_A); stroke(1, KEY_B);
            assert(!strcmp(result, "ab"));
        } else {
            static const int cn[] = { KEY_N, KEY_I, KEY_H, KEY_A, KEY_O, KEY_SPACE };
            static const int en[] = { KEY_H, KEY_E, KEY_L, KEY_L, KEY_O, KEY_SPACE };
            const int *word = mode == IME_MODE_CN ? cn : en;
            for (int i = 0; i < 6; i++) stroke(0, word[i]);
            assert(strstr(result, mode == IME_MODE_CN ? "\xe4\xbd\xa0\xe5\xa5\xbd" : "hello"));
        }
        lock_key(1);
        clear_result();
        stroke(0, KEY_C); stroke(1, KEY_D);
        assert(!strcmp(result, "CD"));
        lock_key(0);
        clear_result();
        stroke(1, KEY_NUMLOCK);
        stroke(0, KEY_KP1); stroke(1, KEY_KP2);
        assert(!strcmp(result, "12"));
        stroke(0, KEY_NUMLOCK);
        assert(type_key(0, KEY_KP1, 1) == K_EOL);
        type_key(0, KEY_KP1, 0);
        assert(type_key(1, KEY_KP2, 1) == K_DOWN);
        type_key(1, KEY_KP2, 0);
        printf("PASS direct real Rime Caps mode %d: generated JIS/US, shared AB/CD and input resumes\n", mode);
        printf("PASS direct real Rime Num Lock mode %d: shared keypad digits and navigation\n", mode);
    }
    ime_done();
    direct_keys_release();
    close(board[0][1]); close(board[1][1]);
    return 0;
}
