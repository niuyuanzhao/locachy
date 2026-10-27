/*
 * Loading kernel modules, for process 1: the drivers for the devices there
 * are, as udev and modprobe would. The kernel is a distribution's, with its
 * drivers as modules under ROOT/lib/modules/VERSION:
 *   modules.alias   "alias PATTERN NAME": a device whose modalias matches
 *                   PATTERN (shell wildcards) wants module NAME
 *   modules.dep     "PATH: DEP...": a module's file and every module it needs
 * At start, every device under /sys/devices with a modalias gets its modules
 * (coldplug); after that, devices that appear are announced on a netlink
 * socket (uevents), and get theirs the same way. A module is loaded with
 * finit_module; the kernel decompresses .ko.zst itself. Firmware is the
 * kernel's own business (firmware_class.path on its command line).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "internal.h"

#define MODULE_INIT_COMPRESSED_FILE 4

struct alias {
    char *pattern, *name;
};

struct module {
    char *name;   /* with '-' turned into '_' */
    char *path;   /* relative to the module directory */
    char **deps;  /* names */
    int ndeps;
    int state;    /* 0 not tried, 1 loading, 2 loaded (or built in), 3 failed */
};

static char dir[256];
static struct alias *aliases;
static int naliases;
static struct module *modules;
static int nmodules;
static int uevents = -1;
static int loaded, unused, failed;

static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;

    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    ssize_t n = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static void normalise(char *s)
{
    for (; *s; s++) {
        if (*s == '-') {
            *s = '_';
        }
    }
}

/* The module's name from its path: "kernel/a/b/foo-bar.ko.zst" -> "foo_bar". */
static char *name_of(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strcspn(base, ".");
    char *name = strndup(base, len);
    normalise(name);
    return name;
}

static struct module *find(const char *name)
{
    for (int i = 0; i < nmodules; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

static int load_tables(void)
{
    char path[320];

    snprintf(path, sizeof path, "%s/modules.dep", dir);
    char *dep = read_file(path);
    snprintf(path, sizeof path, "%s/modules.alias", dir);
    char *alias = read_file(path);
    if (dep == NULL || alias == NULL) {
        say("modules: cannot read %s/modules.dep or modules.alias", dir);
        return -1;
    }
    for (char *line = strtok(dep, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        modules = realloc(modules, sizeof *modules * (size_t)(nmodules + 1));
        struct module *m = &modules[nmodules++];
        *m = (struct module){ .path = line, .name = name_of(line) };
        for (char *d = colon + 1; (d = d + strspn(d, " ")) && *d;) {
            size_t len = strcspn(d, " ");
            char *one = strndup(d, len);
            m->deps = realloc(m->deps, sizeof *m->deps * (size_t)(m->ndeps + 1));
            m->deps[m->ndeps++] = name_of(one);
            free(one);
            d += len;
        }
    }
    for (char *line = strtok(alias, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char *pattern = NULL, *name = NULL;
        if (strncmp(line, "alias ", 6) != 0 || (pattern = strtok_r(line + 6, " ", &name)) == NULL ||
            name == NULL || *name == '\0') {
            continue;
        }
        normalise(name);
        aliases = realloc(aliases, sizeof *aliases * (size_t)(naliases + 1));
        aliases[naliases++] = (struct alias){ pattern, name };
    }
    return 0;
}

/* Load a module and, first, the modules it needs. */
static int load(struct module *m)
{
    if (m->state == 2 || m->state == 1) {
        return 0;
    }
    if (m->state == 3) {
        return -1;
    }
    m->state = 1;
    for (int i = m->ndeps - 1; i >= 0; i--) {
        struct module *d = find(m->deps[i]);
        if (d != NULL && load(d) < 0) {
            m->state = 3;
            return -1;
        }
    }
    char path[512];
    double t0 = uptime();
    snprintf(path, sizeof path, "%s/%s", dir, m->path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int flags = strstr(m->path, ".ko.") ? MODULE_INIT_COMPRESSED_FILE : 0;
    if (fd < 0 || (syscall(SYS_finit_module, fd, "", flags) < 0 && errno != EEXIST)) {
        int err = errno;
        if (fd >= 0) {
            close(fd);
        }
        m->state = 3;
        if (err == ENODEV) {
            unused++; /* matched, but the hardware it wants is not here */
        } else {
            say("modules: %s: %s", m->name, strerror(err));
            failed++;
        }
        return -1;
    }
    close(fd);
    m->state = 2;
    loaded++;
    double ms = (uptime() - t0) * 1000;
    if (ms >= 20) {
        say("modules: %s took %.0f ms", m->name, ms); /* to see where boot time goes */
    }
    return 0;
}

/* Load every module whose alias matches this modalias. */
static void load_for(const char *modalias)
{
    size_t prefix = strcspn(modalias, ":");

    for (int i = 0; i < naliases; i++) {
        const char *p = aliases[i].pattern;
        if (strncmp(p, modalias, prefix) != 0 || fnmatch(p, modalias, 0) != 0) {
            continue;
        }
        struct module *m = find(aliases[i].name);
        if (m != NULL) {
            load(m);
        }
    }
}

/* Every modalias file under path. */
static void coldplug(const char *path, int depth)
{
    DIR *d = opendir(path);
    char sub[1024];

    if (d == NULL || depth > 24) {
        if (d != NULL) {
            closedir(d);
        }
        return;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR) {
            coldplug(sub, depth + 1);
        } else if (e->d_type == DT_REG && strcmp(e->d_name, "modalias") == 0) {
            char *alias = read_file(sub);
            if (alias != NULL) {
                alias[strcspn(alias, "\n")] = '\0';
                if (alias[0]) {
                    load_for(alias);
                }
                free(alias);
            }
        }
    }
    closedir(d);
}

int modules_event(void)
{
    char buf[8192];
    ssize_t n;
    int changed = 0;

    while ((n = recv(uevents, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        /* Processed udev events wake init to retry input after rule tagging.
         * Their binary header is not a second kernel DRM/module event. */
        if (n >= 8 && memcmp(buf, "libudev", 8) == 0) continue;
        char *end = buf + n;
        char *next = memchr(buf, '\0', (size_t)n);
        if (next == NULL) {
            continue;
        }
        int add = next - buf >= 4 && memcmp(buf, "add@", 4) == 0;
        int drm = 0, action = 0, hotplug = 0, lease = 0, synthetic = 0;
        for (char *k = next + 1; k < end; k = next + 1) {
            next = memchr(k, '\0', (size_t)(end - k));
            if (next == NULL) {
                break; /* Ignore an incomplete trailing field. */
            }
            if (strcmp(k, "SUBSYSTEM=drm") == 0) {
                drm = 1;
            } else if (strcmp(k, "ACTION=add") == 0 || strcmp(k, "ACTION=change") == 0 ||
                       strcmp(k, "ACTION=remove") == 0) {
                action = 1;
            } else if (strcmp(k, "HOTPLUG=1") == 0) {
                hotplug = 1;
            } else if (strcmp(k, "LEASE=1") == 0) {
                lease = 1;
            } else if (strncmp(k, "SYNTH_UUID=", 11) == 0) {
                synthetic = 1;
            } else if (add && next - k >= 9 && memcmp(k, "MODALIAS=", 9) == 0) {
                load_for(k + 9);
            }
        }
        /* A lease made or ended (the screen lent, taken back) is no change
         * of displays: counting it would refresh, lend again, and loop. Nor
         * is a replay (udevadm trigger at boot): it takes the screen back
         * from home while home is starting. Its modules are still loaded. */
        changed |= drm && (action || hotplug) && !lease && !synthetic;
    }
    return changed;
}

int modules_fd(void)
{
    return uevents;
}

int modules_start(const char *root)
{
    struct utsname u;
    /* Kernel events load drivers; udev's processed events make newly tagged
     * devices available to libinput in an already-running framebuffer front. */
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 | 2 };

    uname(&u);
    snprintf(dir, sizeof dir, "%s/lib/modules/%s", root, u.release);
    if (load_tables() < 0) {
        return -1;
    }
    /* Listen first, so that nothing that appears while we walk is missed. */
    uevents = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (uevents >= 0 && bind(uevents, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(uevents);
        uevents = -1;
    }
    double t0 = uptime();
    coldplug("/sys/devices", 0);
    double t1 = uptime();
    /* Devices the new drivers made (USB devices, sound codecs, ...): until
     * nothing new has come for half a second. */
    struct pollfd p = { .fd = uevents, .events = POLLIN };
    while (uevents >= 0 && poll(&p, 1, 500) > 0) {
        modules_event();
    }
    say("modules: coldplug %.0f ms, then new devices until quiet %.0f ms (the last 500 ms of it waiting)",
        (t1 - t0) * 1000, (uptime() - t1) * 1000);
    say("modules: %s: %d modules, %d aliases; loaded %d, %d failed (%d found no device)", u.release,
        nmodules, naliases, loaded, failed, unused);
    return 0;
}
