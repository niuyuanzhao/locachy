/*
 * kvm's end of kvm-worker: local native-endian SOCK_SEQPACKET messages of one
 * fixed size, no pointers and no peer strings toward the frontend. Pictures
 * come as one sealed memfd per FRAME, mapped read-only after its size and
 * seals are checked. The frontend never passes descriptors to the worker.
 */
#ifndef LOCACHY_KVM_IPC_H
#define LOCACHY_KVM_IPC_H
#include <stdint.h>
#include <sys/types.h>

#ifdef KVM_WORKER_TEST_PATH
#define KVM_WORKER_PATH KVM_WORKER_TEST_PATH /* tests only; never chosen at run time */
#else
#define KVM_WORKER_PATH "/usr/local/lib/locachy/kvm-worker"
#endif
#define KVM_WORKER_USER 65534 /* nobody: no home, no groups, no devices */
#define KVM_IPC_MAGIC 0x4c4b5631u
#define KVM_IPC_TEXT 1024u
#define KVM_IPC_FD 3        /* the worker's: this channel */
#define KVM_TRANSPORT_FD 4  /* the worker's: the paired stream */
#define KVM_CANCEL_WAIT_MS 150 /* session.h's owner waits 250 ms for the ACK */

enum kvm_ipc_op {
    /* frontend to worker */
    KVM_IPC_HELLO = 1,  /* text: the hello JSON */
    KVM_IPC_INPUT,      /* text: one input JSON; dropped unless the stream is active */
    KVM_IPC_STOP,       /* cancel to the peer, then exit */
    KVM_IPC_FRAME_ACK,  /* the last FRAME is mapped: the next may come */
    KVM_IPC_CANCEL,     /* cancel to the peer now, answered by CANCELLED once written */
    /* worker to frontend */
    KVM_IPC_READY = 16, /* width: decoders, 1 H.264, 2 HEVC */
    KVM_IPC_ACTIVE,     /* streamConfig accepted, decoder open */
    KVM_IPC_FRAME,      /* width, height, bytes; one memfd */
    KVM_IPC_UNAVAILABLE,
    KVM_IPC_FAILED,
    KVM_IPC_CANCELLED,  /* the cancel is in the transport */
};
struct kvm_ipc {
    uint32_t magic, op, width, height;
    uint64_t bytes;
    uint32_t length, reserved;
    char text[KVM_IPC_TEXT];
};
/* transport: the broker's paired AF_UNIX stream (never a raw TCP socket). */
pid_t kvm_worker_spawn(int transport, int *ipc);
int kvm_ipc_send(int fd, struct kvm_ipc *message);
/* 0 and *pixels_fd (-1 unless FRAME), or -1 (errno EAGAIN: nothing there). */
int kvm_ipc_recv(int fd, struct kvm_ipc *message, int *pixels_fd);
void *kvm_frame_map(int fd, const struct kvm_ipc *message);
#endif
