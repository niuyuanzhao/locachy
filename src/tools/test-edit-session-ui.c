/* Actual editor/session adapter integration over the scripted screen fixture.
 * No display, daemon or real account state: getpwuid isolates root's HOME. */
#define EDIT_SESSION_UI_TEST
#include "base.h"
#include "i18n-stub.h"
#include "test-edit-ui.c"
#include <pwd.h>

static char session_test_home[256];
struct passwd *__real_getpwuid(uid_t uid);
struct passwd *__wrap_getpwuid(uid_t uid)
{
    struct passwd *pw = __real_getpwuid(uid);
    if (!pw || uid != EDIT_USER || !*session_test_home) return pw;
    static struct passwd copy;
    copy = *pw;
    copy.pw_dir = session_test_home;
    return &copy;
}

static void answer(const char *s)
{
    for (; *s; s++) push((unsigned char)*s, 0, NULL);
}
static void choose(int index)
{
    for (int i = 0; i < index; i++) push(K_DOWN, 0, NULL);
    push('\n', 0, NULL);
}
static void fresh_session_ui(void)
{
    reset();
    snprintf(session_name, sizeof session_name, "default");
    session_enabled = session_blocked = session_last_error = 0;
    session_digest[0] = 0;
    session_next = 0;
    quitting = 0;
}
static void check_doc_bytes(struct doc *d, const char *bytes, size_t length)
{
    CHECK(doc_len(d) == length);
    char *actual = buffer_dup(&d->buf, 0, doc_len(d));
    CHECK(actual && !memcmp(actual, bytes, length));
    free(actual);
}
static void read_saved(const char *name, struct edit_session_state *state, int recovered)
{
    CHECK(edit_session_load(name, state) == 0 && state->recovered == recovered);
}

static void test_live_restore(void)
{
    fresh_session_ui();
    session_startup();
    CHECK(session_enabled && !session_blocked && ndocs == 0 && nspaces == 1 && spaces[0].cur == -1);
    struct edit_session_state state = {0};
    read_saved("default", &state, 1);
    CHECK(state.ndocuments == 0 && state.spaces[0].ntabs == 0);
    edit_session_free(&state);
    struct tab *t = open_text("session-file.txt", "disk\nold\n");
    CHECK(t);
    static const char data[] = "\xe4\xb8\xad\xe6\x96\x87\0\xf0\x9f\x8c\x8d\nsecond\nlast\n";
    CHECK(doc_set_text(t->doc, data, sizeof(data) - 1) == 0);
    t->doc->saved = -1;
    t->doc->format = (struct file_format){ENC_UTF16BE, 1, EOL_CRLF};
    t->cursor = 11;
    t->anchor = 6;
    t->top = 12;
    t->left = 3;
    t->sub = 1;
    t->numbers = 0;
    t->wrap = 1;
    t->font_px = 28;
    struct doc *shared = t->doc;
    split_direction(1);
    CHECK(nspaces == 2);
    t = cur_tab();
    t->cursor = 6;
    t->anchor = 0;
    t->font_px = 36;
    new_document();
    struct doc *untitled = cur_tab()->doc;
    static const char untitled_data[] = "unsaved\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\n";
    CHECK(doc_set_text(untitled, untitled_data, sizeof(untitled_data) - 1) == 0);
    untitled->saved = -1;
    untitled->format = (struct file_format){ENC_GB18030, 0, EOL_CR};
    cur_tab()->cursor = 8;
    cur_tab()->anchor = 2;
    cur_tab()->font_px = 18;
    sidebar = 1;
    sidebar_width = 29;
    view_layout.root->ratio = 4200;
    CHECK(session_checkpoint(0, 1) == 0);
    CHECK(shared == spaces[1].tabs[0].doc);
    fresh_session_ui();
    answer("r");
    session_startup();
    CHECK(!session_blocked && ndocs == 2 && nspaces == 2 && active == 1 && sidebar && sidebar_width == 29);
    CHECK(view_layout.root->ratio == 4200 && spaces[1].cur == 1);
    CHECK(spaces[0].tabs[0].doc == spaces[1].tabs[0].doc);
    t = &spaces[0].tabs[0];
    CHECK(t->cursor == 11 && t->anchor == 6 && t->top == 12 && t->left == 3 && t->sub == 1);
    CHECK(t->wrap && !t->numbers && t->font_px == 28 && spaces[1].tabs[0].font_px == 36);
    check_doc_bytes(t->doc, data, sizeof(data) - 1);
    CHECK(doc_modified(t->doc) && t->doc->format.encoding == ENC_UTF16BE && t->doc->format.bom && t->doc->format.eol == EOL_CRLF);
    t = cur_tab();
    check_doc_bytes(t->doc, untitled_data, sizeof(untitled_data) - 1);
    CHECK(!t->doc->path[0] && doc_modified(t->doc) && t->font_px == 18);
    CHECK(t->doc->format.encoding == ENC_GB18030 && t->doc->format.eol == EOL_CR);
    CHECK(session_quit() == 1);
    fresh_session_ui();
    session_startup();
    CHECK(ndocs == 2 && qhead == qtail && !session_blocked);
    check_doc_bytes(cur_tab()->doc, untitled_data, sizeof(untitled_data) - 1);
    puts("PASS actual session startup/restore/clean-quit: shared documents, NUL/Unicode, typed views/font/layout and unsaved buffers");
}

static int has_note(struct doc *d, const char *text)
{
    struct docui *u = ui_of(d);
    for (int i = 0; i < u->nnotes; i++) if (strstr(u->notes[i].text, text)) return 1;
    return 0;
}

/* Recovered edits of a file changed on disk since the checkpoint: the restore
 * warns, Save asks Save Nevertheless, and Cancel leaves the disk bytes. */
static void test_external_change(void)
{
    fresh_session_ui();
    snprintf(session_name, sizeof session_name, "external");
    session_enabled = 1;
    struct tab *t = open_text("conflict.txt", "base\n");
    type_text(active, t, "mine ", 5);
    CHECK(doc_modified(t->doc));
    CHECK(session_checkpoint(0, 1) == 0);
    /* Unchanged disk: restored modified, no conflict. */
    fresh_session_ui();
    snprintf(session_name, sizeof session_name, "external");
    struct edit_session_state state = {0};
    CHECK(edit_session_load("external", &state) == 0 && state.documents[0].base[0]);
    CHECK(session_apply(&state) == 0);
    edit_session_free(&state);
    CHECK(ndocs == 1 && doc_modified(docs[0]) && docs[0]->disk == DISK_NONE && text_is(docs[0], "mine base\n"));
    CHECK(has_note(docs[0], "were restored from session"));
    /* Changed disk: warning and guarded Save. */
    put_file(at_dir("conflict.txt"), "theirs\n");
    fresh_session_ui();
    snprintf(session_name, sizeof session_name, "external");
    CHECK(edit_session_load("external", &state) == 0 && session_apply(&state) == 0);
    edit_session_free(&state);
    struct doc *d = docs[0];
    CHECK(d->disk == DISK_MODIFIED && doc_modified(d) && text_is(d, "mine base\n"));
    CHECK(has_note(d, "changed on disk after these unsaved changes were recorded"));
    push(27, 0, NULL); /* Cancel at Possible Data Loss */
    CHECK(save(d) == -1);
    CHECK(!strcmp(file_bytes(at_dir("conflict.txt")), "theirs\n"));
    push('\n', 0, NULL); /* Save Nevertheless: an explicit choice */
    CHECK(save(d) == 0);
    CHECK(!strcmp(file_bytes(at_dir("conflict.txt")), "mine base\n"));
    /* Deleted disk file: flagged deleted, still modified, nothing written. */
    type_text(active, cur_tab(), "x", 1);
    CHECK(session_checkpoint(0, 1) == 0);
    CHECK(unlink(at_dir("conflict.txt")) == 0);
    fresh_session_ui();
    snprintf(session_name, sizeof session_name, "external");
    CHECK(edit_session_load("external", &state) == 0 && session_apply(&state) == 0);
    edit_session_free(&state);
    CHECK(docs[0]->disk == DISK_DELETED && doc_modified(docs[0]));
    struct stat st;
    CHECK(stat(at_dir("conflict.txt"), &st) != 0);
    CHECK(edit_session_remove("external") == 0);
    puts("PASS actual restore of edits to externally changed/deleted files: warning, Save asks, Cancel keeps disk bytes");
}

static void test_live_actions(void)
{
    /* The actual menu routes to Save Session As, then New Session. */
    choose(3);
    answer("alpha\n");
    session_menu();
    CHECK(!strcmp(session_name, "alpha") && ndocs == 2);
    choose(0);
    answer("beta\n");
    session_menu();
    CHECK(!strcmp(session_name, "beta") && ndocs == 0 && nspaces == 1 && spaces[0].cur == -1);
    new_document();
    type_text(active, cur_tab(), "beta work", 9);
    choose(1);
    choose(1); /* title, alpha, beta, default */
    session_menu();
    CHECK(!strcmp(session_name, "alpha") && ndocs == 2 && nspaces == 2);
    choose(1);
    choose(2);
    session_menu();
    CHECK(!strcmp(session_name, "beta") && ndocs == 1 && text_is(cur_tab()->doc, "beta work"));
    struct edit_session_state state = {0};
    read_saved("alpha", &state, 0);
    CHECK(state.ndocuments == 2 && state.documents[0].modified);
    edit_session_free(&state);
    /* The real editor's Sessions menu (Kate's: after Tools) exposes this menu. */
    struct editor_item file_actions[EDITOR_ITEMS];
    CHECK(strcmp(editor_menu_title(MENU_SESSIONS), "Sessions") == 0 && MENU_SESSIONS == EDITOR_MENUS - 1);
    int actions = editor_items(MENU_SESSIONS, file_actions), session_action = 0;
    for (int i = 0; i < actions; i++) if (file_actions[i].action == EA_SESSION_MANAGE) session_action = 1;
    CHECK(session_action);
    choose(2);
    editor_action(EA_SESSION_MANAGE);
    /* Explicit Save action and periodic view-only checkpoint. */
    choose(2);
    session_menu();
    cur_tab()->font_px = 44;
    cur_tab()->numbers = 0;
    clock_now += 3;
    session_tick();
    read_saved("beta", &state, 1);
    CHECK(state.spaces[0].tabs[0].font_px == 44 && !state.spaces[0].tabs[0].numbers);
    edit_session_free(&state);
    CHECK(session_timeout(-1) >= 0 && session_timeout(1) <= 1);
    choose(4);
    choose(1); /* delete alpha */
    answer("d");
    session_menu();
    CHECK(edit_session_load("alpha", &state) == ENOENT);
    key('q', KEYS_CTRL);
    CHECK(quitting);
    read_saved("beta", &state, 0);
    edit_session_free(&state);
    quitting = 0;
    puts("PASS actual File/Sessions actions and Ctrl+Q, create/save-as/switch/save/delete and periodic view/font checkpoint");
}

static void test_recovery_protection(void)
{
    snprintf(session_name, sizeof session_name, "protected");
    CHECK(session_checkpoint(0, 1) == 0 && edit_session_select("protected") == 0);
    fresh_session_ui();
    snprintf(session_name, sizeof session_name, "protected");
    push(27, 0, NULL);
    session_startup();
    CHECK(session_blocked && ndocs == 0);
    new_document();
    type_text(active, cur_tab(), "new work", 8);
    clock_now += 3;
    session_tick();
    struct edit_session_state state = {0};
    read_saved("protected", &state, 1);
    CHECK(state.ndocuments == 1 && state.documents[0].length == 9 && !memcmp(state.documents[0].text, "beta work", 9));
    edit_session_free(&state);
    answer("safe\n");
    session_save_as();
    CHECK(!session_blocked && !strcmp(session_name, "safe") && text_is(cur_tab()->doc, "new work"));
    CHECK(session_quit() == 1);
    fresh_session_ui();
    session_startup();
    CHECK(!strcmp(session_name, "safe") && text_is(cur_tab()->doc, "new work"));
    /* A malformed named target must not replace the running documents. */
    char malformed[PATH_MAX];
    snprintf(malformed, sizeof malformed, "%s/.local/state/locachy/edit/sessions/tampered.json", session_test_home);
    put_file(malformed, "{\"version\":99}");
    choose(1);
    choose(5); /* title, beta, default, protected, safe, tampered */
    answer("o");
    session_menu();
    CHECK(!strcmp(session_name, "safe") && text_is(cur_tab()->doc, "new work"));
    puts("PASS actual recovery decline protects old unsaved data; Save As escapes safely; malformed switch preserves running editor");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && geteuid() == 0);
    setlocale(LC_CTYPE, "C.UTF-8");
    is_root = 1;
    CHECK(mkdir(argv[1], 0755) == 0 && chown(argv[1], EDIT_USER, EDIT_USER) == 0);
    char *real = realpath(argv[1], NULL);
    CHECK(real && strlen(real) < sizeof test_dir);
    snprintf(test_dir, sizeof test_dir, "%s", real);
    free(real);
    snprintf(session_test_home, sizeof session_test_home, "%s", test_dir);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    test_live_restore();
    test_live_actions();
    test_recovery_protection();
    test_external_change();
    reset();
    close(ino);
    free(docs);
    docs = NULL;
    projects_free();
    puts("PASS all actual editor/session integration tests");
    return 0;
}
