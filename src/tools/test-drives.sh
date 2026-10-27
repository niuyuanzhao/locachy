#!/bin/bash
# test-drives.sh: the drives service with a fake removable disk (scsi_debug,
# an ext4 filesystem labelled LOCTEST), as root on a built system where the
# service runs. A mount namespace with slave propagation made before the disk
# appears stands for a running container.
#   1. the disk is mounted at ~/Drives/LOCTEST, and ~/Drives is shared
#   2. the earlier namespace sees it there too
#   3. ejected as the file manager does (unmount, power off): the folder goes,
#      in the namespace as well
#   4. plugged in again and pulled out without ejecting: unmounted
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
pgrep -x drives >/dev/null || { echo "the drives service is not running (service_drives in rc.conf)"; exit 1; }
lsmod | grep -q "^scsi_debug " && { echo "scsi_debug is loaded already"; exit 1; }
home=$(getent passwd 1000 | cut -d: -f6)
d=$home/Drives
wait_for() { for i in $(seq 20); do eval "$1" && return 0; sleep 0.5; done; return 1; }
disk() { for b in /sys/block/sd*; do
    [ "$(tr -d " " <"$b/device/model" 2>/dev/null)" = scsi_debug ] && echo "${b##*/}"; done | tail -1; }

unshare --mount --propagation slave sleep 120 &
ns=$!
sleep 0.3
modprobe scsi_debug removable=1 dev_size_mb=32 || exit 1
wait_for '[ -n "$(disk)" ]'
dev=$(disk)
echo "== the fake disk: /dev/$dev"
mkfs.ext4 -q -F -L LOCTEST "/dev/$dev"

echo "== 1. mounted"
check "it is mounted at $d/LOCTEST" "wait_for \"mountpoint -q $d/LOCTEST\""
check "$d is a shared mount point" "findmnt -no PROPAGATION $d | grep -q shared"
echo "== 2. in a namespace made before"
check "the earlier namespace sees it" "nsenter --target $ns --mount mountpoint -q $d/LOCTEST"
echo "== 3. ejected"
sync
umount "$d/LOCTEST" && echo 1 >"/sys/block/$dev/device/delete"
check "its folder has gone" "wait_for \"[ ! -e $d/LOCTEST ]\""
check "from the namespace too" "! nsenter --target $ns --mount mountpoint -q $d/LOCTEST"
echo "== 4. pulled out"
echo 1 >/sys/bus/pseudo/drivers/scsi_debug/add_host
wait_for '[ -n "$(disk)" ] && [ "$(disk)" != "$dev" ]'
dev=$(disk)
check "plugged in again (/dev/$dev): mounted again" "wait_for \"mountpoint -q $d/LOCTEST\""
echo 1 >"/sys/block/$dev/device/delete"
check "pulled out: unmounted, folder gone" "wait_for \"[ ! -e $d/LOCTEST ]\""
dmesg | grep "drives:" | tail -6
kill $ns
sleep 0.5
rmmod scsi_debug
[ $fail = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $fail
