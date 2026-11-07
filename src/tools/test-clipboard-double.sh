#!/bin/sh
# Test double for the clipboard service, used only by test-app-registry.c:
# freeze CGROUP freezes the test's cgroup files as the compositor's gate
# would; sync succeeds.
set -eu
case "$1" in
freeze) printf 'populated 1\nfrozen 1\n' >"$2/cgroup.events" ;;
sync) ;;
*) exit 2 ;;
esac
