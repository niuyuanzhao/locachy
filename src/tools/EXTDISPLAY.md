# LOCachy Extended Display

`extdisplay` turns a LOCachy computer into an extended display for another computer over a Thunderbolt/USB4 link. The sender creates a virtual display, streams it to LOCachy, and forwards LOCachy keyboard, trackpad, and mouse input back to the sender.

## Components

| File | Role |
|---|---|
| `progs/extdisplay.c` | Frontend: screen output, keyboard and pointer forwarding, session lifecycle. |
| `progs/extdisplay-worker.c` | Sandboxed worker (`nobody`, Landlock, seccomp): framing, JSON, and video decoding. |
| `progs/extdisplay-protocol.[ch]` | Framing and control/input messages. |
| `progs/extdisplay-decode.[ch]` | FFmpeg (`libavcodec`, `libavutil`, `libswscale`) H.264/HEVC decoding and color conversion. |
| `progs/extdisplay-ipc.h`, `progs/extdisplay-client.c` | `SOCK_SEQPACKET` IPC with sealed read-only memfd frames. |
| `progs/extdisplay-setup.[ch]` | Pairing and link status client. |
| `progs/extdisplay-broker.c`, `extdisplay-broker-net.c`, `extdisplay-broker-pump.c`, `extdisplay-broker-tls.c`, `extdisplay-broker.h`, `extdisplay-broker-protocol.h` | Broker: Thunderbolt interface lease in an isolated network namespace, discovery helper, and TLS 1.2 PSK listener. |
| `progs/extdisplay-advertise.c`, `extdisplay-discovery.[ch]` | Interface-scoped mDNS/DNS-SD discovery publisher on the leased Thunderbolt link. |
| `base/include/input-raw.h`, `base/input-raw.c` | Raw keyboard event queue. |

## Installation and Hardware Acceleration

Enable **Extended display (extdisplay)** in **Settings → System → Optional Components** on LOCachy (requires FFmpeg, json-c, Avahi, and OpenSSL).

- On Linux, `extdisplay-worker` automatically uses FFmpeg VA-API (`libva` + `libva-drm`) and zero-copy `VA-API VPP` → `DRM_PRIME_2` scanout over a pre-opened `/dev/dri/renderD*` descriptor (when the active display DRM driver is `i915`, `xe`, `amdgpu`, or `radeon`) initialized before entering its Landlock + seccomp sandbox. On `nouveau`, `nvidia-drm`, `simpledrm`, or when a GPU lacks hardware `4:4:4` decoding, it falls back seamlessly to CPU AVX2/SSE2 decoding and sealed `MADV_HUGEPAGE` `memfd` frames.
- During installation, `extdisplay` setup, and updates, `locachy-pkg` automatically scans `/sys/class/drm` and PCI display controllers to install the matching VA-API/Vulkan drivers for each machine's GPU (`intel-media-driver` on Intel Gen8+, `libva-intel-driver` on Intel Gen4–7.5, `mesa` on AMD/Nouveau, `libva-nvidia-driver` on proprietary NVIDIA, and per-GPU packages on dual-GPU Optimus/MX laptops).
- **Settings → Input & Output → Extended display video** (`extdisplay_decode` in `rc.conf`: `"auto"`, `"hw"`, or `"sw"`) lets you choose between automatic GPU+4:4:4, forcing GPU H.264 on older GPUs without 4:4:4 HW decode, or CPU-only sandboxed decoding.

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
3. The receiver sends `hello` with its physical panel dimensions (`pixelsWide × pixelsHigh`), scale, sanitized `host` and `model` identifiers, supported codecs (`h264`, `hevc`), and input capabilities.
4. From the receiver's reported panel dimensions, the sender computes the 1:1 base mode (`pixels ÷ 2` points at `@2x` HiDPI when divisible into even points for high-density panels, otherwise `1×`), registers five proportional scaling steps (`2.0×`, `1.75×`, `1.5×`, `1.25×`, `1.0×`) in System Settings → Displays (**Larger Text ↔ More Space**), and looks up `(host, model, pixelsWide × pixelsHigh)` in its display-mode database (`~/Library/Application Support/LOCachy/display-modes.json`) so each machine keeps its chosen scaling mode across reconnects (defaulting to `1:1` when unseen). At `1:1` modes, frames stream untouched; at intermediate HiDPI scaling steps, the sender captures natively and applies Metal Mitchell bicubic downscaling (`B=1/3, C=1/3`) before streaming at the panel's physical resolution.

## Session Behavior

While the link is active:

- All keyboard input (including `Super+Esc`, `Ctrl+Q`, `Esc`, `q`, and `Ctrl+Alt+F12`), trackpad gestures, and pointer movements are forwarded to the sender.
- `Super+Esc` (Home) and three-finger home/switch swipes do not switch away from the live extended display; `Super+L` (Lock) still locks locally while keeping the link alive (`base_session_hold`).
- Idle auto-lock is deferred while the live extended display is shown, and takes effect immediately once the cable is unplugged or the session closes.
- When the cable is unplugged, the session ends and the next key closes `extdisplay`.

## Tests

```sh
src/tools/test-extdisplay.sh
src/tools/test-extdisplay-discovery.sh
src/tools/test-extdisplay-broker.sh
```
