#!/bin/sh
# base/duress.c against a scratch directory only: every path it touches
# (verifier, keys, block devices, TPM) points there. test-duress.sh SCRATCH
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
scratch=${1:?usage: test-duress.sh SCRATCH}
mkdir -p "$scratch/sys"
dir=$(CDPATH= cd -- "$scratch" && pwd)
for d in sda2 sda3 sdb1 sdc1 sdd; do mkdir -p "$dir/sys/$d"; done
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DDURESS_TEST_NO_POWEROFF \
    -DDURESS_FILE="\"$dir/duress\"" -DDURESS_SYS_BLOCK="\"$dir/sys\"" -DDURESS_DEV_DIR="\"$dir\"" \
    -DDURESS_KEY_PCR="\"$dir/pcr.pem\"" \
    -DDURESS_KEY_PK="\"$dir/PK.key\"" -DDURESS_KEY_KEK="\"$dir/KEK.key\"" -DDURESS_KEY_DB="\"$dir/db.key\"" \
    -I"$src/base/include" "$src/tools/test-duress.c" -o "$dir/test-duress"
"$dir/test-duress" "$dir"
# The musl build, as process 1 links it (static, the kernel's headers).
if command -v musl-gcc >/dev/null 2>&1; then
    mkdir -p "$dir/uapi"
    for d in linux asm asm-generic; do ln -sfn "/usr/include/$d" "$dir/uapi/$d"; done
    musl-gcc -static -O2 -Wall -Wextra -Werror -I"$src/base/include" -isystem "$dir/uapi" \
        -c "$src/base/duress.c" -o "$dir/duress-musl.o"
    echo "PASS duress.c builds for process 1 (musl, static)"
fi
