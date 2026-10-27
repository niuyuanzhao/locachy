/* Real renderer bbox parity: exact white, raster scale and blank pages.
 * No physical display or private document is accessed. Run as root in the
 * disposable Linux root, with the renderer path compiled into view-client. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../progs/view-proto.h"

static void pdf(const char *path, int blank)
{
    FILE *f = fopen(path, "wx");
    assert(f);
    long offsets[5];
    fputs("%PDF-1.4\n", f);
    offsets[1] = ftell(f); fputs("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n", f);
    offsets[2] = ftell(f); fputs("2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n", f);
    offsets[3] = ftell(f); fputs("3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 2048 2048] /Contents 4 0 R >> endobj\n", f);
    const char *paint = blank ? "" : "0.9960784314 0.9960784314 0.9960784314 rg 400 600 800 700 re f\n";
    offsets[4] = ftell(f); fprintf(f, "4 0 obj << /Length %zu >> stream\n%sendstream\nendobj\n", strlen(paint), paint);
    long xref = ftell(f);
    fputs("xref\n0 5\n0000000000 65535 f \n", f);
    for (int i = 1; i <= 4; i++) fprintf(f, "%010ld 00000 n \n", offsets[i]);
    fprintf(f, "trailer << /Root 1 0 R /Size 5 >>\nstartxref\n%ld\n%%%%EOF\n", xref);
    assert(fclose(f) == 0);
}

static struct view_reply request(int sock, struct view_request *q, struct view_rect *box)
{
    struct view_reply r;
    int fd = -1;
    assert(view_call(sock, q, &r, &fd) == 0);
    if (fd >= 0) {
        void *p = view_map(fd, r.size);
        assert(p);
        if (box) { assert(r.size == sizeof *box); memcpy(box, p, sizeof *box); }
        munmap(p, r.size);
        close(fd);
    }
    return r;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && geteuid() == 0);
    assert(mkdir(argv[1], 0755) == 0);
    char marked[VIEW_TEXT], blank[VIEW_TEXT];
    assert(snprintf(marked, sizeof marked, "%s/nearwhite.pdf", argv[1]) < (int)sizeof marked);
    assert(snprintf(blank, sizeof blank, "%s/blank.pdf", argv[1]) < (int)sizeof blank);
    pdf(marked, 0); pdf(blank, 1);
    int sock;
    pid_t pid = view_spawn(&sock);
    assert(pid > 0);
    struct view_request q = { .op = VIEW_OPEN };
    snprintf(q.text, sizeof q.text, "%s", marked);
    assert(request(sock, &q, NULL).status == 0);
    struct view_rect a, b;
    q = (struct view_request){ .op = VIEW_BBOX, .zoom = 1 };
    struct view_reply r = request(sock, &q, &a);
    assert(r.status == 0 && r.count == 1);
    q.zoom = 2;
    r = request(sock, &q, &b);
    assert(r.status == 0 && r.count == 1);
    assert(fabs(a.x0 - 400.0 / 2048) < .001 && fabs(a.y0 - 748.0 / 2048) < .001);
    assert(fabs(a.x1 - 1199.0 / 2048) < .001 && fabs(a.y1 - 1447.0 / 2048) < .001);
    assert(b.x1 > a.x1 && b.y1 > a.y1); /* inclusive raster edge reflects requested resolution */
    q = (struct view_request){ .op = VIEW_OPEN };
    snprintf(q.text, sizeof q.text, "%s", blank);
    assert(request(sock, &q, NULL).status == 0);
    q = (struct view_request){ .op = VIEW_BBOX, .zoom = 1 };
    r = request(sock, &q, NULL);
    assert(r.status == 0 && r.count == 0);
    close(sock);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS real renderer bbox: near-white content, full requested raster scale, inclusive normalized edges, blank pages");
}
