/* open.c's host programs (host="1", desktop=): their windows and the types
 * they open come from the .desktop files they name in /usr/share/applications,
 * not from a container's upper, and no other program's file is taken for
 * theirs. The framebuffer programs (program:NAME), types as GIO finds and
 * relates them over the system's shared-mime-info database (read only), and
 * open_in_home's request. As root, in a mount namespace of its own: tmpfs
 * over /etc/locachy, /var/lib/locachy/containers, /usr/share/applications,
 * OPEN_PROGRAM_DESKTOP, /run/locachy and a scratch folder, with fixtures;
 * nothing of the host is written. Build with base/rc.c, base/say.c,
 * base/control.c, gio-2.0, -ffunction-sections -fdata-sections
 * -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../base/open.c"
#include <assert.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>

/* The folders up to path's last /. */
static void folders(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; (p = strchr(p, '/')) != NULL; p++) {
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
}

static void put(const char *path, const char *text)
{
    folders(path);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* path's type is want, or an alias of it (shared-mime-info renames types:
 * application/x-shellscript is text/x-shellscript now). */
static int typed(const char *path, const char *want)
{
    char type[OPEN_TYPE];
    return open_type(path, type, sizeof type) == 0 && g_content_type_equals(type, want);
}

int main(void)
{
    assert(unshare(CLONE_NEWNS) == 0 && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    const char *dirs[] = { "/etc/locachy", CONTAINERS_CONF, CONTAINERS, "/usr/share/applications",
                           OPEN_PROGRAM_DESKTOP, "/run/locachy", "/var/tmp/locachy-open-host" };
    for (int i = 0; i < 7; i++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/", dirs[i]);
        folders(dir);
        assert(mount("tmpfs", dirs[i], "tmpfs", 0, "mode=0755") == 0);
    }
    put(CONTAINERS_CONF "/editor.conf", "host=\"1\"\ndesktop=\"ed.desktop ../evil.desktop a/b.desktop ed-url.desktop\"\n");
    put(CONTAINERS_CONF "/box.conf", "package=\"box\"\n");
    put("/usr/share/applications/ed.desktop",
        "[Desktop Entry]\nName=Editor\nMimeType=text/plain;\nStartupWMClass=ed-wm\n");
    put("/usr/share/applications/ed-url.desktop", "[Desktop Entry]\nName=Editor URL\nMimeType=x-scheme-handler/ed;\n");
    put("/usr/share/applications/other.desktop", "[Desktop Entry]\nName=Other\nMimeType=text/plain;image/png;\n");
    put(CONTAINERS "/box/upper/usr/share/applications/box.desktop", "[Desktop Entry]\nName=Box\nMimeType=image/png;\n");

    assert(open_host("editor") && !open_host("box") && !open_host("../editor") && !open_host("none"));
    char ids[8][OPEN_WINDOW_ID];
    int n = open_window_ids("editor", ids, 8);
    assert(n == 3 && !strcmp(ids[0], "ed") && !strcmp(ids[1], "ed-wm") && !strcmp(ids[2], "ed-url"));
    n = open_window_ids("box", ids, 8);
    assert(n == 1 && !strcmp(ids[0], "box"));
    puts("PASS a host program's windows: the .desktop files it names, not every program's");

    char names[8][OPEN_NAME], label[128];
    n = open_candidates("text/plain", names, 8);
    assert(n == 1 && !strcmp(names[0], "editor")); /* other.desktop is nobody's */
    n = open_candidates("image/png", names, 8);
    assert(n == 1 && !strcmp(names[0], "box"));
    n = open_candidates("x-scheme-handler/ed", names, 8);
    assert(n == 1 && !strcmp(names[0], "editor"));
    open_label("editor", "text/plain", label, sizeof label);
    assert(!strcmp(label, "editor (Editor)"));
    n = open_containers(names, 8);
    assert(n == 2 && !strcmp(names[0], "box") && !strcmp(names[1], "editor"));
    puts("PASS what a host program opens and its name come from its own .desktop files");

    /* Files of each case, typed by GIO over the system's MIME database. */
    const char *t = "/var/tmp/locachy-open-host";
    char path[256];
#define AT(name) (snprintf(path, sizeof path, "%s/%s", t, name), path)
    put(AT("a.md"), "# Title\n\nSome *text*.\n");
    assert(typed(path, "text/markdown"));
    put(AT("a.MD"), "# Title\n");
    assert(typed(path, "text/markdown")); /* globs ignore case unless they say cs */
    put(AT("c.c"), "#include <stdio.h>\nint main(void) { return 0; }\n");
    assert(typed(path, "text/x-csrc"));
    FILE *zip = fopen(AT("e.cbz"), "w"); /* an empty zip: its end record only */
    assert(zip && fwrite("PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 1, 22, zip) == 22 && fclose(zip) == 0);
    assert(typed(path, "application/vnd.comicbook+zip"));
    put(AT("x.fb2"), "<?xml version=\"1.0\"?>\n<FictionBook></FictionBook>\n");
    assert(typed(path, "application/x-fictionbook+xml"));
    put(AT("CMakeLists.txt"), "project(x)\n");
    assert(typed(path, "text/x-cmake")); /* the whole name before *.txt */
    put(AT("README.md"), "# Readme\n");
    assert(typed(path, "text/markdown")); /* *.md weighs more than README* */
    put(AT("f.txt"), "plain\n");
    assert(typed(path, "text/plain"));
    put(AT("document"), "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n1 0 obj << >> endobj\ntrailer << >>\n%%EOF\n");
    assert(typed(path, "application/pdf")); /* no glob: the magic */
    put(AT("script"), "#!/bin/sh\necho hi\n");
    assert(typed(path, "application/x-shellscript"));
    assert(mkfifo(AT("pipe"), 0600) == 0 && typed(path, "inode/fifo")); /* not read: it would wait */
    assert(typed(t, "inode/directory") && typed("/dev/null", "inode/chardevice"));
    assert(open_type(AT("missing"), label, sizeof label) < 0 && open_type("relative", label, sizeof label) < 0);
    assert(typed("HTTPS://example.org", "x-scheme-handler/https"));
    puts("PASS types: the name's by the globs, else the magic; kinds of files; URLs by scheme");

    assert(lists("text/plain;", "text/x-csrc") && lists("text/plain;", "text/x-c")); /* an alias is its type */
    assert(lists("application/x-cbz;", "application/vnd.comicbook+zip") && lists("image/*;", "image/png"));
    assert(lists("application/zip;", "application/vnd.comicbook+zip") && !lists("image/png;", "image/jpeg"));
    assert(lists("x-scheme-handler/https;", "x-scheme-handler/https") &&
           !lists("application/octet-stream;", "x-scheme-handler/https") && !lists(";;", "text/plain"));
    puts("PASS a type is opened by what lists it, an alias of it, a type it is a kind of, or its major type");

    /* The framebuffer programs: home's (rc.conf program_NAME) with a .desktop file. */
    put("/etc/locachy/rc.conf", "program_term=\"/usr/local/bin/term\"\nprogram_edit=\"/usr/local/bin/edit --x\"\n"
                                "program_view=\"/usr/local/bin/view\"\nprogram_files=\"/x\"\nprogram_desktop=\"/y\"\n");
    put(OPEN_PROGRAM_DESKTOP "/view.desktop", "[Desktop Entry]\nName=Document viewer\n"
                                              "MimeType=application/pdf;application/x-cbz;\n");
    put(OPEN_PROGRAM_DESKTOP "/edit.desktop", "[Desktop Entry]\nName=Text editor\nMimeType=text/plain;\n");
    put(OPEN_PROGRAM_DESKTOP "/files.desktop", "[Desktop Entry]\nName=Files\nMimeType=text/plain;\n");
    put(OPEN_PROGRAM_DESKTOP "/gone.desktop", "[Desktop Entry]\nName=Gone\nMimeType=text/plain;\n");
    n = open_candidates("text/x-csrc", names, 8);
    assert(n == 2 && !strcmp(names[0], "program:edit") && !strcmp(names[1], "editor"));
    n = open_candidates("text/x-c", names, 8); /* an alias is its type */
    assert(n == 2 && !strcmp(names[0], "program:edit"));
    n = open_candidates("application/vnd.comicbook+zip", names, 8);
    assert(n == 1 && !strcmp(names[0], "program:view"));
    n = open_candidates("text/x-csrc", names, 1);
    assert(n == 1 && !strcmp(names[0], "program:edit"));
    open_label("program:view", "application/pdf", label, sizeof label);
    assert(!strcmp(label, "view (Document viewer)"));
    n = open_containers(names, 8);
    assert(n == 2 && !strcmp(names[0], "box")); /* the programs are not containers */
    puts("PASS a framebuffer program is a candidate for its .desktop file's types and their kinds");

    char spec[256], *argv[16];
    assert(open_program("program:edit", "/a b/c", spec, sizeof spec, argv, 16) == 3);
    assert(!strcmp(argv[0], "/usr/local/bin/edit") && !strcmp(argv[1], "--x") && !strcmp(argv[2], "/a b/c"));
    assert(argv[3] == NULL);
    assert(open_program("program:edit", "/a", spec, sizeof spec, argv, 3) == 2 && !strcmp(argv[1], "/a") && !argv[2]);
    assert(!open_program("program:files", "/a", spec, sizeof spec, argv, 16));
    assert(!open_program("program:gone", "/a", spec, sizeof spec, argv, 16));
    assert(!open_program("program:../view", "/a", spec, sizeof spec, argv, 16));
    assert(!open_program("editor", "/a", spec, sizeof spec, argv, 16));
    puts("PASS a program's command: rc.conf's, with the target; files, desktop and others are none");

    char rule[OPEN_NAME];
    put(OPEN_RULES, "text/x-c=program:edit\nimage/*=box\napplication/pdf=program:../x\n");
    assert(open_rule("text/x-csrc", rule, sizeof rule) == OPEN_RULE_CONTAINER && !strcmp(rule, "program:edit"));
    assert(open_rule("image/png", rule, sizeof rule) == OPEN_RULE_CONTAINER && !strcmp(rule, "box"));
    assert(open_rule("application/pdf", rule, sizeof rule) == OPEN_NO_RULE);
    puts("PASS rules: a program's, and one written for an alias");

    assert(open_in_home(NULL, "https://example.org/a") == 0);
    FILE *in = fopen(OPEN_REQUEST, "r");
    char text[512] = "";
    assert(in && fread(text, 1, sizeof text - 1, in) > 0 && fclose(in) == 0);
    assert(!strcmp(text, "\nhttps://example.org/a\n"));
    assert(open_in_home("program:view", "/x/y.pdf") == 0);
    in = fopen(OPEN_REQUEST, "r");
    memset(text, 0, sizeof text);
    assert(in && fread(text, 1, sizeof text - 1, in) > 0 && fclose(in) == 0);
    assert(!strcmp(text, "program:view\n/x/y.pdf\n"));
    assert(open_in_home(NULL, "/x\n/y") < 0 && open_in_home("a\nb", "/x") < 0);
    DIR *run = opendir("/run/locachy");
    struct dirent *e;
    int files = 0;
    while (run && (e = readdir(run)) != NULL) files += e->d_name[0] != '.';
    assert(run && closedir(run) == 0 && files == 1); /* no temporary file left */
    puts("PASS open_in_home: with and target, whole; a newline is refused");
    puts("ALL PASSED");
    return 0;
}
