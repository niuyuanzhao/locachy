/*
 * extdisplay's end of extdisplay-worker: local native-endian SOCK_SEQPACKET messages of one
 * fixed size, no pointers and no peer strings toward the frontend. Pictures
 * come as one sealed memfd per FRAME, mapped read-only after its size and
 * seals are checked. The frontend never passes descriptors to the worker.
 */
#ifndef LOCACHY_EXTDISPLAY_IPC_H
#define LOCACHY_EXTDISPLAY_IPC_H
#include <stdint.h>
#include <sys/types.h>

#ifdef EXTDISPLAY_WORKER_TEST_PATH
#define EXTDISPLAY_WORKER_PATH EXTDISPLAY_WORKER_TEST_PATH /* tests only; never chosen at run time */
#else
#define EXTDISPLAY_WORKER_PATH "/usr/local/lib/locachy/extdisplay-worker"
#endif
#define EXTDISPLAY_WORKER_USER 65534 /* nobody: no home, no groups, no devices */
#define EXTDISPLAY_IPC_MAGIC 0x4c4b5631u
#define EXTDISPLAY_IPC_TEXT 1024u
#define EXTDISPLAY_IPC_FD 3        /* the worker's: this channel */
#define EXTDISPLAY_TRANSPORT_FD 4  /* the worker's: the paired stream */
#define EXTDISPLAY_CANCEL_WAIT_MS 150 /* session.h's owner waits 250 ms for the ACK */

enum extdisplay_ipc_op {
    /* frontend to worker */
    EXTDISPLAY_IPC_HELLO = 1,  /* text: the hello JSON */
    EXTDISPLAY_IPC_INPUT,      /* text: one input JSON; dropped unless the stream is active */
    EXTDISPLAY_IPC_STOP,       /* cancel to the peer, then exit */
    EXTDISPLAY_IPC_FRAME_ACK,  /* the last FRAME is mapped: the next may come */
    EXTDISPLAY_IPC_CANCEL,     /* cancel to the peer now, answered by CANCELLED once written */
    /* worker to frontend */
    EXTDISPLAY_IPC_READY = 16, /* width: decoders, 1 H.264, 2 HEVC */
    EXTDISPLAY_IPC_ACTIVE,     /* streamConfig accepted, decoder open */
    EXTDISPLAY_IPC_FRAME,      /* width, height, bytes; one memfd */
    EXTDISPLAY_IPC_UNAVAILABLE,
    EXTDISPLAY_IPC_FAILED,
    EXTDISPLAY_IPC_CANCELLED,  /* the cancel is in the transport */
};
struct extdisplay_ipc {
    uint32_t magic, op, width, height;
    uint64_t bytes;
    uint32_t length, reserved;
    char text[EXTDISPLAY_IPC_TEXT];
};
/* transport: the broker's paired AF_UNIX stream (never a raw TCP socket). */
pid_t extdisplay_worker_spawn(int transport, int *ipc);
int extdisplay_ipc_send(int fd, struct extdisplay_ipc *message);
/* 0 and *pixels_fd (-1 unless FRAME), or -1 (errno EAGAIN: nothing there). */
int extdisplay_ipc_recv(int fd, struct extdisplay_ipc *message, int *pixels_fd);
void *extdisplay_frame_map(int fd, const struct extdisplay_ipc *message);
#endif
