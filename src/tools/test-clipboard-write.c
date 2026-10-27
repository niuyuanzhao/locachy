/* Clipboard source writes: large data must not be truncated by an overall
 * deadline while the reader is making progress; a stuck reader expires. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main clipboard_main
#include "../progs/clipboard.c"
#undef main
#include <assert.h>

int main(void)
{
    size_t size = 3 * 1024 * 1024;
    int fd = memfd_create("clipboard-write-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
    unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(data != MAP_FAILED);
    for (size_t i = 0; i < size; i++) data[i] = (unsigned char)(i % 251);
    struct blob *b = calloc(1, sizeof *b);
    assert(b);
    *b = (struct blob){ .fd = fd, .size = size, .refs = 1 };
    int p[2];
    assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    transfer_ms = 20;
    struct sending *s = calloc(1, sizeof *s);
    assert(s);
    *s = (struct sending){ .fd = p[1], .blob = b, .until = now() + transfer_ms };
    sends = s;
    size_t received = 0;
    char buf[32768];
    int64_t start = now();
    while (received < size) {
        sweep();
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n > 0) {
            assert(memcmp(data + received, buf, (size_t)n) == 0);
            received += (size_t)n;
        } else assert(n < 0 && errno == EAGAIN);
        usleep(1000);
    }
    assert(now() - start > transfer_ms);
    sweep();
    assert(sends == NULL);
    close(p[0]); munmap(data, size);
    puts("PASS large transfer completes beyond the overall deadline while progress continues");

    assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    b = calloc(1, sizeof *b);
    assert(b);
    *b = (struct blob){ .fd = memfd_create("clipboard-stuck-test", MFD_CLOEXEC), .size = size, .refs = 1 };
    assert(b->fd >= 0 && ftruncate(b->fd, (off_t)size) == 0);
    s = calloc(1, sizeof *s);
    assert(s);
    *s = (struct sending){ .fd = p[1], .blob = b, .until = now() + transfer_ms };
    sends = s;
    sweep();
    usleep(50000);
    sweep();
    assert(sends == NULL);
    close(p[0]);
    puts("PASS a non-reading destination releases its transfer after no progress");
    puts("ALL PASSED");
    return 0;
}
