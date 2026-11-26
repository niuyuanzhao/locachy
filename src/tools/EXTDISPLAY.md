# LOCachy Extended Display

`extdisplay` turns a LOCachy computer into an extended display for another computer over a Thunderbolt/USB4 link. The sender creates a virtual display, streams it to LOCachy, and forwards LOCachy keyboard, trackpad, mouse, multi-touch touchscreen, and stylus pen input back to the sender.

## Components

| File | Role |
|---|---|
| `progs/extdisplay.c` | Frontend: screen output (non-blocking DRM page-flip + multi-plane dma-buf), keyboard, pointer, multi-touch, and tablet pen forwarding, session lifecycle. |
| `progs/extdisplay-worker.c` | Sandboxed worker (`nobody`, Landlock, seccomp): framing, JSON, and video decoding. |
| `progs/extdisplay-protocol.[ch]` | Framing, capability negotiation (`h264`/`hevc420`/`hevc444`/`hevc`, `touch`/`tablet`), and control/input messages. |
| `progs/extdisplay-decode.[ch]` | FFmpeg (`libavcodec`, `libavutil`, `libswscale`) H.264/HEVC decoding, VA-API hardware capability probing, and multi-plane VPP dma-buf export. |
| `progs/extdisplay-ipc.h`, `progs/extdisplay-client.c` | `SOCK_SEQPACKET` IPC with sealed read-only memfd frames. |
| `progs/extdisplay-setup.[ch]` | Pairing and link status client. |
| `progs/extdisplay-broker.c`, `extdisplay-broker-net.c`, `extdisplay-broker-pump.c`, `extdisplay-broker-tls.c`, `extdisplay-broker.h`, `extdisplay-broker-protocol.h` | Broker: Thunderbolt interface lease in an isolated network namespace, discovery helper, and TLS 1.2 PSK listener. |
| `progs/extdisplay-advertise.c`, `extdisplay-discovery.[ch]` | Interface-scoped mDNS/DNS-SD discovery publisher on the leased Thunderbolt link. |
| `base/include/input-raw.h`, `base/input-raw.c` | Raw keyboard event queue. |

## Installation and Hardware Acceleration

Enable **Extended display (extdisplay)** in **Settings → System → Optional Components** on LOCachy (requires FFmpeg, json-c, Avahi, and OpenSSL).

- On Linux, `extdisplay-worker` queries VA-API VLD entrypoints (`libva` + `libva-drm`) over a pre-opened `/dev/dri/renderD*` descriptor before entering its Landlock + seccomp sandbox, and presents decoded frames via `VA-API VPP` → multi-plane `DRM_PRIME_2` zero-copy scanout using non-blocking `DRM_IOCTL_MODE_PAGE_FLIP` (falling back to synchronous `SETCRTC` with a single startup log if page-flip is refused). `hello.videoCaps` advertises exact hardware profiles (`h264`, `hevc420` when `VAProfileHEVCMain` VLD is present, `hevc444` and legacy `hevc` only when `VAProfileHEVCMain444` VLD is present), so GPUs without 4:4:4 hardware decode use hardware 4:2:0 (`hevc420` / `h264`) rather than falling back to slow CPU 4:4:4 decoding. On `nouveau`, `nvidia-drm`, `simpledrm`, or VMs without VA-API, it uses sandboxed CPU decoding and sealed `MADV_HUGEPAGE` `memfd` frames.
- During installation, `extdisplay` setup, and updates, `locachy-pkg` automatically scans `/sys/class/drm` and PCI display controllers to install the matching VA-API/Vulkan drivers for each machine's GPU (`intel-media-driver` on Intel Gen8+, `libva-intel-driver` on Intel Gen4–7.5, `mesa` on AMD/Nouveau, `libva-nvidia-driver` on proprietary NVIDIA, and per-GPU packages on dual-GPU Optimus/MX laptops).
- **Settings → Input & Output → Extended display video** (`extdisplay_decode` in `rc.conf`: `"auto"`, `"hw"`, or `"sw"`) lets you choose between automatic highest hardware-decoded codec selection, hardware-only preference, or CPU-only sandboxed decoding.

On macOS, install the companion sender service from the `niuyuanzhao/locachy` Homebrew tap:

```sh
brew tap niuyuanzhao/locachy
brew install locachy-macos-sender
brew services start locachy-macos-sender
```

## Connection and Display Negotiation

Thunderbolt/USB4 networking works over a direct cable or through a compatible Thunderbolt/USB4 hub, dock, or daisy chain.

When `extdisplay` is opened on LOCachy:

1. `extdisplay-broker` moves the verified `thunderbolt-net` interface into an isolated network namespace, configures link-local IPv6, and starts `extdisplay-advertise` to publish `_locachy-ext._tcp.local.` with a fresh per-session 128-bit PSK on that link.
2. The macOS background service discovers the advertisement on the Thunderbolt link and dials `TLS_PSK_WITH_AES_128_GCM_SHA256` (identity and ALPN `locachy-extdisplay-v1`) automatically.
3. The receiver sends `hello` with its physical panel dimensions (`pixelsWide × pixelsHigh`), scale, sanitized `host` and `model` identifiers, exact hardware-backed video codecs (`h264`, `hevc420`, `hevc444`, `hevc`), and input capabilities (negotiating `"touch"` and `"tablet"` alongside the five core input capabilities).
4. From the receiver's reported panel dimensions, the sender automatically computes HiDPI scaling presets and synthetic physical geometry for any mainstream aspect ratio and panel resolution (exposing 5 proportional steps in System Settings → Displays **Larger Text ↔ More Space**, with 1:1 HiDPI and 1:1 native modes), and persists each machine's choice by `(host, model, pixelsWide × pixelsHigh)` in `~/Library/Application Support/LOCachy/display-modes.json`. At `1:1` modes, frames stream untouched; at scaled modes, the sender captures at backing resolution and applies Metal Mitchell bicubic downscaling (`B=1/3, C=1/3`) before streaming at the panel's physical resolution.

## Session Behavior

While the link is active:

- All keyboard input (including `Super+Esc`, `Ctrl+Q`, `Esc`, `q`, and `Ctrl+Alt+F12`), trackpad gestures, and mouse movements are forwarded to the sender.
- When the sender accepts `"touch"` and `"tablet"`, direct touchscreen multi-touch frames (1-finger tap/drag/long-press right-click, 2-finger pan/scroll, 2-finger pinch zoom) and active stylus pen events (proximity in/out, pen/eraser switching, sub-pixel position, pressure, X/Y tilt, hover distance, rotation, barrel buttons, with stylus-proximity palm rejection) map strictly onto the extended display's bounds while relative mice/touchpads continue roaming across all displays.
- `Super+Esc` (Home) and three-finger home/switch swipes do not switch away from the live extended display; `Super+L` (Lock) still locks locally while keeping the link alive (`base_session_hold`).
- Idle auto-lock is deferred while the live extended display is shown, and takes effect immediately once the cable is unplugged or the session closes.
- When the cable is unplugged or the sender restarts, the session ends and `extdisplay` returns to the waiting state, reconnecting automatically when a cable link returns without rebooting or relaunching. While waiting, `Esc` cancels pairing, `Super+Esc` goes home, and `Ctrl+Q` quits.

## Tests

```sh
src/tools/test-extdisplay.sh
src/tools/test-extdisplay-input.sh
src/tools/test-extdisplay-discovery.sh
src/tools/test-extdisplay-broker.sh
```
