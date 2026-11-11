/*
 * extdisplay's end of the broker's control socket (tools/EXTDISPLAY.md): a SOCK_SEQPACKET
 * channel of ASCII lines, one per message, at most EXTDISPLAY_SETUP_LINE bytes.
 *
 *   extdisplay -> broker   scan | pair NAME | cancel | release
 *   broker -> extdisplay   iface NAME DRIVER CARRIER ... end      (after scan)
 *                   lease NAME LINKLOCAL PORT, code GROUPS (after pair)
 *                   error TEXT
 *                   paired      with exactly one SCM_RIGHTS stream
 *                   ended REASON
 *
 * Everything the broker sends is checked; anything else is a broken channel.
 * The pairing code and address stay in this structure only and are wiped
 * when they are no longer shown: never logged, never in argv or environment.
 */
#ifndef LOCACHY_EXTDISPLAY_SETUP_H
#define LOCACHY_EXTDISPLAY_SETUP_H
#include <sys/types.h>

#define EXTDISPLAY_SETUP_LINE 512
#define EXTDISPLAY_SETUP_IFACES 16
#define EXTDISPLAY_SETUP_NAME 16   /* an interface name with its NUL */

enum extdisplay_setup_state {
    EXTDISPLAY_SETUP_IDLE,       /* nothing asked */
    EXTDISPLAY_SETUP_SCANNING,   /* scan sent, iface lines coming */
    EXTDISPLAY_SETUP_LIST,       /* candidates known */
    EXTDISPLAY_SETUP_PAIRING,    /* pair sent, lease and code coming */
    EXTDISPLAY_SETUP_WAITING,    /* code shown, waiting for the sender */
    EXTDISPLAY_SETUP_PAIRED,     /* the stream arrived */
    EXTDISPLAY_SETUP_CANCELLING, /* cancel sent */
    EXTDISPLAY_SETUP_RELEASING,  /* release sent */
    EXTDISPLAY_SETUP_RELEASED,   /* the broker has gone */
    EXTDISPLAY_SETUP_BROKEN,     /* protocol error or EOF */
};

enum extdisplay_setup_event {
    EXTDISPLAY_SETUP_EV_NONE,    /* nothing (yet), or a line that changes nothing shown */
    EXTDISPLAY_SETUP_EV_LIST,    /* the scan is complete */
    EXTDISPLAY_SETUP_EV_CODE,    /* lease and code are there to show */
    EXTDISPLAY_SETUP_EV_ERROR,   /* the pair was refused: error[] */
    EXTDISPLAY_SETUP_EV_PAIRED,  /* stream is the plaintext stream, now the caller's */
    EXTDISPLAY_SETUP_EV_ENDED,   /* reason[] */
    EXTDISPLAY_SETUP_EV_BROKEN,  /* the channel is unusable */
};

struct extdisplay_iface {
    char name[EXTDISPLAY_SETUP_NAME], driver[33];
    int carrier; /* 1 up, 0 down */
};

struct extdisplay_setup {
    int fd;
    enum extdisplay_setup_state state;
    struct extdisplay_iface ifaces[EXTDISPLAY_SETUP_IFACES];
    int count, scanned;
    char pairing[EXTDISPLAY_SETUP_NAME];  /* the interface asked for */
    char linklocal[65], port[8], code[65]; /* shown while waiting; wiped after */
    char reason[24], error[201];
    int stream;                    /* EXTDISPLAY_SETUP_EV_PAIRED: its descriptor */
};

void extdisplay_setup_init(struct extdisplay_setup *s, int fd);
/* 0, or -1 when not allowed in this state or not sent. */
int extdisplay_setup_scan(struct extdisplay_setup *s);
int extdisplay_setup_pair(struct extdisplay_setup *s, const char *name);
int extdisplay_setup_cancel(struct extdisplay_setup *s);
int extdisplay_setup_release(struct extdisplay_setup *s);
/* One message from the broker, if there is one (non-blocking). */
enum extdisplay_setup_event extdisplay_setup_read(struct extdisplay_setup *s);
/* The same for a line already received, with the descriptors it carried
 * (owned by the call: kept as the stream or closed). For tests. */
enum extdisplay_setup_event extdisplay_setup_line(struct extdisplay_setup *s, const char *line, const int *fds, int nfds);
/* Wipes the code and the address. */
void extdisplay_setup_forget(struct extdisplay_setup *s);
/* "carrier lost" and the like, for the status line; NULL if unknown. */
const char *extdisplay_setup_reason_text(const char *reason);

/* The broker is a child of extdisplay: "extdisplay-broker --control-fd 3", with stdio on
 * /dev/null and nothing else open (the session owner's descriptor included).
 * Returns its pid and the caller's end of the control channel, or -1. */
#ifdef EXTDISPLAY_BROKER_TEST_PATH
#define EXTDISPLAY_BROKER_PATH EXTDISPLAY_BROKER_TEST_PATH /* tests only */
#else
#define EXTDISPLAY_BROKER_PATH "/usr/local/lib/locachy/extdisplay-broker"
#endif
#define EXTDISPLAY_BROKER_FD 3
pid_t extdisplay_setup_spawn_broker(const char *path, int *control);
/* For after the caller has closed its control end (the broker then releases
 * and exits): waits a bounded time, then SIGTERM, then SIGKILL. */
void extdisplay_setup_reap_broker(pid_t pid);
#endif
