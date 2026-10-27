#!/bin/bash
# Standard ISO acceptance only; no host root privileges or block devices.
# test-installer-vm.sh smoke ISO NEW_TEST_DIRECTORY [--firmware bios|uefi]
# test-installer-vm.sh install ISO NEW_TEST_DIRECTORY [--level x86-64-v2]
set -euo pipefail
source=$(cd "$(dirname "$0")" && pwd)
exec python3 -I "$source/test-installer-vm.py" "$@"
