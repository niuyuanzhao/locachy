/*
 * test-docscan: unit and integration tests for locachy-docscan.
 *
 * Usage:
 *   test-docscan DOCSCAN_BIN [--no-pdf]
 *                [--fixture-pdf PATH] [--fixture-docx PATH]
 *                [--fixture-odt PATH] [--fixture-epub PATH]
 *
 * Generates self-contained fixtures in a temporary directory under
 * $CLAUDE_JOB_DIR/tmp (falling back to $TMPDIR or /tmp):
 *   - plain text (UTF-8, BOM, CJK, invalid UTF-8 -> U+FFFD replacement)
 *   - extensionless text (sniff) and binary file (NUL / non-UTF-8 rejection)
 *   - 2 MiB multi-byte UTF-8 text (1 MiB output cap without splitting UTF-8)
 *   - >8 MiB file (early rejection with non-zero exit + single stderr line)
 *   - symlink and directory (O_NOFOLLOW / regular-file rejection)
 *   - closed stdout / SIGPIPE quiet exit
 *   - extra open fd closed before extraction
 *   - PDF (hand-crafted 2-page PDF 1.4, plus optional external --fixture-pdf)
 *   - DOCX, ODT, EPUB built via libarchive (tag stripping, entity decoding,
 *     whitespace collapsing, <script>/<style> skipping, malformed/missing-member
 *     and oversized-member rejections), plus optional external fixtures.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_OUTPUT_BYTES (1u * 1024u * 1024u)
#define MAX_INPUT_BYTES  (8u * 1024u * 1024u)

static const char *docscan_bin;
static char workdir[PATH_MAX];
static int failures;
static int passed;

static void record(int ok, const char *name, const char *detail)
{
    if (ok) {
        passed++;
        printf("PASS %s\n", name);
    } else {
        failures++;
        printf("FAIL %s%s%s\n", name, detail && *detail ? ": " : "", detail ? detail : "");
    }
    fflush(stdout);
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    (void)remove(path);
    return 0;
}

static void cleanup_workdir(void)
{
    if (workdir[0]) {
        (void)nftw(workdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        workdir[0] = '\0';
    }
}

static void make_workdir(void)
{
    const char *base = getenv("CLAUDE_JOB_DIR");
    char tmpl[PATH_MAX];
    if (base && *base) {
        char tmpdir[PATH_MAX];
        if (snprintf(tmpdir, sizeof tmpdir, "%s/tmp", base) >= (int)sizeof tmpdir) {
            fprintf(stderr, "test-docscan: CLAUDE_JOB_DIR too long\n");
            exit(2);
        }
        (void)mkdir(tmpdir, 0700);
        if (snprintf(tmpl, sizeof tmpl, "%s/docscan-test-XXXXXX", tmpdir) >= (int)sizeof tmpl) {
            fprintf(stderr, "test-docscan: temp path too long\n");
            exit(2);
        }
    } else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        if (snprintf(tmpl, sizeof tmpl, "%s/docscan-test-XXXXXX", tmp) >= (int)sizeof tmpl) {
            fprintf(stderr, "test-docscan: temp path too long\n");
            exit(2);
        }
    }
    if (!mkdtemp(tmpl)) {
        fprintf(stderr, "test-docscan: mkdtemp(%s): %s\n", tmpl, strerror(errno));
        exit(2);
    }
    snprintf(workdir, sizeof workdir, "%s", tmpl);
}

static void join_path(char *dst, size_t size, const char *name)
{
    if (snprintf(dst, size, "%s/%s", workdir, name) >= (int)size) {
        fprintf(stderr, "test-docscan: path too long: %s\n", name);
        exit(2);
    }
}

static void write_file_bytes(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "test-docscan: create %s: %s\n", path, strerror(errno));
        exit(2);
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, (const unsigned char *)data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            fprintf(stderr, "test-docscan: write %s: %s\n", path, strerror(errno));
            close(fd);
            exit(2);
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "test-docscan: close %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

/* ---- Subprocess runner ---- */

struct run_result {
    int exit_code;     /* >=0 if exited normally, -sig if killed by signal */
    unsigned char *out;
    size_t out_len;
    char *err;
    size_t err_len;
};

static void run_result_free(struct run_result *r)
{
    free(r->out);
    free(r->err);
    memset(r, 0, sizeof *r);
}

static void append_buf(unsigned char **buf, size_t *len, size_t *cap, const unsigned char *src, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2u : 4096u;
        while (ncap < *len + n + 1) ncap *= 2u;
        unsigned char *nb = realloc(*buf, ncap);
        if (!nb) {
            fprintf(stderr, "test-docscan: out of memory\n");
            exit(2);
        }
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/* Run docscan_bin with argv_file (or no args if NULL). */
static struct run_result run_docscan(const char *argv_file)
{
    struct run_result res;
    memset(&res, 0, sizeof res);
    res.exit_code = -1;

    int out_p[2], err_p[2];
    if (pipe2(out_p, O_CLOEXEC) != 0 || pipe2(err_p, O_CLOEXEC) != 0) {
        fprintf(stderr, "test-docscan: pipe2: %s\n", strerror(errno));
        exit(2);
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "test-docscan: fork: %s\n", strerror(errno));
        exit(2);
    }
    if (pid == 0) {
        (void)dup2(out_p[1], STDOUT_FILENO);
        (void)dup2(err_p[1], STDERR_FILENO);
        close(out_p[0]);
        close(out_p[1]);
        close(err_p[0]);
        close(err_p[1]);
        if (argv_file) {
            execl(docscan_bin, docscan_bin, argv_file, (char *)NULL);
        } else {
            execl(docscan_bin, docscan_bin, (char *)NULL);
        }
        _exit(127);
    }

    close(out_p[1]);
    close(err_p[1]);

    size_t out_cap = 0, err_cap = 0;
    append_buf(&res.out, &res.out_len, &out_cap, (const unsigned char *)"", 0);
    unsigned char *err_raw = NULL;
    append_buf(&err_raw, &res.err_len, &err_cap, (const unsigned char *)"", 0);

    int open_fds = 2;
    struct pollfd pfds[2] = {
        { .fd = out_p[0], .events = POLLIN },
        { .fd = err_p[0], .events = POLLIN },
    };
    while (open_fds > 0) {
        int pr = poll(pfds, 2, 15000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (pfds[i].fd < 0) continue;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                unsigned char tmp[8192];
                ssize_t n = read(pfds[i].fd, tmp, sizeof tmp);
                if (n > 0) {
                    if (i == 0) append_buf(&res.out, &res.out_len, &out_cap, tmp, (size_t)n);
                    else append_buf(&err_raw, &res.err_len, &err_cap, tmp, (size_t)n);
                } else if (n == 0 || (n < 0 && errno != EINTR)) {
                    close(pfds[i].fd);
                    pfds[i].fd = -1;
                    open_fds--;
                }
            }
        }
    }
    if (pfds[0].fd >= 0) close(pfds[0].fd);
    if (pfds[1].fd >= 0) close(pfds[1].fd);

    res.err = (char *)err_raw;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.exit_code = -WTERMSIG(status);
    return res;
}

static int valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c == 0) return 0; /* no NUL bytes in extracted UTF-8 text */
        size_t len = c < 0x80u ? 1 : (c & 0xE0u) == 0xC0u ? 2 : (c & 0xF0u) == 0xE0u ? 3 : (c & 0xF8u) == 0xF0u ? 4 : 0;
        if (!len || i + len > n || (len == 2 && c < 0xC2u)) return 0;
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0u) != 0x80u) return 0;
        }
        if (len == 3 && ((c == 0xE0u && s[i + 1] < 0xA0u) || (c == 0xEDu && s[i + 1] >= 0xA0u))) return 0;
        if (len == 4 && ((c == 0xF0u && s[i + 1] < 0x90u) || c > 0xF4u || (c == 0xF4u && s[i + 1] >= 0x90u))) return 0;
        i += len;
    }
    return 1;
}

static int stderr_is_at_most_one_short_line(const char *err, size_t len)
{
    if (len == 0) return 1;
    if (len > 600) return 0;
    size_t newlines = 0;
    for (size_t i = 0; i < len; i++) {
        if (err[i] == '\n') {
            newlines++;
            if (i + 1 < len) return 0; /* newline before end means >1 line */
        }
    }
    return newlines <= 1;
}

/* ---- Fixture builders ---- */

struct zip_entry_spec {
    const char *name;
    const void *data;
    size_t len;
};

static void make_zip_fixture(const char *path, const struct zip_entry_spec *entries, size_t nentries)
{
    struct archive *w = archive_write_new();
    if (!w || archive_write_set_format_zip(w) != ARCHIVE_OK ||
        archive_write_open_filename(w, path) != ARCHIVE_OK) {
        fprintf(stderr, "test-docscan: cannot create zip %s: %s\n", path, w ? archive_error_string(w) : "oom");
        exit(2);
    }
    for (size_t i = 0; i < nentries; i++) {
        struct archive_entry *e = archive_entry_new();
        if (!e) exit(2);
        archive_entry_set_pathname(e, entries[i].name);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, (la_int64_t)entries[i].len);
        if (archive_write_header(w, e) != ARCHIVE_OK) {
            fprintf(stderr, "test-docscan: zip header %s: %s\n", entries[i].name, archive_error_string(w));
            exit(2);
        }
        if (entries[i].len > 0) {
            la_ssize_t wr = archive_write_data(w, entries[i].data, entries[i].len);
            if (wr < 0 || (size_t)wr != entries[i].len) {
                fprintf(stderr, "test-docscan: zip data %s: %s\n", entries[i].name, archive_error_string(w));
                exit(2);
            }
        }
        archive_entry_free(e);
    }
    if (archive_write_close(w) != ARCHIVE_OK) {
        fprintf(stderr, "test-docscan: zip close %s: %s\n", path, archive_error_string(w));
        exit(2);
    }
    archive_write_free(w);
}

/* Create a zip containing one member of declared/actual size `big_len` filled
 * with spaces to test the per-member size cap without taking much disk space. */
static void make_oversized_member_docx(const char *path, size_t big_len)
{
    struct archive *w = archive_write_new();
    if (!w || archive_write_set_format_zip(w) != ARCHIVE_OK ||
        archive_write_open_filename(w, path) != ARCHIVE_OK) {
        fprintf(stderr, "test-docscan: cannot create big docx %s\n", path);
        exit(2);
    }
    struct archive_entry *e = archive_entry_new();
    if (!e) exit(2);
    archive_entry_set_pathname(e, "word/document.xml");
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, (la_int64_t)big_len);
    if (archive_write_header(w, e) != ARCHIVE_OK) exit(2);

    char chunk[65536];
    memset(chunk, 'A', sizeof chunk);
    size_t left = big_len;
    while (left > 0) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        if (archive_write_data(w, chunk, n) != (la_ssize_t)n) exit(2);
        left -= n;
    }
    archive_entry_free(e);
    if (archive_write_close(w) != ARCHIVE_OK) exit(2);
    archive_write_free(w);
}

/* Hand-crafted minimal 2-page PDF 1.4 using Base14 Helvetica font. */
static void make_pdf_fixture(const char *path)
{
    char buf[8192];
    size_t pos = 0;
    size_t offsets[8] = { 0 };

    const char *s1 = "BT /F1 18 Tf 72 720 Td (Hello PDF quokka page one) Tj ET\n";
    const char *s2 = "BT /F1 18 Tf 72 720 Td (Second page pangolin marker) Tj ET\n";

    pos += (size_t)snprintf(buf + pos, sizeof buf - pos, "%%PDF-1.4\n");

    offsets[1] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    offsets[2] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>\nendobj\n");

    offsets[3] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");

    offsets[4] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");

    offsets[5] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "5 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
        strlen(s1), s1);

    offsets[6] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "6 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 7 0 R >>\nendobj\n");

    offsets[7] = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "7 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
        strlen(s2), s2);

    size_t xref_off = pos;
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "xref\n0 8\n0000000000 65535 f \n");
    for (int i = 1; i <= 7; i++) {
        pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
            "%010zu 00000 n \n", offsets[i]);
    }
    pos += (size_t)snprintf(buf + pos, sizeof buf - pos,
        "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
        xref_off);

    write_file_bytes(path, buf, pos);
}

/* ---- Individual tests ---- */

static void test_usage(void)
{
    struct run_result r = run_docscan(NULL);
    int ok = r.exit_code != 0 && r.out_len == 0 &&
             r.err_len > 0 && strstr(r.err, "usage:") != NULL &&
             stderr_is_at_most_one_short_line(r.err, r.err_len);
    record(ok, "usage error on missing argument", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_plain_text_and_bom(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "sample.txt");
    const char *body = "\xEF\xBB\xBFHello LOCachy search \xE4\xB8\xAD\xE6\x96\x87\nLine two.\n";
    write_file_bytes(p, body, strlen(body));

    struct run_result r = run_docscan(p);
    const char *want = "Hello LOCachy search \xE4\xB8\xAD\xE6\x96\x87\nLine two.\n";
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(want) && memcmp(r.out, want, r.out_len) == 0;
    record(ok, "plain text UTF-8 with BOM", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_text_extension_with_pdf_string(void)
{
    /* A .md file mentioning "%PDF-1.4" must stay plain text, not be misdetected as PDF. */
    char p[PATH_MAX];
    join_path(p, sizeof p, "notes.md");
    const char *body = "# Notes\nA PDF starts with %PDF-1.4 header.\n";
    write_file_bytes(p, body, strlen(body));

    struct run_result r = run_docscan(p);
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(body) && memcmp(r.out, body, r.out_len) == 0;
    record(ok, "text extension containing %PDF- stays plain text", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_invalid_utf8_replacement(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "latin1.txt");
    const unsigned char raw[] = { 'c', 'a', 'f', 0xE9, ' ', 0xFF, ' ', 'o', 'k', '\n' };
    write_file_bytes(p, raw, sizeof raw);

    struct run_result r = run_docscan(p);
    const char *want = "caf\xEF\xBF\xBD \xEF\xBF\xBD ok\n";
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             valid_utf8(r.out, r.out_len) &&
             r.out_len == strlen(want) && memcmp(r.out, want, r.out_len) == 0;
    record(ok, "invalid UTF-8 replaced by U+FFFD", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_sniff_text_no_extension(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "README");
    const char *body = "Extensionless UTF-8 document: \xE6\x90\x9C\xE7\xB4\xA2\n";
    write_file_bytes(p, body, strlen(body));

    struct run_result r = run_docscan(p);
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(body) && memcmp(r.out, body, r.out_len) == 0;
    record(ok, "extensionless UTF-8 sniff", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_binary_rejected(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "blob.bin");
    unsigned char raw[256];
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = (unsigned char)i;
    raw[0] = 0x7F;
    raw[1] = 'E';
    raw[2] = 'L';
    raw[3] = 'F';
    raw[4] = 0x00;
    write_file_bytes(p, raw, sizeof raw);

    struct run_result r = run_docscan(p);
    int ok = r.exit_code != 0 && r.out_len == 0 &&
             r.err_len > 0 && stderr_is_at_most_one_short_line(r.err, r.err_len);
    record(ok, "binary file rejected", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_output_cap_1mib_utf8_boundary(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "big-utf8.txt");
    /* Repeating 3-byte UTF-8 CJK character (U+4E2D) -> 1.5 MiB input, capped at <= 1 MiB
     * on an exact UTF-8 character boundary (1048576 % 3 == 1, so max clean length is 1048575). */
    size_t total = 1536u * 1024u;
    unsigned char *buf = malloc(total);
    if (!buf) exit(2);
    for (size_t i = 0; i + 3 <= total; i += 3) {
        buf[i] = 0xE4;
        buf[i + 1] = 0xB8;
        buf[i + 2] = 0xAD;
    }
    write_file_bytes(p, buf, total - (total % 3u));
    free(buf);

    struct run_result r = run_docscan(p);
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len <= MAX_OUTPUT_BYTES &&
             r.out_len == (MAX_OUTPUT_BYTES / 3u) * 3u &&
             valid_utf8(r.out, r.out_len);
    char msg[128];
    snprintf(msg, sizeof msg, "exit=%d out_len=%zu err=%.64s", r.exit_code, r.out_len, r.err ? r.err : "");
    record(ok, "1 MiB output cap keeps UTF-8 boundary intact", ok ? NULL : msg);
    run_result_free(&r);
}

static void test_oversized_8mib_rejected(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "huge.txt");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)(MAX_INPUT_BYTES + 1u)) != 0 || close(fd) != 0) {
        fprintf(stderr, "test-docscan: cannot create sparse >8 MiB file: %s\n", strerror(errno));
        exit(2);
    }

    struct run_result r = run_docscan(p);
    int ok = r.exit_code != 0 && r.out_len == 0 &&
             r.err_len > 0 && stderr_is_at_most_one_short_line(r.err, r.err_len);
    record(ok, "file over 8 MiB rejected early", ok ? NULL : r.err);
    run_result_free(&r);
}

static void test_symlink_and_dir_rejected(void)
{
    char target[PATH_MAX], linkp[PATH_MAX], fifop[PATH_MAX];
    join_path(target, sizeof target, "target.txt");
    join_path(linkp, sizeof linkp, "link.txt");
    join_path(fifop, sizeof fifop, "named.fifo");
    write_file_bytes(target, "secret\n", 7);
    if (symlink(target, linkp) != 0) {
        fprintf(stderr, "test-docscan: symlink: %s\n", strerror(errno));
        exit(2);
    }

    struct run_result r1 = run_docscan(linkp);
    int ok1 = r1.exit_code != 0 && r1.out_len == 0 &&
              r1.err_len > 0 && stderr_is_at_most_one_short_line(r1.err, r1.err_len);
    record(ok1, "symlink target rejected (O_NOFOLLOW)", ok1 ? NULL : r1.err);
    run_result_free(&r1);

    struct run_result r2 = run_docscan(workdir);
    int ok2 = r2.exit_code != 0 && r2.out_len == 0 &&
              r2.err_len > 0 && stderr_is_at_most_one_short_line(r2.err, r2.err_len);
    record(ok2, "directory target rejected", ok2 ? NULL : r2.err);
    run_result_free(&r2);

    if (mkfifo(fifop, 0600) == 0) {
        struct run_result r3 = run_docscan(fifop);
        int ok3 = r3.exit_code != 0 && r3.out_len == 0 &&
                  r3.err_len > 0 && stderr_is_at_most_one_short_line(r3.err, r3.err_len);
        record(ok3, "FIFO target rejected without blocking", ok3 ? NULL : r3.err);
        run_result_free(&r3);
    }
}

static void test_sigpipe_quiet_exit(void)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "pipe-input.txt");
    size_t sz = 512u * 1024u;
    unsigned char *buf = malloc(sz);
    if (!buf) exit(2);
    memset(buf, 'x', sz);
    write_file_bytes(p, buf, sz);
    free(buf);

    int out_p[2], err_p[2];
    if (pipe2(out_p, O_CLOEXEC) != 0 || pipe2(err_p, O_CLOEXEC) != 0) exit(2);

    pid_t pid = fork();
    if (pid < 0) exit(2);
    if (pid == 0) {
        (void)dup2(out_p[1], STDOUT_FILENO);
        (void)dup2(err_p[1], STDERR_FILENO);
        close(out_p[0]);
        close(out_p[1]);
        close(err_p[0]);
        close(err_p[1]);
        execl(docscan_bin, docscan_bin, p, (char *)NULL);
        _exit(127);
    }

    /* Immediately close read end of stdout so child hits EPIPE on write. */
    close(out_p[1]);
    close(out_p[0]);
    close(err_p[1]);

    char ebuf[512];
    size_t elen = 0;
    for (;;) {
        ssize_t n = read(err_p[0], ebuf + elen, sizeof ebuf - 1 - elen);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        elen += (size_t)n;
        if (elen >= sizeof ebuf - 1) break;
    }
    ebuf[elen] = '\0';
    close(err_p[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && elen == 0;
    char msg[128];
    snprintf(msg, sizeof msg, "exited=%d code=%d sig=%d err=%.64s",
             WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1,
             WIFSIGNALED(st) ? WTERMSIG(st) : 0, ebuf);
    record(ok, "closed stdout exits quietly (SIGPIPE ignored)", ok ? NULL : msg);
}

static void test_closes_inherited_fds(void)
{
    /* Use a 512 KiB file so child blocks writing to the undrained stdout pipe
     * (>64 KiB pipe buffer). Before draining stdout, leak_p[0] must reach EOF,
     * proving docscan actively closed the inherited fd >= 3 while still running. */
    char p[PATH_MAX];
    join_path(p, sizeof p, "fdcheck.txt");
    size_t sz = 512u * 1024u;
    unsigned char *buf = malloc(sz);
    if (!buf) exit(2);
    memset(buf, 'z', sz);
    write_file_bytes(p, buf, sz);
    free(buf);

    int out_p[2], err_p[2], leak_p[2];
    if (pipe2(out_p, O_CLOEXEC) != 0 || pipe2(err_p, O_CLOEXEC) != 0 || pipe2(leak_p, 0) != 0) exit(2);

    pid_t pid = fork();
    if (pid < 0) exit(2);
    if (pid == 0) {
        (void)dup2(out_p[1], STDOUT_FILENO);
        (void)dup2(err_p[1], STDERR_FILENO);
        close(out_p[0]);
        close(out_p[1]);
        close(err_p[0]);
        close(err_p[1]);
        close(leak_p[0]);
        /* Keep leak_p[1] open and inheritable across exec. */
        int fl = fcntl(leak_p[1], F_GETFD);
        if (fl >= 0) (void)fcntl(leak_p[1], F_SETFD, fl & ~FD_CLOEXEC);
        execl(docscan_bin, docscan_bin, p, (char *)NULL);
        _exit(127);
    }

    close(out_p[1]);
    close(err_p[1]);
    close(leak_p[1]);

    struct pollfd pfd = { .fd = leak_p[0], .events = POLLIN };
    int pr = poll(&pfd, 1, 5000);
    char ch = 0;
    ssize_t lr = (pr > 0) ? read(leak_p[0], &ch, 1) : -1;
    close(leak_p[0]);

    /* Drain stdout and stderr so the child can finish cleanly. */
    size_t got = 0;
    unsigned char tmp[8192];
    for (;;) {
        ssize_t n = read(out_p[0], tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(out_p[0]);
    ssize_t er = read(err_p[0], tmp, sizeof tmp);
    close(err_p[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int ok = pr > 0 && lr == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && got == sz && er == 0;
    char msg[128];
    snprintf(msg, sizeof msg, "poll=%d leak_read=%zd got=%zu exit=%d",
             pr, lr, got, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    record(ok, "closes inherited fds >= 3 before reading", ok ? NULL : msg);
}

static void test_root_privilege_drop(void)
{
    if (geteuid() != 0) return;

    char rootdir[] = "/tmp/docscan-root-XXXXXX";
    if (!mkdtemp(rootdir)) return;
    if (chmod(rootdir, 0755) != 0) {
        (void)nftw(rootdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        return;
    }

    char owned[PATH_MAX], denied[PATH_MAX];
    snprintf(owned, sizeof owned, "%s/owned-by-user.txt", rootdir);
    snprintf(denied, sizeof denied, "%s/denied-to-user.txt", rootdir);
    write_file_bytes(owned, "owner readable\n", 15);
    write_file_bytes(denied, "mode zero secret\n", 17);

    uid_t target_uid = 1000;
    gid_t target_gid = 1000;
    if (chown(owned, target_uid, target_gid) != 0) {
        target_uid = 65534;
        target_gid = 65534;
        if (chown(owned, target_uid, target_gid) != 0) {
            (void)nftw(rootdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
            return;
        }
    }
    if (chmod(owned, 0400) != 0 || chown(denied, target_uid, target_gid) != 0 || chmod(denied, 0000) != 0) {
        (void)nftw(rootdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        return;
    }

    struct run_result r1 = run_docscan(owned);
    int ok1 = r1.exit_code == 0 && r1.err_len == 0 &&
              r1.out_len == 15 && memcmp(r1.out, "owner readable\n", 15) == 0;
    record(ok1, "root drops to file owner and reads 0400 file", ok1 ? NULL : r1.err);
    run_result_free(&r1);

    /* Root with CAP_DAC_OVERRIDE would read mode 0000, whereas the dropped uid cannot. */
    struct run_result r2 = run_docscan(denied);
    int ok2 = r2.exit_code != 0 && r2.out_len == 0 &&
              r2.err_len > 0 && stderr_is_at_most_one_short_line(r2.err, r2.err_len);
    record(ok2, "root drop loses DAC_OVERRIDE on mode 0000 file", ok2 ? NULL : r2.err);
    run_result_free(&r2);

    (void)nftw(rootdir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_docx(const char *external_path)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "sample.docx");
    const char *doc_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:r><w:t>Hel</w:t></w:r><w:r><w:t>lo &amp; &lt;world&gt; &#65;&#x42;</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>   spaced\t\nout   &quot;quoted&quot; &apos;ok&apos;  </w:t></w:r></w:p>"
        "</w:body></w:document>";
    const struct zip_entry_spec entries[] = {
        { "[Content_Types].xml", "<Types/>", 8 },
        { "word/document.xml", doc_xml, strlen(doc_xml) },
    };
    make_zip_fixture(p, entries, 2);

    struct run_result r = run_docscan(p);
    const char *want = "Hello & <world> AB spaced out \"quoted\" 'ok'";
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(want) && memcmp(r.out, want, r.out_len) == 0;
    char msg[256];
    snprintf(msg, sizeof msg, "exit=%d got='%.*s' err='%.80s'",
             r.exit_code, (int)(r.out_len < 120 ? r.out_len : 120), (const char *)r.out, r.err ? r.err : "");
    record(ok, "docx text, entities and whitespace", ok ? NULL : msg);
    run_result_free(&r);

    /* Corrupt / missing document.xml / oversized member checks */
    char bad[PATH_MAX];
    join_path(bad, sizeof bad, "missing-member.docx");
    const struct zip_entry_spec bad_entries[] = {
        { "other.xml", "<root>ignore</root>", 19 },
    };
    make_zip_fixture(bad, bad_entries, 1);
    struct run_result rb = run_docscan(bad);
    int okb = rb.exit_code != 0 && rb.out_len == 0 &&
              rb.err_len > 0 && stderr_is_at_most_one_short_line(rb.err, rb.err_len);
    record(okb, "docx missing word/document.xml rejected", okb ? NULL : rb.err);
    run_result_free(&rb);

    char bomb[PATH_MAX];
    join_path(bomb, sizeof bomb, "bomb.docx");
    make_oversized_member_docx(bomb, 17u * 1024u * 1024u);
    struct run_result rbm = run_docscan(bomb);
    int okbm = rbm.exit_code != 0 && rbm.out_len == 0 &&
               rbm.err_len > 0 && stderr_is_at_most_one_short_line(rbm.err, rbm.err_len);
    record(okbm, "docx member over 16 MiB rejected", okbm ? NULL : rbm.err);
    run_result_free(&rbm);

    if (external_path) {
        struct run_result re = run_docscan(external_path);
        int oke = re.exit_code == 0 && re.err_len == 0 && re.out_len > 0 && valid_utf8(re.out, re.out_len);
        record(oke, "external docx fixture", oke ? NULL : re.err);
        run_result_free(&re);
    }
}

static void test_odt(const char *external_path)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "sample.odt");
    const char *mt = "application/vnd.oasis.opendocument.text";
    const char *content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\">"
        "<office:body><office:text>"
        "<text:h text:outline-level=\"1\">Title&#x20;One</text:h>"
        "<text:p>Blue<text:span>jay</text:span><text:s/>and<!-- hidden comment --> <![CDATA[raw <tag>]]></text:p>"
        "</office:text></office:body></office:document-content>";
    const struct zip_entry_spec entries[] = {
        { "mimetype", mt, strlen(mt) },
        { "content.xml", content, strlen(content) },
    };
    make_zip_fixture(p, entries, 2);

    struct run_result r = run_docscan(p);
    const char *want = "Title One Bluejay and raw <tag>";
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(want) && memcmp(r.out, want, r.out_len) == 0;
    char msg[256];
    snprintf(msg, sizeof msg, "exit=%d got='%.*s' err='%.80s'",
             r.exit_code, (int)(r.out_len < 120 ? r.out_len : 120), (const char *)r.out, r.err ? r.err : "");
    record(ok, "odt content.xml tags, comments and CDATA", ok ? NULL : msg);
    run_result_free(&r);

    if (external_path) {
        struct run_result re = run_docscan(external_path);
        int oke = re.exit_code == 0 && re.err_len == 0 && re.out_len > 0 && valid_utf8(re.out, re.out_len);
        record(oke, "external odt fixture", oke ? NULL : re.err);
        run_result_free(&re);
    }
}

static void test_epub(const char *external_path)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "sample.epub");
    const char *mt = "application/epub+zip";
    const char *ch1 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
        "<head><title>Book</title><style>p { color: red; }</style><script src=\"a.js\"/></head>"
        "<body><h1>Chapter 1</h1><script>var hidden_js = 1;</script><p>Zebra &amp; okapi</p></body></html>";
    const char *ch2 =
        "<!DOCTYPE html><html><body><p>Second chapter \xE4\xB8\xAD\xE6\x96\x87</p></body></html>";
    const struct zip_entry_spec entries[] = {
        { "mimetype", mt, strlen(mt) },
        { "OEBPS/ch1.xhtml", ch1, strlen(ch1) },
        { "OEBPS/ch2.html", ch2, strlen(ch2) },
    };
    make_zip_fixture(p, entries, 3);

    struct run_result r = run_docscan(p);
    const char *want = "Book Chapter 1 Zebra & okapi Second chapter \xE4\xB8\xAD\xE6\x96\x87";
    int ok = r.exit_code == 0 && r.err_len == 0 &&
             r.out_len == strlen(want) && memcmp(r.out, want, r.out_len) == 0 &&
             strstr((const char *)r.out, "hidden_js") == NULL &&
             strstr((const char *)r.out, "color: red") == NULL;
    char msg[256];
    snprintf(msg, sizeof msg, "exit=%d got='%.*s' err='%.80s'",
             r.exit_code, (int)(r.out_len < 120 ? r.out_len : 120), (const char *)r.out, r.err ? r.err : "");
    record(ok, "epub multi-chapter xhtml/html skipping style and script", ok ? NULL : msg);
    run_result_free(&r);

    if (external_path) {
        struct run_result re = run_docscan(external_path);
        int oke = re.exit_code == 0 && re.err_len == 0 && re.out_len > 0 && valid_utf8(re.out, re.out_len);
        record(oke, "external epub fixture", oke ? NULL : re.err);
        run_result_free(&re);
    }
}

static void test_pdf(int no_pdf, const char *external_path)
{
    char p[PATH_MAX];
    join_path(p, sizeof p, "sample.pdf");
    make_pdf_fixture(p);

    struct run_result r = run_docscan(p);
    if (no_pdf) {
        int ok = r.exit_code != 0 && r.out_len == 0 &&
                 r.err_len > 0 && stderr_is_at_most_one_short_line(r.err, r.err_len);
        record(ok, "pdf reports unsupported cleanly when built without libmupdf", ok ? NULL : r.err);
        run_result_free(&r);
        return;
    }

    int ok = r.exit_code == 0 && r.err_len == 0 &&
             valid_utf8(r.out, r.out_len) &&
             strstr((const char *)r.out, "Hello PDF quokka page one") != NULL &&
             strstr((const char *)r.out, "Second page pangolin marker") != NULL;
    char msg[256];
    snprintf(msg, sizeof msg, "exit=%d out='%.*s' err='%.80s'",
             r.exit_code, (int)(r.out_len < 120 ? r.out_len : 120), (const char *)r.out, r.err ? r.err : "");
    record(ok, "pdf 2-page text extraction via libmupdf", ok ? NULL : msg);
    run_result_free(&r);

    /* Corrupt PDF must exit non-zero with at most one short stderr line. */
    char bad[PATH_MAX];
    join_path(bad, sizeof bad, "broken.pdf");
    write_file_bytes(bad, "%PDF-1.4\nnot a valid pdf body\n", 30);
    struct run_result rb = run_docscan(bad);
    int okb = rb.exit_code != 0 && rb.out_len == 0 &&
              rb.err_len > 0 && stderr_is_at_most_one_short_line(rb.err, rb.err_len);
    record(okb, "corrupt pdf fails cleanly with one stderr line", okb ? NULL : rb.err);
    run_result_free(&rb);

    if (external_path) {
        struct run_result re = run_docscan(external_path);
        int oke = re.exit_code == 0 && re.err_len == 0 && re.out_len > 0 && valid_utf8(re.out, re.out_len);
        record(oke, "external pdf fixture", oke ? NULL : re.err);
        run_result_free(&re);
    }
}

int main(int argc, char **argv)
{
    int no_pdf = 0;
    const char *ext_pdf = NULL, *ext_docx = NULL, *ext_odt = NULL, *ext_epub = NULL;

    if (argc < 2) {
        fprintf(stderr, "usage: test-docscan DOCSCAN_BIN [--no-pdf] [--fixture-pdf P] [--fixture-docx P] [--fixture-odt P] [--fixture-epub P]\n");
        return 2;
    }
    docscan_bin = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-pdf") == 0) {
            no_pdf = 1;
        } else if (strcmp(argv[i], "--fixture-pdf") == 0 && i + 1 < argc) {
            ext_pdf = argv[++i];
        } else if (strcmp(argv[i], "--fixture-docx") == 0 && i + 1 < argc) {
            ext_docx = argv[++i];
        } else if (strcmp(argv[i], "--fixture-odt") == 0 && i + 1 < argc) {
            ext_odt = argv[++i];
        } else if (strcmp(argv[i], "--fixture-epub") == 0 && i + 1 < argc) {
            ext_epub = argv[++i];
        } else {
            fprintf(stderr, "test-docscan: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    make_workdir();
    atexit(cleanup_workdir);

    test_usage();
    test_plain_text_and_bom();
    test_text_extension_with_pdf_string();
    test_invalid_utf8_replacement();
    test_sniff_text_no_extension();
    test_binary_rejected();
    test_output_cap_1mib_utf8_boundary();
    test_oversized_8mib_rejected();
    test_symlink_and_dir_rejected();
    test_sigpipe_quiet_exit();
    test_closes_inherited_fds();
    test_root_privilege_drop();
    test_docx(ext_docx);
    test_odt(ext_odt);
    test_epub(ext_epub);
    test_pdf(no_pdf, ext_pdf);

    if (failures == 0) {
        printf("ALL PASSED (%d tests)\n", passed);
        return 0;
    }
    printf("FAILED (%d failed, %d passed)\n", failures, passed);
    return 1;
}
