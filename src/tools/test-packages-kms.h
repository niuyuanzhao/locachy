/* packages' actual drawing and modal loops on the VKMS lease. Only keyboard
 * events and package metadata are supplied here; screen.c draws and modesets. */
#define rows packages_rows
#define main packages_main
#include "../progs/packages.c"
#undef main
#undef rows

const char *base_language(void) { return ""; }
double uptime(void) { return (double)now_ms() / 1000; }
void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{
    snprintf(home, hsize, "/home/tester");
    snprintf(dirs, dsize, "Documents Pictures");
}
const char *base_tr(const char *s) { return s; }
const char *base_trc(const char *context, const char *s) { (void)context; return s; }
const char *base_trn(const char *one, const char *many, unsigned long n) { return n == 1 ? one : many; }

static int kms_keys[8], kms_key_at;
int key_get(void)
{
    CHECK(kms_key_at < (int)(sizeof kms_keys / sizeof kms_keys[0]));
    int key = kms_keys[kms_key_at++];
    if (key == K_SHOWN) {
        resized_width = 1280;
        resized_height = 720;
        int lease = dup(card);
        CHECK(lease >= 0 && screen_take(lease));
    }
    return key;
}
const struct pointer_event *pointer_last(void) { abort(); }

static void kms_ui_mode(int w, int h)
{
    resized_width = w;
    resized_height = h;
    int lease = dup(card);
    CHECK(lease >= 0 && screen_take(lease));
}
static void packages_kms(void)
{
    uint32_t id;
    int lease = screen_lend_outputs((const char *[]){ outputs[1].name }, 1, &id);
    CHECK(lease >= 0 && id);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(card);
        card = -1;
        clear_outputs(-1);
        CHECK(screen_take(lease) && noutputs == 1);
        snprintf(resized_output, sizeof resized_output, "%s", outputs[0].name);
        nleaves = 2;
        installed = 1234;
        snprintf(leaves[0], sizeof leaves[0], "libraw");
        snprintf(leaves[1], sizeof leaves[1], "chmlib");
        page = HOST;
        draw();
        CHECK(ui_hit_at(2, screen_rows() - 2) == HIT_KEY + '/');
        int lit = 0;
        for (uint32_t y = top + 4 * glyph_h; y < top + 5 * glyph_h; y++) {
            for (uint32_t x = 2 * glyph_w; x < 8 * glyph_w; x++) lit += shadow[(size_t)y * width + x] == BG;
        }
        CHECK(lit > 0); /* libraw's actual font pixels on the selected row */
        verify_outputs(1);

        kms_keys[0] = 'a'; kms_keys[1] = K_SHOWN; kms_keys[2] = 'b'; kms_keys[3] = '\n';
        kms_key_at = 0;
        char draft[32] = "";
        CHECK(prompt("Packages:", draft, sizeof draft) && strcmp(draft, "ab") == 0);
        CHECK(width == 1280 && height == 720 && screen_cols() == 160 && screen_rows() == 44);
        CHECK(ui_hit_at(2, 42) == HIT_KEY + '/');
        verify_outputs(1);

        kms_ui_mode(1024, 768);
        draw();
        kms_keys[0] = K_SHOWN; kms_keys[1] = 'y'; kms_key_at = 0;
        CHECK(yes("Apply?"));
        CHECK(ui_hit_at(18, 43) == HIT_KEY + 'y');
        verify_outputs(1);

        kms_ui_mode(1024, 768);
        struct form f = { .is_new = 1 };
        struct form_line lines[FORM_LINES];
        int n = form_lines(&f, lines);
        for (int i = 0; i < n; i++) if (lines[i].field == F_ARGUMENTS) f.sel = i;
        form_show(&f);
        kms_keys[0] = 'x'; kms_keys[1] = K_SHOWN; kms_keys[2] = 'y'; kms_keys[3] = '\n';
        kms_key_at = 0;
        draft[0] = '\0';
        CHECK(form_edit(&f, draft, sizeof draft) && strcmp(draft, "xy") == 0);
        CHECK(width == 1280 && height == 720);
        verify_outputs(1);
        resized_scale = 1.5;
        int scaled = dup(card);
        CHECK(scaled >= 0 && screen_take(scaled));
        draw();
        say("packages scale: %.2f, font %ux%u, bar %u, cells %dx%d", scale, glyph_w, glyph_h, bar_h, cols, rows);
        CHECK(width == 1280 && height == 720 && glyph_h == 28 && bar_h == 42);
        CHECK(screen_cols() == 91 && screen_rows() == 25);
        CHECK(ui_hit_at(2, 23) == HIT_KEY + '/');
        verify_outputs(1);
        screen_release();
        _exit(0);
    }
    close(lease);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(screen_take_back(id) == 0);
    say("PASS packages on real KMS: resized page, input draft, confirmation hit map, container field and 1.5 scale");
}
