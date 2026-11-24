/*
 * Unit test for DRM card enumeration, matching, selection, render-node lookup,
 * and strict-first / fallback-second card probing without touching real GPUs.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../base/screen.c"

void say(const char *fmt, ...)
{
    (void)fmt;
}

int monitor_conf(const char *name, struct monitor_conf *out)
{
    (void)name;
    memset(out, 0, sizeof *out);
    return 0;
}

int monitor_internal(const char *name)
{
    (void)name;
    return 0;
}

int monitor_profiles_select_sysfs(void)
{
    return 0;
}

int control_fd(void)
{
    return -1;
}

void control_wait_take(void)
{
    abort();
}

void base_language_reload(void)
{
}

int panel_containers(const char *name, unsigned long long *ram)
{
    (void)name;
    if (ram) *ram = 0;
    return 0;
}

void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *mid_x0, int *mid_x1)
{
    (void)px; (void)w; (void)h; (void)stride; (void)title; (void)sub;
    (void)ime; (void)reserve; (void)mid_x0; (void)mid_x1;
    abort();
}

static int failures;
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    assert(n > 0 && (size_t)n < sizeof tmp);
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
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    assert(fd >= 0);
    size_t len = strlen(content);
    assert(write(fd, content, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void make_card(const char *root, const char *sys_drm, const char *dev_dri,
                      const char *card_name, const char *driver, const char *pci_slot,
                      const char *render_name, const char *const *conns, const char *const *statuses,
                      int nconns)
{
    char p[PATH_MAX], target[PATH_MAX];
    /* Fake /sys/bus/pci/drivers/<driver> */
    snprintf(p, sizeof p, "%s/sys/bus/pci/drivers/%s", root, driver);
    mkdir_p(p);
    /* Fake /sys/devices/pci0000:00/<pci_slot>/drm/<card_name> and <render_name> */
    snprintf(p, sizeof p, "%s/sys/devices/pci0000:00/%s/drm/%s", root, pci_slot, card_name);
    mkdir_p(p);
    if (render_name && *render_name) {
        snprintf(p, sizeof p, "%s/sys/devices/pci0000:00/%s/drm/%s", root, pci_slot, render_name);
        mkdir_p(p);
        snprintf(p, sizeof p, "%s/%s", dev_dri, render_name);
        write_file(p, "");
    }
    /* driver symlink inside PCI device */
    snprintf(p, sizeof p, "%s/sys/devices/pci0000:00/%s/driver", root, pci_slot);
    snprintf(target, sizeof target, "%s/sys/bus/pci/drivers/%s", root, driver);
    assert(symlink(target, p) == 0);
    /* device symlink inside drm/cardN -> PCI device */
    snprintf(p, sizeof p, "%s/sys/devices/pci0000:00/%s/drm/%s/device", root, pci_slot, card_name);
    snprintf(target, sizeof target, "%s/sys/devices/pci0000:00/%s", root, pci_slot);
    assert(symlink(target, p) == 0);
    /* /sys/class/drm/cardN -> .../drm/cardN */
    snprintf(p, sizeof p, "%s/%s", sys_drm, card_name);
    snprintf(target, sizeof target, "%s/sys/devices/pci0000:00/%s/drm/%s", root, pci_slot, card_name);
    assert(symlink(target, p) == 0);
    /* connectors /sys/class/drm/cardN-CONN/status */
    for (int i = 0; i < nconns; i++) {
        snprintf(p, sizeof p, "%s/%s-%s", sys_drm, card_name, conns[i]);
        mkdir_p(p);
        snprintf(p, sizeof p, "%s/%s-%s/status", sys_drm, card_name, conns[i]);
        write_file(p, statuses[i]);
    }
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            rm_rf(child);
        }
        closedir(d);
    }
    rmdir(path);
}

int main(void)
{
    char tmpl[] = "/tmp/test-drm-card-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char sys_drm[PATH_MAX], dev_dri[PATH_MAX];
    snprintf(sys_drm, sizeof sys_drm, "%s/sys/class/drm", root);
    snprintf(dev_dri, sizeof dev_dri, "%s/dev/dri", root);
    mkdir_p(sys_drm);
    mkdir_p(dev_dri);

    /* Hybrid GPU setup: card0 = Intel i915 (1 connected), card1 = NVIDIA (0 connected). */
    const char *c0_conns[] = { "eDP-1", "HDMI-A-1" };
    const char *c0_stats[] = { "connected\n", "disconnected\n" };
    make_card(root, sys_drm, dev_dri, "card0", "i915", "0000:00:02.0", "renderD128",
              c0_conns, c0_stats, 2);

    const char *c1_conns[] = { "DP-1", "HDMI-A-2" };
    const char *c1_stats[] = { "disconnected\n", "disconnected\n" };
    make_card(root, sys_drm, dev_dri, "card1", "nvidia", "0000:01:00.0", "renderD129",
              c1_conns, c1_stats, 2);

    /* 1. Pure-sysfs enumeration */
    struct screen_card_info cards[8];
    int n = screen_list_cards_from(sys_drm, cards, 8);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(strcmp(cards[0].name, "card0") == 0);
        CHECK(strcmp(cards[0].driver, "i915") == 0);
        CHECK(strcmp(cards[0].pci, "0000:00:02.0") == 0);
        CHECK(cards[0].connected_outputs == 1);

        CHECK(strcmp(cards[1].name, "card1") == 0);
        CHECK(strcmp(cards[1].driver, "nvidia") == 0);
        CHECK(strcmp(cards[1].pci, "0000:01:00.0") == 0);
        CHECK(cards[1].connected_outputs == 0);
    }
    /* Capacity limit respected */
    struct screen_card_info one[1];
    CHECK(screen_list_cards_from(sys_drm, one, 1) == 1 && strcmp(one[0].name, "card0") == 0);
    CHECK(screen_list_cards_from(sys_drm, one, 0) == 0);
    printf("%s pure-sysfs enumeration (card0=i915 connected=1, card1=nvidia connected=0)\n",
           failures ? "FAIL" : "PASS");

    /* 2. Exact name, driver, PCI slot, auto/NULL/empty, case sensitivity, invalid/traversal */
    int before = failures;
    CHECK(screen_card_matches_from(sys_drm, "card0", "card0") == 1);
    CHECK(screen_card_matches_from(sys_drm, "/dev/dri/card0", "card0") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card0", "i915") == 1);
    CHECK(screen_card_matches_from(sys_drm, "/dev/dri/card0", "i915") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card0", "0000:00:02.0") == 1);
    CHECK(screen_card_matches_from(sys_drm, "/dev/dri/card0", "0000:00:02.0") == 1);

    /* card1 must NOT match i915 / card0 / 0000:00:02.0 */
    CHECK(screen_card_matches_from(sys_drm, "card1", "i915") == 0);
    CHECK(screen_card_matches_from(sys_drm, "/dev/dri/card1", "i915") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card1", "card0") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card1", "0000:00:02.0") == 0);

    /* card1 matches nvidia / card1 / 0000:01:00.0 */
    CHECK(screen_card_matches_from(sys_drm, "card1", "nvidia") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card1", "card1") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card1", "0000:01:00.0") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card0", "nvidia") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "xe") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "amdgpu") == 0);

    /* auto / NULL / empty match any valid card name */
    CHECK(screen_card_matches_from(sys_drm, "card0", "auto") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card1", "auto") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card0", NULL) == 1);
    CHECK(screen_card_matches_from(sys_drm, "card1", "") == 1);

    /* Case sensitivity */
    CHECK(screen_card_matches_from(sys_drm, "card0", "I915") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "CARD0") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card1", "NVIDIA") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "AUTO") == 0);

    /* Invalid / traversal card names and selectors */
    CHECK(screen_card_matches_from(sys_drm, "../card0", "auto") == 0);
    CHECK(screen_card_matches_from(sys_drm, "../card0", "card0") == 0);
    CHECK(screen_card_matches_from(sys_drm, "/tmp/card0", "card0") == 0);
    CHECK(screen_card_matches_from(sys_drm, "/dev/dri/renderD128", "auto") == 0);
    CHECK(screen_card_matches_from(sys_drm, "", "auto") == 0);
    CHECK(screen_card_matches_from(sys_drm, NULL, "auto") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "../card0") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "..") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "i915/foo") == 0);
    CHECK(screen_card_matches_from(sys_drm, "card0", "i915;rm") == 0);
    printf("%s card matching (name, driver, pci, auto, case sensitivity, traversal rejection)\n",
           failures == before ? "PASS" : "FAIL");

    /* 3. Card path resolution (seat WLR_DRM_DEVICES / matching check) */
    before = failures;
    char out[PATH_MAX];
    CHECK(screen_select_card_path_from(sys_drm, "i915", out, sizeof out) == 0 &&
          strcmp(out, "/dev/dri/card0") == 0);
    CHECK(screen_select_card_path_from(sys_drm, "card0", out, sizeof out) == 0 &&
          strcmp(out, "/dev/dri/card0") == 0);
    CHECK(screen_select_card_path_from(sys_drm, "0000:00:02.0", out, sizeof out) == 0 &&
          strcmp(out, "/dev/dri/card0") == 0);
    CHECK(screen_select_card_path_from(sys_drm, "nvidia", out, sizeof out) == 0 &&
          strcmp(out, "/dev/dri/card1") == 0);
    CHECK(screen_select_card_path_from(sys_drm, "amdgpu", out, sizeof out) < 0);
    CHECK(screen_select_card_path_from(sys_drm, "auto", out, sizeof out) < 0);
    CHECK(screen_select_card_path_from(sys_drm, "", out, sizeof out) < 0);
    CHECK(screen_select_card_path_from(sys_drm, "../card0", out, sizeof out) < 0);
    printf("%s card path selection\n", failures == before ? "PASS" : "FAIL");

    /* 4. Render-node selection helper */
    before = failures;
    char expected_r0[PATH_MAX * 2];
    snprintf(expected_r0, sizeof expected_r0, "%s/renderD128", dev_dri);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "i915", out, sizeof out) == 0 &&
          strcmp(out, expected_r0) == 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "card0", out, sizeof out) == 0 &&
          strcmp(out, expected_r0) == 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "0000:00:02.0", out, sizeof out) == 0 &&
          strcmp(out, expected_r0) == 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "auto", out, sizeof out) == 0 &&
          strcmp(out, expected_r0) == 0);
    /* NVIDIA card is NOT in the verified VA-API+PRIME allowlist -> rejected */
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "nvidia", out, sizeof out) < 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "card1", out, sizeof out) < 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "0000:01:00.0", out, sizeof out) < 0);
    /* Invalid selector rejected */
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "../card0", out, sizeof out) < 0);

    /* Add card2 = amdgpu and card3 = xe to verify multi-vendor matching and render nodes */
    const char *c2_conns[] = { "DP-2" };
    const char *c2_stats[] = { "disconnected\n" };
    make_card(root, sys_drm, dev_dri, "card2", "amdgpu", "0000:03:00.0", "renderD130",
              c2_conns, c2_stats, 1);
    const char *c3_conns[] = { "HDMI-A-3" };
    const char *c3_stats[] = { "connected\n" };
    make_card(root, sys_drm, dev_dri, "card3", "xe", "0000:04:00.0", "renderD131",
              c3_conns, c3_stats, 1);

    char expected_r2[PATH_MAX * 2], expected_r3[PATH_MAX * 2];
    snprintf(expected_r2, sizeof expected_r2, "%s/renderD130", dev_dri);
    snprintf(expected_r3, sizeof expected_r3, "%s/renderD131", dev_dri);
    CHECK(screen_card_matches_from(sys_drm, "card2", "amdgpu") == 1);
    CHECK(screen_card_matches_from(sys_drm, "card3", "xe") == 1);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "amdgpu", out, sizeof out) == 0 &&
          strcmp(out, expected_r2) == 0);
    CHECK(screen_render_node_for_from(sys_drm, dev_dri, "xe", out, sizeof out) == 0 &&
          strcmp(out, expected_r3) == 0);
    CHECK(screen_list_cards_from(sys_drm, cards, 8) == 4);
    printf("%s render-node selection (i915/xe/amdgpu allowed, nvidia refused)\n",
           failures == before ? "PASS" : "FAIL");

    rm_rf(root);
    if (failures) {
        fprintf(stderr, "FAILED %d checks\n", failures);
        return 1;
    }
    puts("ALL PASSED");
    return 0;
}
