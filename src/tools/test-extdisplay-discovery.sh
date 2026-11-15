#!/bin/sh
# Exercise real scoped DNS-SD without a system daemon or a physical interface.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ] || ! pkg-config --exists avahi-core; then
    printf '%s\n' 'SKIP: discovery requires Linux and avahi-core'
    exit 77
fi
if [ "$#" -gt 0 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-discovery.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi
flags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
cc $flags $(pkg-config --cflags avahi-core) "$src/progs/extdisplay-advertise.c" \
    $(pkg-config --libs avahi-core) -o "$out/extdisplay-advertise"
cc $flags -I"$src/progs" -DEXTDISPLAY_DISCOVERY_PATH=\"$out/extdisplay-advertise\" \
    "$src/tools/test-extdisplay-discovery.c" "$src/progs/extdisplay-discovery.c" \
    $(pkg-config --cflags --libs avahi-core) -o "$out/discovery-peer"
printf '%s\n' 'PASS: scoped DNS-SD production helper and peer build'
if ! unshare --user --map-auto --map-root-user --net --mount true >/dev/null 2>&1; then
    printf '%s\n' 'SKIP: scoped discovery needs unprivileged user/network namespaces and subordinate IDs'
    exit 77
fi
unshare --user --map-auto --map-root-user --net --mount python3 -I - "$out" <<'PY'
import json
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading

work = Path(sys.argv[1])
peer = str(work / 'discovery-peer')
processes = []

def run(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
    if result.returncode:
        raise RuntimeError(repr(args) + ': ' + result.stdout)
    return result.stdout

def launch(args, **kwargs):
    p = subprocess.Popen(args, **kwargs)
    processes.append(p)
    return p

def lines(p):
    q = queue.Queue()
    def reader():
        for line in p.stdout:
            q.put(line.rstrip())
        q.put(None)
    threading.Thread(target=reader, daemon=True).start()
    return q

def expect(q, prefix, timeout=10):
    while True:
        line = q.get(timeout=timeout)
        if line is None:
            raise RuntimeError('process exited while waiting for ' + prefix)
        if line.startswith(prefix):
            return line

try:
    ns = launch(['unshare', '--net', 'sh', '-c', 'printf ready; exec sleep 120'], stdout=subprocess.PIPE)
    assert ns.stdout.read(5) == b'ready'
    netns = '/proc/' + str(ns.pid) + '/ns/net'
    enter = ['nsenter', '-t', str(ns.pid), '-n', '--']
    for rx, tx, suffix in [('rx', 'tx', '1'), ('other', 'txother', '2')]:
        run('ip', 'link', 'add', rx, 'type', 'veth', 'peer', 'name', tx)
        run('ip', 'link', 'set', rx, 'netns', str(ns.pid))
        run(*enter, 'ip', 'link', 'set', rx, 'up')
        run(*enter, 'ip', '-6', 'addr', 'replace', 'fe80::' + suffix + '/64', 'dev', rx, 'nodad')
        run('ip', 'link', 'set', 'dev', tx, 'up')
        run('ip', '-6', 'addr', 'replace', 'fe80::' + suffix + '0/64', 'dev', tx, 'nodad')
    index = json.loads(run(*enter, 'ip', '-j', 'link', 'show', 'rx'))[0]['ifindex']
    if shutil.which('tcpdump'):
        packet_log = (work / 'packets.log').open('w')
        launch(['tcpdump', '-i', 'tx', '-n', '-vv', '-l', 'udp', 'port', '5353'],
               stdout=packet_log, stderr=subprocess.DEVNULL)
    visible = launch([peer, 'browse', 'tx', '30'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    hidden = launch([peer, 'browse', 'txother', '30'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    vq, hq = lines(visible), lines(hidden)
    expect(vq, 'BROWSING')
    expect(hq, 'BROWSING')
    advert_log = (work / 'advertise.log').open('w')
    advert = launch([peer, 'advertise', netns, str(index), 'fe80::1'],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=advert_log, text=True)
    aq = lines(advert)
    ready = expect(aq, 'READY ')
    child = int(ready.split()[1])
    status = Path('/proc/' + str(child) + '/status').read_text()
    fields = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
    assert fields['Uid'].split() == ['65534'] * 4, fields['Uid']
    assert fields['NoNewPrivs'].strip() == '1'
    assert fields['Seccomp'].strip() == '2'
    assert int(fields['CapEff'].strip(), 16) == 0
    print('PASS: DNS-SD responder is nobody, capability-free, no_new_privs and seccomp filtered', flush=True)
    match = expect(vq, 'FOUND ')
    assert match.endswith('valid=1'), match
    print('PASS: real DNS-SD resolves link-local endpoint with exactly three public TXT fields', flush=True)
    advert.stdin.write('stop\n')
    advert.stdin.flush()
    expect(aq, 'STOPPED')
    assert advert.wait(timeout=5) == 0, (work / 'advertise.log').read_text()
    expect(vq, 'REMOVED')
    print('PASS: stopping the pairing advertisement sends a DNS-SD goodbye', flush=True)
    assert hidden.wait(timeout=35) == 0, hidden.stderr.read()
    hidden_result = expect(hq, 'RESULT ')
    assert 'found=0' in hidden_result and 'invalid=0' in hidden_result, hidden_result
    print('PASS: no advertisement is visible on the unselected interface', flush=True)
    assert visible.wait(timeout=5) == 0, visible.stderr.read()
    print('PASS: scoped discovery lifecycle (no system daemon, no DNS/default route, no pairing secret)', flush=True)
except Exception:
    if (work / 'packets.log').exists():
        print((work / 'packets.log').read_text(), flush=True)
    if (work / 'advertise.log').exists():
        print((work / 'advertise.log').read_text(), flush=True)
    for p in processes:
        print('PROCESS', p.args, 'exit', p.poll(), flush=True)
        if p.poll() is not None and p.stderr:
            print(p.stderr.read(), flush=True)
    raise
finally:
    for p in reversed(processes):
        if p.poll() is None:
            p.terminate()
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=3)
PY
