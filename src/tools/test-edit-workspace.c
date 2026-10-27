/* Real editor actions and pointer dispatch with the existing device fixture. */
#ifndef EDIT_WORKSPACE_HARNESS
#error Build with test-edit-workspace.sh
#endif
#include "base.h"
#include "i18n-stub.h"
#include EDIT_WORKSPACE_HARNESS

static void point(int x, int y, int button, int pressed, int buttons)
{
    ptr_now = (struct pointer_event){ .x = x * 10 + 2, .y = y * 20 + 2, .col = x, .row = y,
                                     .button = button, .pressed = pressed, .buttons = buttons, .clicks = 1 };
    mods_now = 0;
    key_event(K_POINTER);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(mkdir(argv[1], 0755) == 0 && realpath(argv[1], test_dir));
    if (geteuid() == 0) CHECK(chown(test_dir, EDIT_USER, EDIT_USER) == 0);
    reset();
    struct tab *t = open_text("workspace.txt", "line zero\nline one\nline two\nline three\nline four\n");
    struct doc *d = t->doc;
    draw();
    CHECK(on_screen("File") >= 0 && on_screen("Edit") >= 0 && on_screen("View") >= 0 && on_screen("Sessions") >= 0);
    CHECK(ui_hit_at(1, 1) == HIT_MENU);
    push(K_DOWN, 0, NULL); push('\n', 0, NULL); /* Open... menu item */
    int before = picks;
    point(1, 1, BTN_LEFT, 1, 1);
    CHECK(picks == before + 1);
    draw();
    editor_action(EA_SPLIT_H);
    CHECK(nspaces == 2 && active == 1 && tab_of(0)->doc == d && tab_of(1)->doc == d);
    struct edit_rect upper = space_rect(0), lower = space_rect(1);
    CHECK(upper.y == 2 && lower.y == upper.y + upper.h + 1 && upper.x == lower.x);
    int top, height;
    text_rows(1, &top, &height);
    CHECK(top > lower.y && top + height <= lower.y + lower.h - 1);
    draw();
    CHECK(ui_hit_at(lower.x + 1, lower.y) >= HIT_TAB);
    struct edit_split *separator = edit_layout_separator(&view_layout, editor_area(), 5, lower.y - 1, NULL);
    CHECK(separator && !separator->vertical);
    int old = separator->ratio;
    point(5, lower.y - 1, BTN_LEFT, 1, 1);
    point(5, lower.y + 2, 0, 0, 1);
    point(5, lower.y + 2, BTN_LEFT, 0, 0);
    CHECK(separator->ratio > old && !split_drag);
    editor_action(EA_SPLIT_V);
    CHECK(nspaces == 3);
    struct edit_rect left = space_rect(1), right = space_rect(2);
    CHECK(right.x > left.x && right.y == left.y && right.h == left.h);
    editor_action(EA_TOGGLE_SPLIT);
    left = space_rect(1); right = space_rect(2);
    CHECK(right.y > left.y && right.x == left.x);
    editor_action(EA_CLOSE_OTHER_VIEWS);
    CHECK(nspaces == 1 && ndocs == 1 && cur_tab()->doc == d);
    editor_action(EA_PROJECTS);
    CHECK(sidebar);
    draw();
    CHECK(ui_hit_at(SIDE_W - 2, 2) == HIT_TREE_CLOSE);
    old = sidebar_width;
    point(SIDE_W, 8, BTN_LEFT, 1, 1);
    point(old + 7, 8, 0, 0, 1);
    point(old + 7, 8, BTN_LEFT, 0, 0);
    CHECK(sidebar_width == old + 7 && !sidebar_drag);
    draw();
    point(SIDE_W - 2, 2, BTN_LEFT, 1, 1);
    CHECK(!sidebar && !tree_focus);
    reset();
    puts("PASS actual editor menus, horizontal/nested splits, resize/toggle/close, tool-view drag and close");
}
