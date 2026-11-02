#!/bin/bash
# test-network-handoff.sh [DIR]: network-client's handoff of the host's
# interfaces to locachy-net, locachy-setup's debug port and the udevd of
# locachy-net, as any user, in user, network, mount and process namespaces
# of the test's (unshare -rnmpf). The kernel, ip and systemd-udevd are the
# real ones: moves, name and altname clashes, renames and uevents are the
# kernel's. Dummy, ipvlan (a permanent address) and tun interfaces stand
# for cards: DIR/class-net,
# mounted over /sys/class/net, gives each a device of its own (not
# /sys/devices/virtual); the kernel log is DIR/kmsg, rc.conf DIR/rc.conf.
#   1. a card named as one in locachy-net goes, under a free name
#   2. a card of another type than Ethernet (raw IP, as a modem's) goes
#   3. a card renamed before the rule's program runs goes, by its index
#   4. a virtual interface stays; an interface gone is no error
#   5. one that cannot go (its altname taken there) stays, down, said once;
#      the others go all the same
#   6. the debug link's port is its card's permanent address, whatever its
#      name and present address; with no address recorded, none stays
#   7. locachy-setup debug-link on records the card's address and takes it
#      from locachy-net, finds it again under another name, gives it back
#   8. two movers of one card at once: it goes, nothing said
#   9. the host's udevd announces at its start the interfaces still on the
#      host (udev-client's coldplug), and the preset rule moves them
#  10. the udevd of locachy-net: the rules of interfaces only (/etc first,
#      masked ones none), a /dev of its own, never on the host
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-/var/tmp/locachy-network-handoff-test}
if [ "${LOCACHY_HANDOFF_TEST:-}" != inner ]; then
    rm -rf "$out"
    mkdir -p "$out"
    out=$(cd "$out" && pwd)
    LOCACHY_HANDOFF_TEST=inner exec timeout 300 unshare -rnmpf --mount-proc bash "$0" "$out"
fi
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

mount -t tmpfs tmpfs /run
mkdir -p /run/netns /run/udev /run/dbus
mount -t sysfs sysfs /sys
: >"$out/kmsg"
mount --bind "$out/kmsg" /dev/kmsg
mkdir -p "$out/class-net" "$out/devices" "$out/bin" "$out/rules"
ln -s /sys/devices/virtual/net/lo "$out/class-net/lo"
mount --bind "$out/class-net" /sys/class/net
# nmcli (locachy-setup's): there is no NetworkManager here.
printf '#!/bin/sh\nexit 0\n' >"$out/bin/nmcli"
chmod +x "$out/bin/nmcli"
export PATH=$out/bin:$PATH LOCACHY_NETWORK_RC=$out/rc.conf LOCACHY_SETUP_RC=$out/rc.conf
export LOCACHY_NETWORK_CLIENT=$src/link/network-client
NC=$src/link/network-client
nc() { bash "$NC" "$@"; }
ip netns add locachy-net
in_ns() { ip netns exec locachy-net "$@"; }
in_ns ip link set lo up

slot=16
# card NAME MAC [tun]: a card's interface on the host, a device of its own
# in class-net (a slot of the test's PCI bus).
card() {
    if [ "${3:-}" = tun ]; then ip tuntap add mode tun name "$1"; else ip link add "$1" address "$2" type dummy; fi
    device "$1"
}
# perm_card NAME PERM: a card with a permanent address, PERM (an ipvlan:
# NET_ADDR_PERM, so the kernel keeps its address at registration as its
# permanent one, net/core/dev.c); NetworkManager's address on it later is
# its lower interface's, set by new_address NAME ADDRESS.
# In locachy-net with in=in_ns.
perm_card() {
    ${in:-} ip link add "l-$1" address "$2" type dummy
    ${in:-} ip link add "$1" link "l-$1" type ipvlan mode l2
}
new_address() { ${in:-} ip link set "l-$1" address "$2"; }
device() {
    slot=$((slot + 1))
    mkdir -p "$out/devices/pci0000:00/0000:00:$slot.0/net/$1"
    ln -sfn "$out/devices/pci0000:00/0000:00:$slot.0/net/$1" "$out/class-net/$1"
}
ifindex() { ip -o link show dev "$1" 2>/dev/null | sed -n 's/^\([0-9]*\): .*/\1/p'; }
here() { ip -o link show | grep -qi "link/[a-z]* $1 "; }
there() { in_ns ip -o link show | grep -i "link/[a-z]* $1 " | sed -n 's/^[0-9]*: \([^:@]*\).*/\1/p'; }
said() { grep -c "$1" "$out/kmsg"; }
rc() { printf '%s\n' 'service_network="/usr/local/lib/locachy/network-client"' "$@" >"$out/rc.conf"; }
rc
in_ns ip link add eth0 address 02:00:00:00:00:01 type dummy

# 1. A card named as one in locachy-net (a USB card hotplugged after boot).
card eth0 52:54:00:00:00:21
nc --ifindex "$(ifindex eth0)"
new=$(there 52:54:00:00:00:21)
check "a card named eth0 as one in locachy-net goes there, as $new" \
    "! here 52:54:00:00:00:21 && [[ \$new =~ ^eth[1-9][0-9]*$ ]] && [ \$(said '<3>') = 0 ]"
card eth0 52:54:00:00:00:22
nc --ifindex "$(ifindex eth0)"
check "and a second one" "! here 52:54:00:00:00:22 && [ -n \"\$(there 52:54:00:00:00:22)\" ]"

# 2. Raw IP (ARPHRD_NONE), as qmi_wwan's and mhi_net's.
card wwan0 - tun
nc --ifindex "$(ifindex wwan0)"
check "a card of raw IP goes (type $(in_ns cat /sys/class/net/wwan0/type 2>/dev/null))" \
    "[ -z \"\$(ifindex wwan0)\" ] && [ \"\$(in_ns cat /sys/class/net/wwan0/type)\" = 65534 ]"

# 3. Renamed by udevd (NAME=, a .link file) after the rule was read.
card eth5 52:54:00:00:00:25
index=$(ifindex eth5)
ip link set eth5 name lan0
device lan0
nc --interface eth5
check "by its old name (\$kernel): it stays" "here 52:54:00:00:00:25"
nc --ifindex "$index"
check "by its index: it goes" "! here 52:54:00:00:00:25 && [ \"\$(there 52:54:00:00:00:25)\" = lan0 ]"

# 4. Software's own interfaces; one gone.
ip link add v0 address 52:54:00:00:00:30 type dummy
ln -s /sys/devices/virtual/net/v0 "$out/class-net/v0"
nc --ifindex "$(ifindex v0)"
check "a virtual interface stays" "here 52:54:00:00:00:30"
nc --ifindex 999
status=$?
check "an interface gone: no error, nothing said" "[ $status = 0 ] && [ \$(said '<3>') = 0 ]"

# 5. One that cannot go: its altname is a name in locachy-net (EEXIST, with or
# without a pattern).
card c1 52:54:00:00:00:41
ip link property add dev c1 altname lan9
in_ns ip link add lan9 address 02:00:00:00:00:09 type dummy
ip link set c1 up
card eth6 52:54:00:00:00:46
card eth7 52:54:00:00:00:47
nc --interfaces
status=$?
check "the one that cannot go stays, down, said once at <3>" \
    "here 52:54:00:00:00:41 && ! ip -o link show c1 | grep -q '[<,]UP[,>]' && [ \$(said '<3>network-client: c1 stays on the host, down') = 1 ]"
check "the others go all the same; the whole fails ($status)" \
    "! here 52:54:00:00:00:46 && ! here 52:54:00:00:00:47 && [ $status = 1 ]"
ip link del c1

# 6. The debug link's port: debug_link_mac, its card's permanent address.
rc 'service_link="/usr/local/lib/locachy/link-client fe80::1 2300"' 'debug_link_interface="eth0"' \
    'debug_link_mac="52:54:00:00:00:10"'
card eth0 52:54:00:00:00:31
perm_card eth1 52:54:00:00:00:10
new_address eth1 02:12:34:56:78:9a
device eth1
nc --interfaces
check "another card named eth0 (debug_link_interface) goes" "! here 52:54:00:00:00:31"
check "the card of debug_link_mac stays, its address changed (permaddr $(nc --address eth1))" \
    "ip -o link show dev eth1 | grep -q 'link/ether 02:12:34:56:78:9a '"
check "network-client --debug-port: eth1" "[ \"\$(nc --debug-port)\" = eth1 ]"
card eth2 02:00:00:00:00:10
check "a card without a permanent address: its address names it (as NetworkManager)" \
    "[ \"\$(nc --address eth2)\" = 02:00:00:00:00:10 ]"
ip link del eth2
rc 'service_link="/usr/local/lib/locachy/link-client fe80::1 2300"' 'debug_link_interface="eth1"'
nc --interfaces
check "the link on, no debug_link_mac: none stays, said at <4>" \
    "[ -z \"\$(ifindex eth1)\" ] && [ \$(said '<4>network-client: the debug link has no port') -ge 1 ]"

# 7. locachy-setup debug-link on, a card in locachy-net (NetworkManager's
# address on it, not its own).
in=in_ns perm_card lan4 52:54:00:00:00:44
in=in_ns new_address lan4 02:00:00:00:00:44
rc 'debug_link_interface="eth0"'
bash "$src/link/locachy-setup" debug-link on fe80::1 2300 lan4 >"$out/setup.log" 2>&1
device lan4
check "debug-link on lan4: its permanent address recorded, the port here" \
    "grep -qx 'debug_link_mac=\"52:54:00:00:00:44\"' $out/rc.conf && grep -q '^service_link=' $out/rc.conf && [ -n \"\$(ifindex lan4)\" ]"
ip link set lan4 name lan5
device lan5
check "renamed lan5 (another boot): --debug-port lan5, it stays" \
    "[ \"\$(nc --debug-port)\" = lan5 ] && nc --interfaces && [ -n \"\$(ifindex lan5)\" ]"
bash "$src/link/locachy-setup" debug-link off >>"$out/setup.log" 2>&1
check "debug-link off: it goes to locachy-net" "[ -z \"\$(ifindex lan5)\" ] && in_ns ip link show dev lan5 >/dev/null"
rm "$out/class-net/lan5" # gone from the host's sysfs, as the kernel's
bash "$src/link/locachy-setup" debug-link on >>"$out/setup.log" 2>&1
device lan5
check "debug-link on again: found in locachy-net by its address, here again" "[ -n \"\$(ifindex lan5)\" ]"
in_ns ip link add r0 type dummy
check "a card with a random address (addr_assign_type 1) is refused" \
    "! in_ns bash $NC --address r0 2>/dev/null && ! bash $src/link/locachy-setup debug-link on fe80::1 2300 r0 >>$out/setup.log 2>&1"
ip link set dev lan5 netns locachy-net
rc

# 8. Two movers of one card at once (network-client's start, the rule's run).
card eth12 52:54:00:00:00:52
before=$(said '<3>')
nc --ifindex "$(ifindex eth12)" & nc --interfaces & wait
check "two movers at once: it goes, nothing said" "! here 52:54:00:00:00:52 && [ \$(said '<3>') = $before ]"

# 9. A card that appeared while the host's udevd was not there (its start,
# its restart): its coldplug (udev-client's line) announces it, the preset
# rule moves it. The rule's program is this network-client; DEVPATH!=
# "/devices/virtual/*" is left out, the test's cards being dummies.
card eth13 52:54:00:00:00:53
sed -e 's|DEVPATH!="/devices/virtual/\*", ||' \
    -e "s|/usr/local/lib/locachy/network-client|/usr/bin/env LOCACHY_NETWORK_RC=$out/rc.conf PATH=$PATH /bin/bash $NC|" \
    "$src/link/90-locachy-network.rules" >"$out/rules/90-locachy-network.rules"
check "the preset rule is valid (udevadm verify)" \
    "udevadm verify --resolve-names=never $src/link/90-locachy-network.rules >$out/verify.log 2>&1"
unshare --mount --propagation private bash -c 'mount -t tmpfs tmpfs /run/udev
    mount -t tmpfs -o ro tmpfs /usr/lib/udev/rules.d
    mount --bind "$0" /etc/udev/rules.d
    exec /usr/lib/systemd/systemd-udevd' "$out/rules" 2>"$out/host-udevd.log" &
hudevd=$!
for _ in $(seq 1 100); do [ -S "/proc/$hudevd/root/run/udev/control" ] && break; sleep 0.05; done
check "until the host's udevd announces it, eth13 is on the host" "here 52:54:00:00:00:53"
# What udev-client's line announces, read in the real sysfs (udevadm reads
# devices under /sys only, not the test's class-net); then that announcement
# of eth13, the write of "add" to its uevent file that udevadm trigger makes.
coldplug=$(grep '^udevadm trigger --type=devices' "$src/link/udev-client")
unshare --mount bash -c "umount /sys/class/net && $coldplug --dry-run --verbose" >"$out/coldplug.list" 2>/dev/null
check "udev-client's coldplug ($coldplug) announces eth13" \
    "grep -qx /sys/devices/virtual/net/eth13 $out/coldplug.list"
nsenter -t "$hudevd" -m bash -c 'printf add >/sys/devices/virtual/net/eth13/uevent && udevadm settle -t 10'
check "announced, eth13 goes (the preset rule)" "! here 52:54:00:00:00:53 && [ -n \"\$(there 52:54:00:00:00:53)\" ]"
kill "$hudevd"
wait "$hudevd" 2>/dev/null

# 10. The udevd of locachy-net, started as network-client starts it. In
# /etc/udev/rules.d: a rule of no interface's (it must not run), one of
# NetworkManager's replaced, one masked.
nc --udevd 2>/dev/null
status=$?
check "--udevd on the host: refused ($status), nothing hidden" \
    "[ $status = 2 ] && [ -n \"\$(ls /usr/lib/udev/rules.d)\" ]"
mkdir -p "$out/etc-rules"
printf '%s\n' "ACTION==\"add\", RUN+=\"/usr/bin/touch $out/hidden-ran\"" >"$out/etc-rules/95-probe.rules"
printf '%s\n' 'SUBSYSTEM=="net", ENV{PROBE_ETC}="1"' >"$out/etc-rules/85-nm-unmanaged.rules"
ln -s /dev/null "$out/etc-rules/81-net-dhcp.rules"
mount --bind "$out/etc-rules" /etc/udev/rules.d
cat >"$out/netns.sh" <<'INNER'
# In locachy-net, where NetworkManager would run: its /run/udev, its udevd.
nc=$1 out=$2
mount -t tmpfs -o mode=0755 tmpfs /run/udev
unshare --mount --propagation private bash "$nc" --udevd 2>"$out/netns-udevd.log" &
udevd=$!
for _ in $(seq 1 100); do [ -S /run/udev/control ] && break; sleep 0.05; done
ip link add n1 address 02:00:00:00:00:61 type dummy
udevadm settle -t 10
udevadm info /sys/class/net/n1 >"$out/n1.info" 2>&1
root=/proc/$udevd/root
ls "$root/run/udev/rules.d" | tr '\n' ' ' >"$out/netns-rules"
wc -c <"$root/run/udev/rules.d/81-net-dhcp.rules" >"$out/netns-masked"
{ ls -A "$root/usr/lib/udev/rules.d"; ls -A "$root/etc/udev/rules.d"; } >"$out/netns-hidden"
ls -A "$root/dev" | tr '\n' ' ' >"$out/netns-dev"
kill "$udevd"
wait "$udevd"
INNER
in_ns unshare --mount --propagation slave bash "$out/netns.sh" "$NC" "$out"
check "NetworkManager's view: n1 initialized, ID_NET_DRIVER=dummy (50-net-driver)" \
    "grep -q '^E: USEC_INITIALIZED=' $out/n1.info && grep -qx 'E: ID_NET_DRIVER=dummy' $out/n1.info"
check "/etc first: its 85-nm-unmanaged.rules, not /usr/lib's" "grep -qx 'E: PROBE_ETC=1' $out/n1.info"
check "systemd's 99-systemd.rules (SYSTEMD_ALIAS)" "grep -q '^E: SYSTEMD_ALIAS=' $out/n1.info"
check "a rule of no interface's (95-probe.rules) did not run" "[ ! -e $out/hidden-ran ]"
check "its rules: $(cat "$out/netns-rules")" \
    "[ \"\$(cat $out/netns-rules)\" = '50-net-driver.rules 75-net-description.rules 80-net-setup-link.rules 81-net-bridge.rules 81-net-dhcp.rules 82-net-auto-link-local.rules 84-nm-drivers.rules 85-nm-unmanaged.rules 99-systemd.rules ' ]"
check "the masked one (a link to /dev/null) empty" "[ \"\$(cat $out/netns-masked)\" = 0 ]"
check "the other rules directories hidden from it" "[ ! -s $out/netns-hidden ]"
check "its /dev: $(cat "$out/netns-dev")" \
    "! tr ' ' '\n' <$out/netns-dev | grep -qvxE 'null|zero|full|random|urandom|kmsg|char|block|'"
check "it set no mode of the host's device nodes" "! grep -q 'Failed to set owner/mode' $out/netns-udevd.log"

[ "$fail" = 0 ] && echo "PASS network-client: no interface left on the host, by the kernel's rules" ||
    echo "FAIL network-client handoff (files in $out)"
exit "$fail"
