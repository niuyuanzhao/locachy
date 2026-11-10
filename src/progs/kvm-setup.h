/*
 * kvm's end of the broker's control socket (tools/KVM.md): a SOCK_SEQPACKET
 * channel of ASCII lines, one per message, at most KVM_SETUP_LINE bytes.
 *
 *   kvm -> broker   scan | pair NAME | cancel | release
 *   broker -> kvm   iface NAME DRIVER CARRIER ... end      (after scan)
 *                   lease NAME LINKLOCAL PORT, code GROUPS (after pair)
 *                   error TEXT
 *                   paired      with exactly one SCM_RIGHTS stream
 *                   ended REASON
 *
 * Everything the broker sends is checked; anything else is a broken channel.
 * The pairing code and address stay in this structure only and are wiped
 * when they are no longer shown: never logged, never in argv or environment.
 */
#ifndef LOCACHY_KVM_SETUP_H
#define LOCACHY_KVM_SETUP_H
#include <sys/types.h>

#define KVM_SETUP_LINE 512
#define KVM_SETUP_IFACES 16
#define KVM_SETUP_NAME 16   /* an interface name with its NUL */

enum kvm_setup_state {
    KVM_SETUP_IDLE,       /* nothing asked */
    KVM_SETUP_SCANNING,   /* scan sent, iface lines coming */
    KVM_SETUP_LIST,       /* candidates known */
    KVM_SETUP_PAIRING,    /* pair sent, lease and code coming */
    KVM_SETUP_WAITING,    /* code shown, waiting for the sender */
    KVM_SETUP_PAIRED,     /* the stream arrived */
    KVM_SETUP_CANCELLING, /* cancel sent */
    KVM_SETUP_RELEASING,  /* release sent */
    KVM_SETUP_RELEASED,   /* the broker has gone */
    KVM_SETUP_BROKEN,     /* protocol error or EOF */
};

enum kvm_setup_event {
    KVM_SETUP_EV_NONE,    /* nothing (yet), or a line that changes nothing shown */
    KVM_SETUP_EV_LIST,    /* the scan is complete */
    KVM_SETUP_EV_CODE,    /* lease and code are there to show */
    KVM_SETUP_EV_ERROR,   /* the pair was refused: error[] */
    KVM_SETUP_EV_PAIRED,  /* stream is the plaintext stream, now the caller's */
    KVM_SETUP_EV_ENDED,   /* reason[] */
    KVM_SETUP_EV_BROKEN,  /* the channel is unusable */
};

struct kvm_iface {
    char name[KVM_SETUP_NAME], driver[33];
    int carrier; /* 1 up, 0 down */
};

struct kvm_setup {
    int fd;
    enum kvm_setup_state state;
    struct kvm_iface ifaces[KVM_SETUP_IFACES];
    int count, scanned;
    char pairing[KVM_SETUP_NAME];  /* the interface asked for */
    char linklocal[65], port[8], code[65]; /* shown while waiting; wiped after */
    char reason[24], error[201];
    int stream;                    /* KVM_SETUP_EV_PAIRED: its descriptor */
};

void kvm_setup_init(struct kvm_setup *s, int fd);
/* 0, or -1 when not allowed in this state or not sent. */
int kvm_setup_scan(struct kvm_setup *s);
int kvm_setup_pair(struct kvm_setup *s, const char *name);
int kvm_setup_cancel(struct kvm_setup *s);
int kvm_setup_release(struct kvm_setup *s);
/* One message from the broker, if there is one (non-blocking). */
enum kvm_setup_event kvm_setup_read(struct kvm_setup *s);
/* The same for a line already received, with the descriptors it carried
 * (owned by the call: kept as the stream or closed). For tests. */
enum kvm_setup_event kvm_setup_line(struct kvm_setup *s, const char *line, const int *fds, int nfds);
/* Wipes the code and the address. */
void kvm_setup_forget(struct kvm_setup *s);
/* "carrier lost" and the like, for the status line; NULL if unknown. */
const char *kvm_setup_reason_text(const char *reason);

/* The broker is a child of kvm: "kvm-broker --control-fd 3", with stdio on
 * /dev/null and nothing else open (the session owner's descriptor included).
 * Returns its pid and the caller's end of the control channel, or -1. */
#ifdef KVM_BROKER_TEST_PATH
#define KVM_BROKER_PATH KVM_BROKER_TEST_PATH /* tests only */
#else
#define KVM_BROKER_PATH "/usr/local/lib/locachy/kvm-broker"
#endif
#define KVM_BROKER_FD 3
pid_t kvm_setup_spawn_broker(const char *path, int *control);
/* For after the caller has closed its control end (the broker then releases
 * and exits): waits a bounded time, then SIGTERM, then SIGKILL. */
void kvm_setup_reap_broker(pid_t pid);
#endif
