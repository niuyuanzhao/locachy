#!/bin/bash
# test-timezone-client.sh [DIR]: timezone-client (service_timezone), as any
# user, offline: ip, curl and sleep are the test's (ip netns exec runs the
# rest here, curl answers as ipinfo.io would, sleep ends a pass of the
# service); rc.conf is the shipped one with the layers added; the zones and
# /etc/localtime are in DIR. The zone is Asia/Tokyo before each run.
#   1. direct: asked of ipinfo.io in locachy-net, /etc/localtime its answer
#   2. full-system Tor: nothing asked, the zone kept; Clash in each mode (its
#      TUN by default or named, its port only): asked, as clash-client sends
#      ipinfo.io direct
#   3. Tor turned off (the route the same): asked then; Tor turned on while
#      it is asked: the answer is not taken
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-/var/tmp/locachy-timezone-test}
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
rm -rf "$out"
mkdir -p "$out/bin" "$out/zoneinfo/Asia" "$out/zoneinfo/Europe" "$out/zoneinfo/America"
out=$(cd "$out" && pwd)
: >"$out/zoneinfo/Asia/Tokyo"
: >"$out/zoneinfo/Europe/Berlin"
: >"$out/zoneinfo/America/New_York"
echo "default via 192.0.2.1 dev eth0 proto dhcp src 192.0.2.10 metric 100" >"$out/route"
cat >"$out/bin/ip" <<'EOF'
#!/bin/sh
# ip netns exec NS COMMAND...: COMMAND, here; anything else: the route.
echo "ip $*" >>"$T/calls"
if [ "$1 $2" = "netns exec" ]; then
    shift 3
    exec "$@"
fi
cat "$T/route"
EOF
cat >"$out/bin/curl" <<'EOF'
#!/bin/sh
# ipinfo.io's answer (T/answer); T/during: rc.conf's lines added meanwhile.
echo "curl $*" >>"$T/calls"
[ ! -f "$T/during" ] || cat "$T/during" >>"$T/rc.conf"
cat "$T/answer"
EOF
cat >"$out/bin/sleep" <<'EOF'
#!/bin/sh
# The end of pass N: rc.conf becomes T/rc.N for the next one; without it,
# the service is ended.
n=$(($(cat "$T/passes") + 1))
echo "$n" >"$T/passes"
if [ -f "$T/rc.$n" ]; then
    mv "$T/rc.$n" "$T/rc.conf"
else
    kill "$PPID"
fi
EOF
chmod +x "$out/bin/ip" "$out/bin/curl" "$out/bin/sleep"

# run ANSWER LINES [NEXT_LINES]: the service, ipinfo.io answering ANSWER,
# rc.conf the shipped one with LINES, for a second pass with NEXT_LINES;
# DURING (if set): lines added to it while ipinfo.io is asked.
run() {
    rm -f "$out/calls" "$out/during" "$out/rc."*
    echo 0 >"$out/passes"
    echo "$1" >"$out/answer"
    { cat "$src/rc.conf"; printf '%s' "$2"; } >"$out/rc.conf"
    [ $# -lt 3 ] || { cat "$src/rc.conf"; printf '%s' "$3"; } >"$out/rc.1"
    [ -z "${DURING:-}" ] || printf '%s' "$DURING" >"$out/during"
    : >"$out/calls"
    ln -sfn "$out/zoneinfo/Asia/Tokyo" "$out/localtime"
    # Its output in T/log; the shell's word of it being ended, nowhere.
    (T=$out PATH=$out/bin:$PATH LOCACHY_TIMEZONE_RC=$out/rc.conf LOCACHY_TIMEZONE_ZONES=$out/zoneinfo \
        LOCACHY_TIMEZONE_LOCALTIME=$out/localtime timeout 20 bash "$src/link/timezone-client" >"$out/log" 2>&1 ||
        :) 2>/dev/null
}
zone() { [ "$(readlink "$out/localtime")" = "$out/zoneinfo/$1" ]; }
asked() { [ "$(grep -c '^curl ' "$out/calls")" = "$1" ]; }

TOR=$'service_tor="/usr/local/lib/locachy/tor-client"\ntor_front="bridges"\n'
CLASH=$'service_clash="/usr/local/lib/locachy/clash-client"\n'

run Europe/Berlin ""
check "direct (the shipped rc.conf): asked of ipinfo.io in locachy-net" \
    "asked 1 && grep -q '^ip netns exec locachy-net curl -fsS --max-time 10 https://ipinfo.io/timezone' $out/calls"
check "direct: /etc/localtime is its answer, Europe/Berlin" "zone Europe/Berlin && grep -q 'timezone-client: Europe/Berlin' $out/log"

run America/New_York "$TOR" "$TOR"
check "Tor: nothing asked in two passes, Asia/Tokyo kept, said once" \
    "asked 0 && zone Asia/Tokyo && [ \"\$(grep -c 'Tor carries the network' $out/log)\" = 1 ]"
run Europe/Berlin "$CLASH"
check "Clash's TUN (no clash_mode): asked, Europe/Berlin" "asked 1 && zone Europe/Berlin && ! grep -q carries $out/log"
run Europe/Berlin "$CLASH"$'clash_mode="tun"\n'
check "Clash's TUN (clash_mode tun): asked, Europe/Berlin" "asked 1 && zone Europe/Berlin"
run Europe/Berlin "$CLASH"$'clash_mode="proxy"\n'
check "Clash's port only: asked, Europe/Berlin" "asked 1 && zone Europe/Berlin"
run America/New_York "$CLASH$TOR"
check "Tor with Clash on: nothing asked, Asia/Tokyo kept" "asked 0 && zone Asia/Tokyo"

run Europe/Berlin "$TOR" ""
check "Tor turned off, the route the same: asked then, Europe/Berlin" \
    "asked 1 && zone Europe/Berlin && [ \"\$(cat $out/passes)\" = 2 ]"
DURING=$TOR run America/New_York ""
check "Tor turned on while asked: its answer not taken, Asia/Tokyo kept" "asked 1 && zone Asia/Tokyo"

[ "$fail" = 0 ] && echo "PASS timezone-client: asked unless Tor carries the network" ||
    echo "FAIL timezone-client (files in $out)"
exit "$fail"
