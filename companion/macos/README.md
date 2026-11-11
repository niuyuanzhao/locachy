# macOS extended-display sender

A small LOCachy companion for macOS. It adds one virtual display to the current login session, so macOS arranges windows onto a real extra monitor while the existing displays stay as they are. It captures only that display with ScreenCaptureKit, encodes H.264 with VideoToolbox, and sends it to the LOCachy receiver over a dedicated wired host-to-host link (Thunderbolt/USB4 IP networking). LOCachy keyboard and touchpad input is applied to that display.

It is not display mirroring, not Screen Sharing/VNC, and not a full macOS application.

## Upstream source and license

The virtual-display code comes from `peetzweg/opendisplay` commit `0f44853adc5d44646a9e4d74e4516db460efb8bb` (release 1.30.2), locked by archive SHA-256 in `upstream.json`. `build.sh` uses four upstream files only (`VirtualDisplay.swift`, `OriginRestore.swift`, `VirtualCanvasSizing.swift`, `CGVirtualDisplayPrivate.h`) and applies `patches/session-display.patch`. The patch makes display mode and origin changes session-scoped and removes upstream's reconfiguration of other displays. The upstream Mac application is not copied.

Upstream is released under the GNU GPL version 3 (its `LICENSE` is pinned by hash in `upstream.json`). The companion's own files are GPL-3.0-or-later like the rest of LOCachy; a built sender combines both and is distributed under the GPL version 3.

`CGVirtualDisplay` is an undocumented CoreGraphics API; macOS has no public API to create a display. `--check` reports whether the private classes exist; a session fails if the display cannot be created. This rules out the Mac App Store; it is a Developer ID/sideload companion.

## Build and checks

Requires Xcode Command Line Tools, `curl`, and `shasum`. The tests also need `python3` with its `ssl` module built against OpenSSL 1.1.1 or newer (the listener and test secrets; Python 3.15 with OpenSSL 3.5 was used).

```sh
companion/macos/build.sh /tmp/locachy-macos-build          # binary only
companion/macos/tests/run-all.sh /tmp/locachy-macos-build  # build + all checks
```

Everything generated stays in the output directory. Neither command installs the sender, creates a display, captures the screen, posts input, or requests a permission; `run-all.sh` executes the sender only with `--check`. It runs the protocol, input, and pairing-code tests (input goes to a recording sink; nothing is posted), then loopback TLS interoperability against an independent listener, then `--check`.

## Running

```sh
locachy-macos-sender --interface <wired-interface> --address <169.254.x.x | fe80::...%interface> --port <port>
```

The sender prompts, with echo off, for the 26-character code shown on the LOCachy screen. If stdin is not a terminal, it reads one line from stdin instead (tests and scripts). The code is never taken from arguments, the environment, a file, the Keychain, or logs.

Before the first run, macOS 14 or later is needed, and the user grants Screen Recording and Accessibility to the companion binary in System Settings. The companion never opens a permission prompt itself. Recent macOS may also require Local Network permission to dial a link-local peer. A bare executable has no Info.plist usage description, so packaging the sender as a signed app bundle is still open.

## Session profile (frozen)

**Transport.** The receiver listens; the sender dials. Only a numeric link-local address on one named wired interface is accepted. Wi-Fi, cellular, and loopback paths are prohibited. There is no DNS, no discovery, and no fallback to another network.

**TLS.**
- TLS 1.2 only, with suite `TLS_PSK_WITH_AES_128_GCM_SHA256` (0x00A8).
- PSK identity `locachy-extdisplay-v1`; ALPN `locachy-extdisplay-v1` is required.
- Session tickets and resumption are disabled. Any certificate presented is rejected.
- Renegotiation is disabled explicitly; Network.framework enables it by default.
- Network.framework ignores the suite restriction for PSK: the ClientHello offers all four of its PSK suites (AES-128/256, GCM and CBC; verified from the listener side). A TLS 1.2 handshake can also complete without the ALPN being agreed. The companion therefore checks the negotiated version, suite, and ALPN the moment the handshake completes, and closes before sending any application data if anything differs. The broker must allow only `PSK-AES128-GCM-SHA256`; any other outcome is a hard failure with no fallback.

**Pairing code.** The LOCachy broker generates 16 random bytes per connection and shows them as RFC 4648 base32: 26 characters, no padding, in groups separated by `-`. Input is case-insensitive, and `-` and whitespace are ignored. Anything that does not decode canonically to exactly 16 bytes is refused. The companion zeroes its own copies after handing the key to the TLS layer and never stores it.

### Why TLS 1.2 PSK, and what it does not protect

Network.framework cannot complete TLS 1.3 with an external PSK. Against an OpenSSL 4.1 TLS 1.3 PSK listener, the client fails locally with `errSSLHandshakeFail` (-9858) before sending a ClientHello, in every configuration tried. TLS 1.2 PSK works. Network.framework offers no ECDHE-PSK suites, so a session has no forward secrecy.

This is accepted here because the key is per-connection, random, shown only on the LOCachy screen, typed by the user, held only in memory, and never persisted. No long-term secret exists whose later compromise would decrypt recorded sessions. TLS 1.3 with pinned certificates was not chosen because the Mac side would have to generate a certificate without a system API.

What it does not protect:
- If the code of a connection leaks (seen on screen, or read from either process's memory) and that connection's traffic was recorded, that connection can be decrypted. An active attacker on the cable who has the code can impersonate either side for that connection.
- The key is handed to Security.framework as a dispatch-data copy whose lifetime the framework controls. The companion cannot zero that copy.
- A memory dump or crash report of the sender during a session may contain the key.

## Protocol

The receiver's contract is authoritative. The sender follows it:
- `hello` arrives first: `pv` 3, `locachy` 1, `extendedOnly` true, the panel, H.264 among `videoCaps`, and all five input capabilities.
- The sender replies `welcome` exactly once, then `streamConfig` (H.264, size, rate up to 60).
- Video is complete Annex-B access units with 4-byte start codes; every IDR carries SPS/PPS.
- Receiver `ping` gets `pong` with the same `t`. `closing` ends the session. Unknown message types are ignored.

Input:
- `key` uses the receiver's logical keysym, resolved against the Mac's current keyboard layout. Super maps to Command, Alt/AltGr to Option, Ctrl to Control. Held state is tracked per device and code. The modifiers and locks in a message are the state before that event; a modifier key's own press shows its modifier and its release clears it.
- `button` covers 272/273/274; `pointer` is a normalized position.
- `scroll` takes deltas in stream pixels with natural sign. `phase` is 0 none, 1 begin, 2 update, 3 end, 4 cancel. Begin, end, and cancel must carry zero deltas and come only from trackpad-like sources (2 finger, 3 TrackPoint). A wheel step (`source` 1) arrives as a phase-2 update with deltas and is posted as a discrete, non-continuous wheel event with no gesture phase; a wheel begin, end, or cancel is refused. `source` is 0 to 4.
- `cancel` (`scope` `all`) releases every held key and button and ends an open scroll gesture.
- Malformed input ends the session before any event is posted.

Version 1 limits: H.264 only; no live resize (a changed `hello` ends the session so a new one is created at the new size); at most 4096 pixels per axis, with the frame rate lowered to stay inside the H.264 level 5.2 macroblock limit; no audio, clipboard, or file transfer.

## Lifecycle

A process owns one virtual display. The session ends on transport end, eight seconds of silence, TLS close, malformed input, `closing`, or a signal. The sender then releases held keys and buttons, ends any open scroll gesture, stops capture, drops the only owner of the virtual display (which removes it), and exits. Other displays are never reconfigured.

## Verification status

Verified here: build; 39 protocol, input, and pairing-code checks; loopback TLS 1.2 PSK interoperability against an independent listener that enforces the identity and key in its PSK callback and records the negotiated identity, suite, version, ALPN, and the received framed message, plus refusal of a wrong code, a wrong expected identity, a wrong suite, a missing ALPN, and a TLS 1.3-only peer; `--check`.

Not verified: creating the virtual display, capture, and input injection in a real Mac session; Screen Recording, Accessibility, and Local Network prompts; a Thunderbolt/USB4 cable; the real LOCachy receiver and broker. Loopback interoperability is not a cable run.

Reference listener (identity-enforcing, `tests/tls-listener.py`, Python `ssl`):

```sh
python3 -I tests/tls-listener.py PORT <32 hex digits> locachy-extdisplay-v1 locachy-extdisplay-v1 PSK-AES128-GCM-SHA256 1.2 LOG
```

The equivalent OpenSSL command also completes the handshake, but it is not an identity test: with TLS 1.2, `s_server -psk_identity` accepted a client presenting a different identity. A broker built on OpenSSL must check the identity in its PSK server callback and return no key for anything else.

```sh
openssl s_server -accept 127.0.0.1:PORT -tls1_2 -cipher PSK-AES128-GCM-SHA256 \
  -psk <32 hex digits> -psk_identity locachy-extdisplay-v1 -nocert -alpn locachy-extdisplay-v1 -msg
```
