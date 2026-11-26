/* open.c's managed VM windows (vm="1") and plain container windows, rootless:
 * the containers' configuration is a fixture directory given at build time
 * (FIXTURE_CONF), so no mount namespace is needed. Every QEMU window has a
 * "qemu..." app id (GTK "qemu", SDL the program's name), so a VM's windows are
 * named by QEMU's title ("QEMU (TAG)" GTK, "QEMU (TAG-N)" SDL): a window of one
 * VM is neither another VM's nor a plain program's, and a window that only
 * looks like one (another app id) is no VM's. Build with base/rc.c (conf_read)
 * and gio-2.0, -ffunction-sections -fdata-sections -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../base/include/base.h"
#undef CONTAINERS_CONF
#define CONTAINERS_CONF FIXTURE_CONF
#include "../base/open.c"
#include <assert.h>
#include <sys/stat.h>

static void put(const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.conf", CONTAINERS_CONF, name);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* The one name of names that claims the window, "" for none, "?" for more. */
static const char *owner(const char *const *names, const char *app_id, const char *title)
{
    const char *found = "";
    for (; *names; names++) {
        if (open_window_matches(*names, app_id, title)) {
            found = found[0] ? "?" : *names;
        }
    }
    return found;
}

int main(void)
{
    assert(mkdir(CONTAINERS_CONF, 0755) == 0 || errno == EEXIST);
    put("vmbox", "vm=\"1\"\nvm_kind=\"custom\"\nvm_base=\"/var/tmp/base.qcow2\"\n");
    put("debian", "vm=\"1\"\nvm_kind=\"custom\"\n");
    put("debian-12", "vm=\"1\"\nvm_kind=\"custom\"\n");
    put("vmcomp", "vm=\"1\"\nvm_kind=\"compute\"\nvm_name=\"ubuntu-dev\"\n");
    put("vmcomp2", "vm=\"1\"\nvm_kind=\"compute\"\n");
    put("vmwin", "vm=\"1\"\nvm_kind=\"winmaint\"\n");
    put("vmcmd", "vm=\"1\"\nvm_kind=\"custom\"\ncommand=\"/usr/bin/vmlauncher\"\n");
    put("vmqemu", "vm=\"1\"\nvm_kind=\"custom\"\ncommand=\"/usr/bin/qemu-system-x86_64\"\n");
    put("plain", "command=\"/usr/bin/plainapp\"\n");
    put("qemuapp", "command=\"/usr/bin/qemu\"\n");
    static const char *const all[] = { "vmbox", "debian", "debian-12", "vmcomp", "vmcomp2", "vmwin", "vmcmd",
                                       "vmqemu", "plain", "qemuapp", NULL };

    assert(open_vm("vmbox") && open_vm("vmcomp") && open_vm("vmwin") && open_vm("vmcmd"));
    assert(!open_vm("plain") && !open_vm("none") && !open_vm("../vmbox"));
    assert(open_window_qemu("qemu") && open_window_qemu("qemu-system-x86_64"));
    assert(!open_window_qemu("firefox") && !open_window_qemu("") && !open_window_qemu(NULL));

    /* A custom VM: its -name is locachy-vm[NAME]; GTK and SDL titles both carry it. */
    assert(open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox])"));
    assert(open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox]) [Paused]"));
    assert(open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox]): virtio-vga"));
    assert(open_window_matches("vmbox", "qemu-system-x86_64", "QEMU (locachy-vm[vmbox]-0)"));
    assert(open_window_matches("vmbox", "qemu-system-x86_64", "QEMU (locachy-vm[vmbox]-12) [Stopped]"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox2])"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[other])"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox]-)"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU (locachy-vm[vmbox]-0x)"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU (locachy-vm-vmbox)"));
    assert(!open_window_matches("vmbox", "qemu", "QEMU"));
    assert(!open_window_matches("vmbox", "qemu", "Editor"));
    puts("PASS a custom VM's window is named by its QEMU title (GTK, [Paused], : label, SDL), and only that VM's");

    /* debian and debian-12: brackets end the name, so neither claims the other's. */
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[debian])"), "debian") == 0);
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[debian-12])"), "debian-12") == 0);
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[debian]) [Paused]"), "debian") == 0);
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[debian-12]): virtio-vga"), "debian-12") == 0);
    assert(strcmp(owner(all, "qemu-system-x86_64", "QEMU (locachy-vm[debian]-0)"), "debian") == 0);
    assert(strcmp(owner(all, "qemu-system-x86_64", "QEMU (locachy-vm[debian-12]-0)"), "debian-12") == 0);
    puts("PASS debian and debian-12 each claim only their own GTK and SDL windows");

    /* A VM's title under another app id (a browser tab, a terminal) is no VM's. */
    assert(!open_window_matches("vmbox", "firefox", "QEMU (locachy-vm[vmbox])"));
    assert(!open_window_matches("vmbox", "", "QEMU (locachy-vm[vmbox])"));
    assert(!open_window_matches("vmbox", NULL, "QEMU (locachy-vm[vmbox]-0)"));
    assert(!open_window_matches("vmcomp", "foot", "QEMU (locachy-compute[ubuntu-dev])"));
    assert(!open_window_matches("vmwin", "foot", "QEMU (locachy-winmaint)"));
    puts("PASS a VM's title with an app id other than QEMU's is not that VM's");

    /* A compute VM: its -name is locachy-compute[VMNAME] (vm_name, else the app's NAME). */
    assert(open_window_matches("vmcomp", "qemu", "QEMU (locachy-compute[ubuntu-dev])"));
    assert(open_window_matches("vmcomp", "qemu-system-x86_64", "QEMU (locachy-compute[ubuntu-dev]-0)"));
    assert(!open_window_matches("vmcomp", "qemu", "QEMU (locachy-compute[ubuntu-dev2])"));
    assert(!open_window_matches("vmcomp", "qemu", "QEMU (locachy-compute[vmcomp])"));
    assert(!open_window_matches("vmcomp", "qemu", "QEMU (locachy-vm[vmcomp])"));
    assert(!open_window_matches("vmcomp", "qemu", "QEMU (locachy-compute-ubuntu-dev)"));
    assert(open_window_matches("vmcomp2", "qemu", "QEMU (locachy-compute[vmcomp2])"));
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-compute[ubuntu-dev])"), "vmcomp") == 0);
    puts("PASS a compute VM's window is named by locachy-compute[VMNAME]");

    /* winmaint: one machine, named locachy-winmaint. */
    assert(open_window_matches("vmwin", "qemu", "QEMU (locachy-winmaint)"));
    assert(open_window_matches("vmwin", "qemu", "QEMU (locachy-winmaint) [Paused]"));
    assert(open_window_matches("vmwin", "qemu-system-x86_64", "QEMU (locachy-winmaint-0)"));
    assert(!open_window_matches("vmwin", "qemu", "QEMU (locachy-vm[vmwin])"));
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-winmaint)"), "vmwin") == 0);
    puts("PASS the winmaint window is named by locachy-winmaint");

    /* A command= VM is matched by its command's app id, as containers are,
     * but never claims another VM's window, even when its command is QEMU. */
    assert(open_window_matches("vmcmd", "vmlauncher", "Anything"));
    assert(!open_window_matches("vmcmd", "qemu", "QEMU (locachy-vm[vmcmd2])"));
    assert(open_window_matches("vmqemu", "qemu-system-x86_64", "QEMU (mine-0)"));
    assert(!open_window_matches("vmqemu", "qemu-system-x86_64", "QEMU (locachy-vm[vmbox]-0)"));
    assert(strcmp(owner(all, "qemu-system-x86_64", "QEMU (locachy-vm[vmbox]-0)"), "vmbox") == 0);
    assert(strcmp(owner(all, "qemu-system-x86_64", "QEMU (locachy-compute[ubuntu-dev]-0)"), "vmcomp") == 0);
    puts("PASS a command= VM is matched by its command's app id, never another VM's tagged window");

    /* A plain container: its command's app id, never another program's QEMU title. */
    assert(open_window_matches("plain", "plainapp", ""));
    assert(!open_window_matches("plain", "qemu", "QEMU (locachy-vm[plain])"));
    assert(!open_window_matches("plain", "", ""));
    /* One whose ids include "qemu": untagged QEMU windows are its, tagged ones a VM's. */
    assert(open_window_matches("qemuapp", "qemu", "QEMU"));
    assert(open_window_matches("qemuapp", "qemu", "QEMU (mine)"));
    assert(!open_window_matches("qemuapp", "qemu", "QEMU (locachy-vm[vmbox])"));
    assert(!open_window_matches("qemuapp", "qemu", "QEMU (locachy-compute[ubuntu-dev])"));
    assert(!open_window_matches("qemuapp", "qemu", "QEMU (locachy-winmaint)"));
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[vmbox])"), "vmbox") == 0);
    assert(strcmp(owner(all, "qemu", "QEMU"), "qemuapp") == 0);
    /* A reserved prefix no VM's tag matches is no one's. */
    assert(strcmp(owner(all, "qemu", "QEMU (locachy-vm[nosuch])"), "") == 0);
    puts("PASS a plain container's windows are not a VM's, even when its app ids include qemu");
    return 0;
}
