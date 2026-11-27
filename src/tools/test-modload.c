/* Unit test for modload.c: modprobe.d precedence, options, blacklist,
 * /proc/cmdline modprobe.blacklist=, softdep pre:/post:, modules.softdep,
 * and modules-load.d ordering before coldplug.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

struct load_rec {
    char name[64];
    char opts[256];
    int flags;
};

static struct load_rec recs[64];
static int nrecs;
static char fail_mod[64];
static int fail_errno;
static char fail_mod2[64];
static int fail_errno2;
static char eexist_mod[64];

static long test_finit_module(int fd, const char *opts, int flags)
{
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    assert(n > 0);
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    if (eexist_mod[0] && strcmp(buf, eexist_mod) == 0) {
        assert(nrecs < (int)(sizeof recs / sizeof recs[0]));
        snprintf(recs[nrecs].name, sizeof recs[nrecs].name, "%s", buf);
        snprintf(recs[nrecs].opts, sizeof recs[nrecs].opts, "%s", opts ? opts : "");
        recs[nrecs].flags = flags;
        nrecs++;
        errno = EEXIST;
        return -1;
    }
    if (fail_mod[0] && strcmp(buf, fail_mod) == 0) {
        errno = fail_errno;
        return -1;
    }
    if (fail_mod2[0] && strcmp(buf, fail_mod2) == 0) {
        errno = fail_errno2;
        return -1;
    }
    assert(nrecs < (int)(sizeof recs / sizeof recs[0]));
    snprintf(recs[nrecs].name, sizeof recs[nrecs].name, "%s", buf);
    snprintf(recs[nrecs].opts, sizeof recs[nrecs].opts, "%s", opts ? opts : "");
    recs[nrecs].flags = flags;
    nrecs++;
    return 0;
}

#define MODLOAD_FINIT_MODULE(fd, opts, flags) test_finit_module((fd), (opts), (flags))
#include "../base/modload.c"
#undef MODLOAD_FINIT_MODULE

void say(const char *fmt, ...)
{
    (void)fmt;
}

double uptime(void)
{
    return 1.0;
}

static void reset_mock(void)
{
    nrecs = 0;
    fail_mod[0] = '\0';
    fail_errno = 0;
    fail_mod2[0] = '\0';
    fail_errno2 = 0;
    eexist_mod[0] = '\0';
}

static int find_rec(const char *name)
{
    for (int i = 0; i < nrecs; i++) {
        if (strcmp(recs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int count_rec(const char *name)
{
    int c = 0;
    for (int i = 0; i < nrecs; i++) {
        if (strcmp(recs[i].name, name) == 0) {
            c++;
        }
    }
    return c;
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    assert(n > 0 && n < (int)sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                perror(tmp);
                abort();
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        perror(tmp);
        abort();
    }
}

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(path);
        abort();
    }
    size_t len = strlen(content);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        assert(w > 0);
        off += (size_t)w;
    }
    assert(close(fd) == 0);
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d != NULL) {
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char sub[1024];
            if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) < (int)sizeof sub) {
                rm_rf(sub);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

static void make_mod(const char *moddir, const char *rel, const char *tag)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", moddir, rel);
    char *slash = strrchr(path, '/');
    if (slash != NULL) {
        *slash = '\0';
        mkdir_p(path);
        *slash = '/';
    }
    write_file(path, tag);
}

static void set_moddir(const char *path)
{
    int n = snprintf(dir, sizeof dir, "%s", path);
    assert(n > 0 && n < (int)sizeof dir);
}

static void test_modprobe_precedence_and_blacklist(const char *base)
{
    char root[384], moddir[512], p[1024];
    struct utsname u;
    uname(&u);
    assert(snprintf(root, sizeof root, "%s/t1", base) < (int)sizeof root);
    assert(snprintf(moddir, sizeof moddir, "%s/lib/modules/%s", root, u.release) < (int)sizeof moddir);
    mkdir_p(moddir);

    make_mod(moddir, "kernel/nvidia.ko.zst", "nvidia");
    make_mod(moddir, "kernel/nouveau.ko.zst", "nouveau");
    make_mod(moddir, "kernel/masked_mod.ko", "masked_mod");
    make_mod(moddir, "kernel/dash_mod.ko.zst", "dash_mod");

    snprintf(p, sizeof p, "%s/modules.dep", moddir);
    write_file(p,
               "kernel/nvidia.ko.zst:\n"
               "kernel/nouveau.ko.zst:\n"
               "kernel/masked_mod.ko:\n"
               "kernel/dash_mod.ko.zst:\n");
    snprintf(p, sizeof p, "%s/modules.alias", moddir);
    write_file(p,
               "alias pci:v000010DE* nvidia\n"
               "alias pci:v000010DE* nouveau\n"
               "alias pci:v0000AAAA* masked_mod\n"
               "alias pci:v0000DEAD* dash-mod\n");

    snprintf(p, sizeof p, "%s/usr/lib/modprobe.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/usr/local/lib/modprobe.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/etc/modprobe.d", root);
    mkdir_p(p);

    snprintf(p, sizeof p, "%s/usr/lib/modprobe.d/10-gpu.conf", root);
    write_file(p, "blacklist nouveau\noptions nvidia NVreg_A=1 # comment\n");
    snprintf(p, sizeof p, "%s/usr/lib/modprobe.d/20-shadowed.conf", root);
    write_file(p, "blacklist masked_mod\noptions masked_mod bad=1\n");
    snprintf(p, sizeof p, "%s/usr/local/lib/modprobe.d/20-shadowed.conf", root);
    write_file(p, "options masked_mod local_opt=1\n");
    snprintf(p, sizeof p, "%s/etc/modprobe.d/20-shadowed.conf", root);
    assert(symlink("/dev/null", p) == 0);
    snprintf(p, sizeof p, "%s/usr/local/lib/modprobe.d/30-order.conf", root);
    write_file(p, "options nvidia NVreg_B=2\n");
    snprintf(p, sizeof p, "%s/etc/modprobe.d/40-etc.conf", root);
    write_file(p, "options nvidia NVreg_C=3\nblacklist dash-mod\n");

    modload_reset_for_test();
    reset_mock();
    snprintf(p, sizeof p, "%s/no-cmdline", root);
    modload_cmdline_path = p;
    set_moddir(moddir);
    load_modprobe_config(root);
    assert(load_tables() == 0);

    load_for("pci:v000010DEd000028E0");
    load_for("pci:v0000AAAAd00000001");
    load_for("pci:v0000DEADd00000001");

    assert(find_rec("nouveau") == -1);
    assert(find_rec("dash_mod") == -1);
    int idx_nv = find_rec("nvidia");
    assert(idx_nv >= 0);
    assert(strcmp(recs[idx_nv].opts, "NVreg_A=1 NVreg_B=2 NVreg_C=3") == 0);
    assert(recs[idx_nv].flags == MODULE_INIT_COMPRESSED_FILE);

    int idx_mk = find_rec("masked_mod");
    assert(idx_mk >= 0);
    assert(strcmp(recs[idx_mk].opts, "") == 0);
    assert(recs[idx_mk].flags == 0);

    modload_reset_for_test();
}

static void test_cmdline_blacklist(const char *base)
{
    char root[384], moddir[512], cmdline[512], p[1024];
    struct utsname u;
    uname(&u);
    assert(snprintf(root, sizeof root, "%s/t2", base) < (int)sizeof root);
    assert(snprintf(moddir, sizeof moddir, "%s/lib/modules/%s", root, u.release) < (int)sizeof moddir);
    mkdir_p(moddir);

    make_mod(moddir, "kernel/nouveau.ko.zst", "nouveau");
    make_mod(moddir, "kernel/foo_bar.ko.zst", "foo_bar");
    make_mod(moddir, "kernel/baz_qux.ko.zst", "baz_qux");
    make_mod(moddir, "kernel/quoted_mod.ko.zst", "quoted_mod");
    make_mod(moddir, "kernel/not_this.ko.zst", "not_this");
    make_mod(moddir, "kernel/hard_dep.ko.zst", "hard_dep");
    make_mod(moddir, "kernel/parent_mod.ko.zst", "parent_mod");

    snprintf(p, sizeof p, "%s/modules.dep", moddir);
    write_file(p,
               "kernel/nouveau.ko.zst:\n"
               "kernel/foo_bar.ko.zst:\n"
               "kernel/baz_qux.ko.zst:\n"
               "kernel/quoted_mod.ko.zst:\n"
               "kernel/not_this.ko.zst:\n"
               "kernel/hard_dep.ko.zst:\n"
               "kernel/parent_mod.ko.zst: kernel/hard_dep.ko.zst\n");
    snprintf(p, sizeof p, "%s/modules.alias", moddir);
    write_file(p,
               "alias pci:v1 nouveau\n"
               "alias pci:v2 foo_bar\n"
               "alias pci:v3 baz_qux\n"
               "alias pci:v4 quoted_mod\n"
               "alias pci:v5 not_this\n"
               "alias pci:v6 hard_dep\n"
               "alias pci:v7 parent_mod\n");

    snprintf(cmdline, sizeof cmdline, "%s/cmdline", root);
    write_file(cmdline,
               "root=LABEL=locachy-root quiet other=\"modprobe.blacklist=not_this\" "
               "modprobe.blacklist=nouveau,foo-bar \"modprobe.blacklist=baz_qux,hard-dep\" "
               "modprobe.blacklist=\"quoted-mod,foo_bar\"\n");

    modload_reset_for_test();
    reset_mock();
    modload_cmdline_path = cmdline;
    set_moddir(moddir);
    load_modprobe_config(root);
    assert(load_tables() == 0);

    load_for("pci:v1");
    load_for("pci:v2");
    load_for("pci:v3");
    load_for("pci:v4");
    load_for("pci:v5");
    load_for("pci:v6");

    assert(find_rec("nouveau") == -1);
    assert(find_rec("foo_bar") == -1);
    assert(find_rec("baz_qux") == -1);
    assert(find_rec("quoted_mod") == -1);
    assert(find_rec("hard_dep") == -1);
    assert(find_rec("not_this") >= 0);

    /* Explicit request of blacklisted module is suppressed, while hard dep of a
     * non-blacklisted module still resolves. */
    assert(load_named("foo-bar", 1) == 0);
    assert(find_rec("foo_bar") == -1);

    load_for("pci:v7");
    int idx_dep = find_rec("hard_dep");
    int idx_par = find_rec("parent_mod");
    assert(idx_dep >= 0 && idx_par > idx_dep);

    modload_reset_for_test();
}

static void test_softdep_and_cycles(const char *base)
{
    char root[384], moddir[512], p[1024];
    struct utsname u;
    uname(&u);
    assert(snprintf(root, sizeof root, "%s/t3", base) < (int)sizeof root);
    assert(snprintf(moddir, sizeof moddir, "%s/lib/modules/%s", root, u.release) < (int)sizeof moddir);
    mkdir_p(moddir);

    const char *mods[] = {
        "gpu_unlock", "helper_pre", "failing_pre", "nvidia", "nvidia_modeset",
        "nvidia_drm", "nvidia_uvm", "hp_omen_gpu", "failing_post", "blocked_post",
        "leds_gpio", "alias_user", "cyc_a", "cyc_b", "post_a", "post_b",
        "nvidia_already", "post_after_eexist", "failing_main", "never_post",
        "aes_mod", "custom_mod",
    };
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        char rel[128];
        snprintf(rel, sizeof rel, "kernel/%s.ko.zst", mods[i]);
        make_mod(moddir, rel, mods[i]);
    }

    snprintf(p, sizeof p, "%s/modules.dep", moddir);
    write_file(p,
               "kernel/gpu_unlock.ko.zst:\n"
               "kernel/helper_pre.ko.zst:\n"
               "kernel/failing_pre.ko.zst:\n"
               "kernel/nvidia.ko.zst:\n"
               "kernel/nvidia_modeset.ko.zst: kernel/nvidia.ko.zst\n"
               "kernel/nvidia_drm.ko.zst: kernel/nvidia_modeset.ko.zst kernel/nvidia.ko.zst\n"
               "kernel/nvidia_uvm.ko.zst: kernel/nvidia.ko.zst\n"
               "kernel/hp_omen_gpu.ko.zst:\n"
               "kernel/failing_post.ko.zst:\n"
               "kernel/blocked_post.ko.zst:\n"
               "kernel/leds_gpio.ko.zst:\n"
               "kernel/alias_user.ko.zst:\n"
               "kernel/cyc_a.ko.zst:\n"
               "kernel/cyc_b.ko.zst:\n"
               "kernel/post_a.ko.zst:\n"
               "kernel/post_b.ko.zst:\n"
               "kernel/nvidia_already.ko.zst:\n"
               "kernel/post_after_eexist.ko.zst:\n"
               "kernel/failing_main.ko.zst:\n"
               "kernel/never_post.ko.zst:\n"
               "kernel/aes_mod.ko.zst:\n"
               "kernel/custom_mod.ko.zst:\n");
    snprintf(p, sizeof p, "%s/modules.alias", moddir);
    write_file(p,
               "alias pci:v000010DE* nvidia_drm\n"
               "alias platform:leds-gpio leds_gpio\n"
               "alias crypto-* aes_mod\n"
               "alias *:custom custom_mod\n");
    snprintf(p, sizeof p, "%s/modules.softdep", moddir);
    write_file(p,
               "# kernel extracted softdeps\n"
               "softdep cifs gcm\n"
               "softdep nvidia post: nvidia-uvm\n"
               "softdep alias_user pre: platform:leds-gpio\n");

    snprintf(p, sizeof p, "%s/etc/modprobe.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/etc/modprobe.d/gpu.conf", root);
    write_file(p,
               "blacklist blocked_post\n"
               "softdep nvidia pre: gpu-unlock missing_pre failing_pre\n"
               "softdep nvidia post: hp_omen_gpu blocked-post missing_post failing_post pre: helper_pre\n"
               "softdep cyc_a pre: cyc_b\n"
               "softdep cyc_b pre: cyc_a\n"
               "softdep post_a post: post_b\n"
               "softdep post_b post: post_a\n"
               "softdep nvidia_already post: post_after_eexist\n"
               "softdep failing_main post: never_post\n");

    modload_reset_for_test();
    reset_mock();
    snprintf(p, sizeof p, "%s/no-cmdline", root);
    modload_cmdline_path = p;
    snprintf(fail_mod, sizeof fail_mod, "%s", "failing_pre");
    fail_errno = ENODEV;
    snprintf(fail_mod2, sizeof fail_mod2, "%s", "failing_post");
    fail_errno2 = EIO;
    snprintf(eexist_mod, sizeof eexist_mod, "%s", "nvidia_already");

    set_moddir(moddir);
    load_modprobe_config(root);
    assert(load_tables() == 0);

    load_for("pci:v000010DEd000028E0");

    int idx_unlock = find_rec("gpu_unlock");
    int idx_helper = find_rec("helper_pre");
    int idx_nv = find_rec("nvidia");
    int idx_omen = find_rec("hp_omen_gpu");
    int idx_uvm = find_rec("nvidia_uvm");
    int idx_ms = find_rec("nvidia_modeset");
    int idx_drm = find_rec("nvidia_drm");
    assert(idx_unlock >= 0 && idx_helper >= 0 && idx_nv >= 0);
    assert(idx_unlock < idx_nv && idx_helper < idx_nv);
    assert(idx_omen > idx_nv && idx_uvm > idx_nv);
    assert(idx_ms > idx_nv && idx_drm > idx_ms);
    assert(find_rec("blocked_post") == -1);
    assert(find_rec("failing_pre") == -1);
    assert(find_rec("failing_post") == -1);

    /* Alias softdep (platform:leds-gpio) */
    assert(load_named("alias_user", 1) == 0);
    int idx_leds = find_rec("leds_gpio");
    int idx_user = find_rec("alias_user");
    assert(idx_leds >= 0 && idx_user > idx_leds);

    /* Cycles must terminate cleanly and load each module once. */
    assert(load_named("cyc_a", 1) == 0);
    assert(count_rec("cyc_a") == 1 && count_rec("cyc_b") == 1);
    assert(load_named("post_a", 1) == 0);
    assert(count_rec("post_a") == 1 && count_rec("post_b") == 1);

    /* EEXIST still triggers post: softdep once. */
    assert(load_named("nvidia_already", 1) == 0);
    assert(count_rec("post_after_eexist") == 1);
    assert(load_named("nvidia_already", 1) == 0);
    assert(count_rec("post_after_eexist") == 1);

    /* Failed module must not run its post: softdep. */
    snprintf(fail_mod, sizeof fail_mod, "%s", "failing_main");
    fail_errno = ENOENT;
    assert(load_named("failing_main", 1) < 0);
    assert(find_rec("never_post") == -1);

    /* Wildcard and non-colon alias matching. */
    assert(load_named("crypto-aes", 1) == 0);
    assert(find_rec("aes_mod") >= 0);
    assert(load_for("pci:custom") > 0);
    assert(find_rec("custom_mod") >= 0);

    modload_reset_for_test();
}

static void test_modules_load_before_coldplug(const char *base)
{
    char root[512], moddir[768], sysfs[768], p[1024];
    struct utsname u;
    uname(&u);
    snprintf(root, sizeof root, "%s/t4", base);
    snprintf(moddir, sizeof moddir, "%s/lib/modules/%s", root, u.release);
    snprintf(sysfs, sizeof sysfs, "%s/sys/devices/pci0000:00/0000:00:01.0", root);
    mkdir_p(moddir);
    mkdir_p(sysfs);

    const char *mods[] = {
        "corefreqk", "gpu_unlock", "from_etc", "from_usr_lib",
        "disabled_mod", "blocked_explicit", "nvidia", "hp_omen_gpu",
    };
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        char rel[128];
        snprintf(rel, sizeof rel, "kernel/%s.ko.zst", mods[i]);
        make_mod(moddir, rel, mods[i]);
    }

    snprintf(p, sizeof p, "%s/modules.dep", moddir);
    write_file(p,
               "kernel/corefreqk.ko.zst:\n"
               "kernel/gpu_unlock.ko.zst:\n"
               "kernel/from_etc.ko.zst:\n"
               "kernel/from_usr_lib.ko.zst:\n"
               "kernel/disabled_mod.ko.zst:\n"
               "kernel/blocked_explicit.ko.zst:\n"
               "kernel/nvidia.ko.zst:\n"
               "kernel/hp_omen_gpu.ko.zst:\n");
    snprintf(p, sizeof p, "%s/modules.alias", moddir);
    write_file(p, "alias pci:v000010DE* nvidia\n");

    snprintf(p, sizeof p, "%s/modalias", sysfs);
    write_file(p, "pci:v000010DEd000028E0\n");

    snprintf(p, sizeof p, "%s/etc/modprobe.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/etc/modprobe.d/local.conf", root);
    write_file(p,
               "options corefreqk Register_ClockSource=1\n"
               "blacklist blocked_explicit\n"
               "softdep nvidia pre: gpu_unlock\n"
               "softdep nvidia post: hp_omen_gpu\n");

    snprintf(p, sizeof p, "%s/usr/lib/modules-load.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/usr/local/lib/modules-load.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/etc/modules-load.d", root);
    mkdir_p(p);

    snprintf(p, sizeof p, "%s/usr/lib/modules-load.d/10-base.conf", root);
    write_file(p, "from_usr_lib\n");
    snprintf(p, sizeof p, "%s/etc/modules-load.d/10-base.conf", root);
    write_file(p, "# overridden by /etc\nfrom_etc ; inline comment\n");
    snprintf(p, sizeof p, "%s/usr/lib/modules-load.d/20-disabled.conf", root);
    write_file(p, "disabled_mod\n");
    snprintf(p, sizeof p, "%s/etc/modules-load.d/20-disabled.conf", root);
    assert(symlink("/dev/null", p) == 0);
    snprintf(p, sizeof p, "%s/usr/local/lib/modules-load.d/30-explicit.conf", root);
    write_file(p,
               "\n"
               "; semicolon comment\n"
               "  # hash comment\n"
               "  corefreqk   # trailing comment\n"
               "gpu-unlock\n"
               "blocked-explicit\n");

    modload_reset_for_test();
    reset_mock();
    snprintf(p, sizeof p, "%s/no-cmdline", root);
    modload_cmdline_path = p;
    snprintf(sysfs, sizeof sysfs, "%s/sys/devices", root);
    modload_sysfs_devices = sysfs;
    modload_quiet_ms = 0;

    assert(modules_start(root) == 0);

    int idx_etc = find_rec("from_etc");
    int idx_cf = find_rec("corefreqk");
    int idx_gu = find_rec("gpu_unlock");
    int idx_nv = find_rec("nvidia");
    int idx_omen = find_rec("hp_omen_gpu");

    assert(idx_etc >= 0);
    assert(find_rec("from_usr_lib") == -1);
    assert(find_rec("disabled_mod") == -1);
    assert(find_rec("blocked_explicit") == -1);
    assert(idx_cf >= 0 && idx_gu >= 0 && idx_nv >= 0 && idx_omen >= 0);
    assert(idx_etc < idx_cf && idx_cf < idx_gu && idx_gu < idx_nv && idx_nv < idx_omen);
    assert(count_rec("gpu_unlock") == 1);
    assert(strcmp(recs[idx_cf].opts, "Register_ClockSource=1") == 0);

    modload_reset_for_test();
}

static void test_cmdline_module_options(const char *base)
{
    char root[384], moddir[512], p[1024], cmdline[1024];
    struct utsname u;
    uname(&u);
    assert(snprintf(root, sizeof root, "%s/t5", base) < (int)sizeof root);
    assert(snprintf(moddir, sizeof moddir, "%s/lib/modules/%s", root, u.release) < (int)sizeof moddir);
    mkdir_p(moddir);

    make_mod(moddir, "kernel/i915.ko.zst", "i915");
    make_mod(moddir, "kernel/xe.ko.zst", "xe");
    make_mod(moddir, "kernel/nvidia_drm.ko.zst", "nvidia_drm");
    make_mod(moddir, "kernel/gpu_unlock.ko.zst", "gpu_unlock");
    make_mod(moddir, "kernel/spaced.ko.zst", "spaced");

    snprintf(p, sizeof p, "%s/modules.dep", moddir);
    write_file(p,
               "kernel/i915.ko.zst:\n"
               "kernel/xe.ko.zst:\n"
               "kernel/nvidia_drm.ko.zst:\n"
               "kernel/gpu_unlock.ko.zst:\n"
               "kernel/spaced.ko.zst:\n");
    snprintf(p, sizeof p, "%s/modules.alias", moddir);
    write_file(p, "");

    snprintf(p, sizeof p, "%s/etc/modprobe.d", root);
    mkdir_p(p);
    snprintf(p, sizeof p, "%s/etc/modprobe.d/10-test.conf", root);
    write_file(p, "options i915 enable_psr=0\n");

    snprintf(cmdline, sizeof cmdline, "%s/cmdline", root);
    write_file(cmdline,
               "quiet nouveau.modeset=0 nvidia-drm.modeset=1 "
               "i915.force_probe=!4688 xe.force_probe=4688 "
               "gpu_unlock.targets=0000:01:00.0 "
               "spaced.msg=\"hello world\" spaced.debug "
               "modprobe.blacklist=not_an_option\n");

    modload_reset_for_test();
    reset_mock();
    modload_cmdline_path = cmdline;
    set_moddir(moddir);
    load_modprobe_config(root);
    assert(load_tables() == 0);

    assert(load_named("i915", 0) == 0);
    assert(load_named("xe", 0) == 0);
    assert(load_named("nvidia_drm", 0) == 0);
    assert(load_named("gpu_unlock", 0) == 0);
    assert(load_named("spaced", 0) == 0);

    int idx_i915 = find_rec("i915");
    int idx_xe = find_rec("xe");
    int idx_nvdrm = find_rec("nvidia_drm");
    int idx_gu = find_rec("gpu_unlock");
    int idx_sp = find_rec("spaced");
    assert(idx_i915 >= 0 && strcmp(recs[idx_i915].opts, "enable_psr=0 force_probe=!4688") == 0);
    assert(idx_xe >= 0 && strcmp(recs[idx_xe].opts, "force_probe=4688") == 0);
    assert(idx_nvdrm >= 0 && strcmp(recs[idx_nvdrm].opts, "modeset=1") == 0);
    assert(idx_gu >= 0 && strcmp(recs[idx_gu].opts, "targets=0000:01:00.0") == 0);
    assert(idx_sp >= 0 && strcmp(recs[idx_sp].opts, "msg=\"hello world\" debug") == 0);
    assert(strcmp(options_for("modprobe"), "") == 0);

    modload_reset_for_test();
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/test-modload-XXXXXX";
    const char *base = NULL;
    int own_dir = 0;

    if (argc > 1) {
        base = argv[1];
        mkdir_p(base);
    } else {
        base = mkdtemp(tmpl);
        assert(base != NULL);
        own_dir = 1;
    }

    test_modprobe_precedence_and_blacklist(base);
    test_cmdline_blacklist(base);
    test_softdep_and_cycles(base);
    test_modules_load_before_coldplug(base);
    test_cmdline_module_options(base);

    if (own_dir) {
        rm_rf(base);
    }
    puts("PASS modload: modprobe.d precedence/options/blacklist, /proc/cmdline blacklist, "
         "softdep pre/post and cycles, modules.softdep, modules-load.d before coldplug");
    return 0;
}
