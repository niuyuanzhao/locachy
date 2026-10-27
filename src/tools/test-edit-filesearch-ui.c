/* Reuse the framebuffer editor harness without changing its owned source. */
#ifndef EDIT_FILESEARCH_HARNESS
#error Build with test-edit-filesearch-ui.sh
#endif
#include "base.h"
#include "i18n-stub.h"
#include EDIT_FILESEARCH_HARNESS
#include "../progs/edit-filesearch-ui.inc"

static void filesearch_test_wait(void)
{
    double deadline = ms() + 10000;
    while (filesearch_active() && ms() < deadline) {
        struct pollfd p = {edit_filesearch_fd(filesearch_ui.search), POLLIN, 0};
        (void)poll(&p, 1, 10); filesearch_tick();
    }
    CHECK(!filesearch_active()); CHECK(!*edit_filesearch_error(filesearch_ui.search));
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); setlocale(LC_CTYPE, "C.UTF-8"); is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0);
    char *real = realpath(argv[1], NULL); snprintf(test_dir, sizeof test_dir, "%s", real); free(real);
    if (is_root) CHECK(chown(test_dir, EDIT_USER, EDIT_USER) == 0);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    struct tab *t = open_text("filesearch-open.txt", "needle one\nneedle two\n");
    filesearch_init();
    free(filesearch_ui.pattern); filesearch_ui.pattern = strdup("needle");
    filesearch_ui.scope = 2;
    CHECK(filesearch_start_ui()); filesearch_test_wait();
    CHECK(edit_filesearch_count(filesearch_ui.search) == 2);
    struct edit_filematch *match = edit_filesearch_match(filesearch_ui.search, 0);
    CHECK(match->from_buffer); filesearch_open_match(match); CHECK(cur_tab()->cursor == 6 && cur_tab()->anchor == 0);
    /* Replace first checked occurrence only; prompt then explicit confirmation. */
    edit_filesearch_match(filesearch_ui.search, 1)->checked = 0;
    push('w', 0, NULL); push('o', 0, NULL); push('r', 0, NULL); push('d', 0, NULL); push('\n', 0, NULL); push('r', 0, NULL);
    filesearch_replace_checked(); CHECK(qhead == qtail);
    CHECK(doc_len(t->doc) == strlen("word one\nneedle two\n") && !memcmp(buffer_text(&t->doc->buf), "word one\nneedle two\n", doc_len(t->doc))); CHECK(doc_modified(t->doc));
    CHECK(!strcmp(file_bytes(at_dir("filesearch-open.txt")), "needle one\nneedle two\n"));
    size_t cursor, anchor; CHECK(doc_undo(t->doc, &cursor, &anchor));
    CHECK(doc_len(t->doc) == strlen("needle one\nneedle two\n") && !memcmp(buffer_text(&t->doc->buf), "needle one\nneedle two\n", doc_len(t->doc)));
    /* Folder and current-project scopes must search unsaved text, not the old
     * bytes still on disk. Their results must remain buffer replacements. */
    doc_safe_point(t->doc); CHECK(doc_insert(t->doc, 0, "unsaved-project-only ", 21) == 0); doc_safe_point(t->doc);
    free(filesearch_ui.pattern); filesearch_ui.pattern = strdup("unsaved-project-only");
    filesearch_ui.scope = 0; free(filesearch_ui.root); filesearch_ui.root = strdup(test_dir);
    CHECK(filesearch_start_ui()); filesearch_test_wait();
    CHECK(edit_filesearch_count(filesearch_ui.search) == 1 && edit_filesearch_match(filesearch_ui.search, 0)->from_buffer);
    CHECK(!strstr(file_bytes(at_dir("filesearch-open.txt")), "unsaved-project-only"));
    CHECK(project && !strcmp(project->root, test_dir)); filesearch_ui.scope = 1;
    CHECK(filesearch_start_ui()); filesearch_test_wait();
    CHECK(edit_filesearch_count(filesearch_ui.search) == 1 && edit_filesearch_match(filesearch_ui.search, 0)->from_buffer);
    free(filesearch_ui.pattern); filesearch_ui.pattern = strdup("needle");
    /* Disk scope only the generated fixture; not any home/project root. */
    put_file(at_dir("filesearch-disk.txt"), "needle disk\r\n");
    filesearch_ui.scope = 0; free(filesearch_ui.root); filesearch_ui.root = strdup(test_dir);
    free(filesearch_ui.include); filesearch_ui.include = strdup("filesearch-disk.txt");
    CHECK(filesearch_start_ui()); filesearch_test_wait(); CHECK(edit_filesearch_count(filesearch_ui.search) == 1);
    push('\n', 0, NULL); push('r', 0, NULL); filesearch_replace_checked(); CHECK(qhead == qtail);
    CHECK(!strcmp(file_bytes(at_dir("filesearch-disk.txt")), "word disk\r\n"));
    /* Cancel configuration/results menus: real paths execute without mutation. */
    push(27, 0, NULL); filesearch_menu(); CHECK(qhead == qtail);
    push(27, 0, NULL); filesearch_results_ui(); CHECK(qhead == qtail);
    /* Results view drains the real socket while waiting, then repaints without
     * blocking on the entire search or leaking keys into the open document. */
    CHECK(filesearch_start_ui());
    const char *const progress[] = {"Searching", "Close"}; int focus = 0;
    for (int i = 0; i < 1000 && filesearch_active(); i++) {
        int result = filesearch_result_menu(progress, 2, &focus);
        CHECK(result == -2);
    }
    CHECK(!filesearch_active() && edit_filesearch_count(filesearch_ui.search) == 0);
    filesearch_done(); CHECK(!filesearch_active());
    puts("PASS Find in Files actual UI adapter: start/tick/open/checked buffer undo, disk CRLF, menus and cleanup");
    return 0;
}
