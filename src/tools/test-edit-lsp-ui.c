/* Actual editor callbacks with the established framebuffer/key harness. */
#include "base.h"
#include "i18n-stub.h"
#include "test-edit-lsp-ui-harness.inc"

static void lsp_ui_wait(struct language_document *binding)
{
    int64_t end = g_get_monotonic_time() + 10000000;
    while (g_get_monotonic_time() < end) {
        language_tick();
        if (binding && ed_lsp_state(binding->server->client) == ED_LSP_READY && !language_requests->len) return;
        poll(NULL, 0, 2);
    }
    CHECK(0 && "Language request timed out");
}
/* A workspace edit's documentChanges built as the client decodes them, then
 * applied through the editor's real language_apply. */
static GPtrArray *ui_changes;
static void ui_text_free(gpointer p) { EdLspTextEdit *e = p; g_free(e->new_text); g_free(e); }
static void ui_change_free(gpointer p) { EdLspDocumentEdit *c = p; g_free(c->uri); g_free(c->old_uri); g_ptr_array_unref(c->edits); g_free(c); }
static EdLspDocumentEdit *ui_change(EdLspEditKind kind, const char *rel, const char *old_rel)
{
    if (!ui_changes) ui_changes = g_ptr_array_new_with_free_func(ui_change_free);
    EdLspDocumentEdit *c = g_new0(EdLspDocumentEdit, 1); c->kind = kind; c->version = -1;
    c->uri = g_filename_to_uri(at_dir(rel), NULL, NULL); c->old_uri = old_rel ? g_filename_to_uri(at_dir(old_rel), NULL, NULL) : NULL;
    c->edits = g_ptr_array_new_with_free_func(ui_text_free); g_ptr_array_add(ui_changes, c); return c;
}
static void ui_edit(EdLspDocumentEdit *c, EdLspRange range, const char *text)
{
    EdLspTextEdit *e = g_new0(EdLspTextEdit, 1); e->range = range; e->new_text = g_strdup(text); g_ptr_array_add(c->edits, e);
}
static int ui_apply(struct language_server *s, struct doc *origin)
{
    GPtrArray *annotations = g_ptr_array_new(); EdLspWorkspaceEdit w = {.documents = ui_changes, .annotations = annotations};
    int ok = language_apply(s, &w, ed_lsp_revision(s->client), origin);
    g_ptr_array_unref(annotations); g_clear_pointer(&ui_changes, g_ptr_array_unref); return ok;
}
static int ui_exists(const char *rel) { struct stat st; return lstat(at_dir(rel), &st) == 0; }
static struct doc *ui_doc(const char *rel) { for (int i = 0; i < ndocs; i++) if (!strcmp(docs[i]->path, at_dir(rel))) return docs[i]; return NULL; }
/* Nothing moved aside is left behind in the folder. */
static int ui_leftovers(void)
{
    DIR *dir = opendir(test_dir); struct dirent *e; int n = 0; CHECK(dir);
    while ((e = readdir(dir))) n += strstr(e->d_name, ".edit-lsp-") != NULL;
    closedir(dir); return n;
}
static void lsp_ui_resources(struct language_server *s, struct doc *d)
{
    static const EdLspRange start = {{0, 0}, {0, 0}};
    /* CreateFile, then a text edit into it: the new file is the user's, and
     * opened and edited as Kate opens a document it edits (unsaved). */
    puts("LSP UI: create");
    ui_change(ED_LSP_EDIT_CREATE, "created.c", NULL); ui_edit(ui_change(ED_LSP_EDIT_TEXT, "created.c", NULL), start, "int created;\n");
    CHECK(ui_apply(s, d)); CHECK(ui_exists("created.c")); CHECK(file_bytes(at_dir("created.c"))[0] == '\0');
    struct stat st; CHECK(stat(at_dir("created.c"), &st) == 0); if (is_root) CHECK(st.st_uid == EDIT_USER);
    struct doc *created = ui_doc("created.c"); CHECK(created && text_is(created, "int created;\n") && doc_modified(created));
    language_close_now(created);
    put_file(at_dir("exists.txt"), "kept\n");
    ui_change(ED_LSP_EDIT_CREATE, "exists.txt", NULL); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "already exists"));
    ui_change(ED_LSP_EDIT_CREATE, "exists.txt", NULL)->ignore_if_exists = TRUE; CHECK(ui_apply(s, d)); CHECK(!strcmp(file_bytes(at_dir("exists.txt")), "kept\n"));
    put_file(at_dir("replaced.txt"), "old\n");
    EdLspDocumentEdit *over = ui_change(ED_LSP_EDIT_CREATE, "replaced.txt", NULL); over->overwrite = over->ignore_if_exists = TRUE; /* overwrite wins */
    CHECK(ui_apply(s, d)); CHECK(file_bytes(at_dir("replaced.txt"))[0] == '\0'); CHECK(ui_leftovers() == 0);
    if (is_root) {
        /* The editor runs as root; operations are the user's: a root folder refuses them. */
        CHECK(mkdir(at_dir("root-only"), 0755) == 0); FILE *f = fopen(at_dir("root-only/file.txt"), "w"); CHECK(f && fclose(f) == 0);
        ui_change(ED_LSP_EDIT_CREATE, "root-only/new.txt", NULL); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "Permission denied") && !ui_exists("root-only/new.txt"));
        ui_change(ED_LSP_EDIT_DELETE, "root-only/file.txt", NULL); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "Permission denied") && ui_exists("root-only/file.txt"));
        ui_change(ED_LSP_EDIT_RENAME, "taken.txt", "root-only/file.txt"); CHECK(!ui_apply(s, d)); CHECK(ui_exists("root-only/file.txt") && !ui_exists("taken.txt"));
    }

    /* RenameFile of an open, modified document: it follows (path, name, watch,
     * session state, language binding); its text and undo stay. */
    puts("LSP UI: rename open document");
    put_file(at_dir("moved-from.cpp"), "int moved;\n");
    struct doc *moved = open_file(at_dir("moved-from.cpp")); CHECK(moved && language_attach(moved, 1));
    CHECK(doc_insert(moved, 0, "// edited\n", 10) == 0); show_doc(active, d);
    ui_change(ED_LSP_EDIT_RENAME, "moved-to.cpp", "moved-from.cpp"); CHECK(ui_apply(s, d));
    CHECK(!ui_exists("moved-from.cpp") && !strcmp(file_bytes(at_dir("moved-to.cpp")), "int moved;\n"));
    CHECK(ui_doc("moved-to.cpp") == moved && !ui_doc("moved-from.cpp") && moved->watch >= 0);
    CHECK(text_is(moved, "// edited\nint moved;\n") && doc_modified(moved));
    char name[256]; doc_name(moved, name, sizeof name); CHECK(!strcmp(name, "moved-to.cpp"));
    struct edit_session_state state; CHECK(session_capture(&state) == 0); int in_session = 0;
    for (size_t i = 0; i < state.ndocuments; i++) in_session |= state.documents[i].path && !strcmp(state.documents[i].path, at_dir("moved-to.cpp"));
    edit_session_free(&state); CHECK(in_session);
    language_tick(); struct language_document *follow = language_binding(moved); char *moved_uri = g_filename_to_uri(at_dir("moved-to.cpp"), NULL, NULL);
    CHECK(follow && !strcmp(follow->uri, moved_uri)); g_free(moved_uri); lsp_ui_wait(follow);
    size_t c, a; CHECK(doc_undo(moved, &c, &a)); CHECK(text_is(moved, "int moved;\n")); language_changed(moved);

    /* RenameFile onto an existing file without overwrite is refused; with
     * ignoreIfExists it does nothing; onto a modified open document it is
     * refused even with overwrite. */
    puts("LSP UI: rename onto existing");
    put_file(at_dir("b.txt"), "bee\n");
    ui_change(ED_LSP_EDIT_RENAME, "exists.txt", "b.txt"); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "already exists"));
    CHECK(!strcmp(file_bytes(at_dir("b.txt")), "bee\n") && !strcmp(file_bytes(at_dir("exists.txt")), "kept\n"));
    ui_change(ED_LSP_EDIT_RENAME, "exists.txt", "b.txt")->ignore_if_exists = TRUE; CHECK(ui_apply(s, d)); CHECK(ui_exists("b.txt"));
    struct doc *target = open_file(at_dir("exists.txt")); CHECK(target && doc_insert(target, 0, "x", 1) == 0); show_doc(active, d);
    ui_change(ED_LSP_EDIT_RENAME, "exists.txt", "b.txt")->overwrite = TRUE; CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "unsaved changes"));
    CHECK(!strcmp(file_bytes(at_dir("b.txt")), "bee\n") && !strcmp(file_bytes(at_dir("exists.txt")), "kept\n") && text_is(target, "xkept\n"));
    CHECK(doc_undo(target, &c, &a)); language_close_now(target); CHECK(ui_leftovers() == 0);

    /* DeleteFile of an open modified document asks as closing does: Cancel
     * aborts the whole edit (the earlier CreateFile is undone), Discard applies. */
    puts("LSP UI: delete open modified document");
    put_file(at_dir("doomed.txt"), "doomed\n");
    struct doc *doomed = open_file(at_dir("doomed.txt")); CHECK(doomed && doc_insert(doomed, 0, "unsaved ", 8) == 0); show_doc(active, d);
    ui_change(ED_LSP_EDIT_CREATE, "side.txt", NULL); ui_change(ED_LSP_EDIT_DELETE, "doomed.txt", NULL);
    push(27, 0, NULL); CHECK(!ui_apply(s, d)); CHECK(qhead == qtail); CHECK(strstr(language_failure, "cancelled"));
    CHECK(!ui_exists("side.txt") && !strcmp(file_bytes(at_dir("doomed.txt")), "doomed\n"));
    CHECK(language_alive(doomed) == doomed && text_is(doomed, "unsaved doomed\n") && doc_modified(doomed));
    ui_change(ED_LSP_EDIT_CREATE, "side.txt", NULL); ui_change(ED_LSP_EDIT_DELETE, "doomed.txt", NULL);
    push('d', 0, NULL); CHECK(ui_apply(s, d)); CHECK(qhead == qtail);
    CHECK(ui_exists("side.txt") && !ui_exists("doomed.txt") && !ui_doc("doomed.txt") && ui_leftovers() == 0);
    /* A folder: not without recursive when it has contents; recursive removes it. */
    CHECK(mkdir(at_dir("folder"), 0755) == 0); if (is_root) CHECK(chown(at_dir("folder"), EDIT_USER, EDIT_USER) == 0);
    put_file(at_dir("folder/inner.txt"), "inner\n");
    ui_change(ED_LSP_EDIT_DELETE, "folder", NULL); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "not empty") && ui_exists("folder/inner.txt"));
    ui_change(ED_LSP_EDIT_DELETE, "folder", NULL)->recursive = TRUE; CHECK(ui_apply(s, d)); CHECK(!ui_exists("folder") && ui_leftovers() == 0);
    ui_change(ED_LSP_EDIT_DELETE, "gone.txt", NULL); CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "does not exist"));
    ui_change(ED_LSP_EDIT_DELETE, "gone.txt", NULL)->ignore_if_not_exists = TRUE; CHECK(ui_apply(s, d));

    /* A failure in the middle: everything before it undone, applied=false. */
    puts("LSP UI: rollback");
    put_file(at_dir("keep.txt"), "keep\n");
    struct language_document *binding = language_binding(d); char *before = text_of(d); size_t done = d->done;
    EdLspDocumentEdit *text = ui_change(ED_LSP_EDIT_TEXT, "sample.cpp", NULL); text->version = binding->version; ui_edit(text, start, "// rolled back\n");
    ui_change(ED_LSP_EDIT_DELETE, "keep.txt", NULL); ui_change(ED_LSP_EDIT_RENAME, "c.txt", "b.txt");
    ui_change(ED_LSP_EDIT_CREATE, "exists.txt", NULL); ui_change(ED_LSP_EDIT_CREATE, "never.txt", NULL);
    CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "already exists"));
    CHECK(text_is(d, before) && d->done == done); free(before);
    CHECK(!strcmp(file_bytes(at_dir("keep.txt")), "keep\n") && !strcmp(file_bytes(at_dir("b.txt")), "bee\n"));
    CHECK(!ui_exists("c.txt") && !ui_exists("never.txt") && ui_leftovers() == 0);
    /* A text edit failing after a CreateFile: the created file is removed and
     * the document opened for it closed again. */
    ui_change(ED_LSP_EDIT_CREATE, "later.txt", NULL); ui_edit(ui_change(ED_LSP_EDIT_TEXT, "later.txt", NULL), (EdLspRange){{3, 0}, {3, 0}}, "x");
    CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "Invalid workspace edit")); CHECK(!ui_exists("later.txt") && !ui_doc("later.txt"));
    /* A stale versioned text edit is found before any file changes. */
    text = ui_change(ED_LSP_EDIT_TEXT, "sample.cpp", NULL); text->version = binding->version + 7; ui_edit(text, start, "x");
    ui_change(ED_LSP_EDIT_RENAME, "c.txt", "b.txt"); ui_change(ED_LSP_EDIT_RENAME, "b.txt", "c.txt");
    CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "Stale")); CHECK(ui_exists("b.txt") && !ui_exists("c.txt"));
    /* Malformed operations are refused before anything changes. */
    ui_change(ED_LSP_EDIT_CREATE, "never.txt", NULL); ui_change(ED_LSP_EDIT_INVALID, "never.txt", NULL);
    CHECK(!ui_apply(s, d)); CHECK(strstr(language_failure, "malformed") && !ui_exists("never.txt"));
    show_doc(active, d);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    setlocale(LC_CTYPE, "C.UTF-8"); is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0); if (is_root) CHECK(chown(argv[1], EDIT_USER, EDIT_USER) == 0);
    char *real = realpath(argv[1], NULL); snprintf(test_dir, sizeof test_dir, "%s", real); free(real);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); spaces[0] = (struct space){.cur = -1, .share = 1000}; CHECK(edit_layout_init(&view_layout) == 0);
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/sample.cpp", test_dir);
    const char *code = "// Unicode 😀 中\nint add(int a, int b) { return a+b; }\nint main() { int value=add(1, 2); return value; }\n";
    CHECK(chmod(test_dir, 0755) == 0); /* mktemp parent must also be searchable by EDIT_USER. */
    char *parent = g_path_get_dirname(test_dir); CHECK(chmod(parent, 0755) == 0); g_free(parent);
    CHECK(g_file_set_contents(path, code, -1, NULL)); if (is_root) CHECK(chown(path, EDIT_USER, EDIT_USER) == 0);
    struct doc *d = open_file(path); CHECK(d); struct tab *t = cur_tab(); CHECK(t && t->doc == d);
    setbuf(stdout, NULL); puts("LSP UI: attach");
    language_init(); language_auto_completion = language_auto_signature = language_auto_hover = 0;
    struct language_document *binding = language_attach(d, 1); CHECK(binding); lsp_ui_wait(binding);
    t->cursor = t->anchor = (size_t)(strstr(code, "value=") - code) + 2;
    /* Rename prompt -> actual clangd response -> actual doc/undo transaction. */
    push('r', 0, NULL); push('e', 0, NULL); push('n', 0, NULL); push('a', 0, NULL); push('m', 0, NULL); push('e', 0, NULL); push('d', 0, NULL); push('\n', 0, NULL);
    ime_set_mode(IME_MODE_EN); puts("LSP UI: rename"); language_request_action(ED_LSP_RENAME); lsp_ui_wait(binding);
    CHECK(strstr(buffer_text(&d->buf), "int renamed")); CHECK(strstr(buffer_text(&d->buf), "return renamed"));
    undo_redo(active, t, 0); language_changed(d); CHECK(text_is(d, code));
    /* Format callback is an undoable real edit, not just a backend result. */
    puts("LSP UI: format"); language_request_action(ED_LSP_FORMAT); lsp_ui_wait(binding); CHECK(strstr(buffer_text(&d->buf), "a + b"));
    undo_redo(active, t, 0); language_changed(d);
    /* Definition response navigates the current editor cursor. */
    t->cursor = t->anchor = (size_t)(strstr(code, "add(1") - code) + 1;
    language_request_action(ED_LSP_DEFINITION); lsp_ui_wait(binding); CHECK(line_of(d, cur_tab()->cursor) == 1);
    /* Signature/hover use real result viewers; Escape closes each. */
    cur_tab()->cursor = cur_tab()->anchor = (size_t)(strstr(code, "add(1") - code) + 1;
    push(27, 0, NULL); language_request_action(ED_LSP_HOVER); lsp_ui_wait(binding);
    /* Language menu actually dispatches Symbol Outline and its selected row. */
    for (int i = 0; i < 8; i++) push(K_DOWN, 0, NULL);
    push('\n', 0, NULL); push('\n', 0, NULL);
    language_menu(); lsp_ui_wait(binding); CHECK(line_of(d, cur_tab()->cursor) == 1);
    /* Cross-file rename-style edits stage every document, then one undo step
     * per document; accepting the confirmation must not drop the other file. */
    char other_path[PATH_MAX]; snprintf(other_path, sizeof other_path, "%s/other.cpp", test_dir);
    CHECK(g_file_set_contents(other_path, "int beta;\n", -1, NULL)); if (is_root) CHECK(chown(other_path, EDIT_USER, EDIT_USER) == 0);
    struct doc *other = open_file(other_path); CHECK(other); struct language_document *other_binding = language_attach(other, 1); CHECK(other_binding); lsp_ui_wait(other_binding);
    GPtrArray *documents = g_ptr_array_new(), *annotations = g_ptr_array_new(), *first_edits = g_ptr_array_new(), *other_edits = g_ptr_array_new();
    EdLspTextEdit first_edit = {.range = {{1, 4}, {1, 7}}, .new_text = "sum"}; EdLspTextEdit other_edit = {.range = {{0, 4}, {0, 8}}, .new_text = "gamma"};
    g_ptr_array_add(first_edits, &first_edit); g_ptr_array_add(other_edits, &other_edit);
    EdLspDocumentEdit first_change = {.uri = binding->uri, .version = binding->version, .edits = first_edits};
    EdLspDocumentEdit other_change = {.uri = other_binding->uri, .version = other_binding->version, .edits = other_edits};
    g_ptr_array_add(documents, &first_change); g_ptr_array_add(documents, &other_change); EdLspWorkspaceEdit workspace = {.documents = documents, .annotations = annotations};
    push('\n', 0, NULL); CHECK(language_apply(binding->server, &workspace, ed_lsp_revision(binding->server->client), d));
    CHECK(strstr(binding->snapshot, "int sum")); CHECK(strstr(other_binding->snapshot, "int gamma"));
    size_t c, a; CHECK(doc_undo(d, &c, &a)); CHECK(doc_undo(other, &c, &a)); language_changed(d); language_changed(other);
    CHECK(text_is(d, code)); CHECK(text_is(other, "int beta;\n"));
    g_ptr_array_unref(documents); g_ptr_array_unref(annotations); g_ptr_array_unref(first_edits); g_ptr_array_unref(other_edits); show_doc(active, d);
    lsp_ui_resources(binding->server, d); lsp_ui_wait(binding);
    /* Save callback sends didSave after the real file writer succeeds. */
    CHECK(save(d) == 0); language_saved(d); lsp_ui_wait(binding);
    language_done(); CHECK(language_servers == NULL); puts("PASS actual editor LSP callbacks: clangd rename/undo/format/navigation/hover/menu/save/cleanup, workspace create/rename/delete/rollback");
    return 0;
}
