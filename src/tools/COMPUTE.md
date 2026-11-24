# LOCachy Compute Node

`service_compute` turns a LOCachy machine into a headless compute node: it runs
Linux programs in native containers built from any distribution's root
filesystem, optionally in KVM micro-VMs, with GPU and data-disk access, and is
driven over an authenticated HTTPS control plane. It has no Wayland, compositor,
X11, seat, or session-bus dependency, and the desktop never freezes or stops it.

## Components

| File | Role |
|---|---|
| `link/locachy-compute` | CLI and daemon: images, environments, native containers, jobs, KVM micro-VMs, `serve`, and `remote` (the client). |
| `link/compute-client` | `service_compute` launcher. Runs `locachy-compute serve` with `nsenter --net` into `locachy-net`. |
| `link/locachy-pair` | Once-forever pairing: add, list, show, revoke, and token export for paired devices. |
| `link/locachy_peers.py` | The shared peer store (`/etc/locachy/peers`), used by `locachy-pair`, `locachy-compute`, and `extdisplay-broker`. |
| `link/network-client` | Inbound filter for `locachy-net`, including the Thunderbolt allow-lists (`tb_ifaces`, `tb_tcp_ports`, `tb_udp_ports`). |
| `progs/extdisplay-broker*.c`, `progs/extdisplay-advertise.c` | Extended-display broker: persistent peer PSKs, paired-only mode, and shared-link mode. |

## Native containers

`locachy-compute run ENV -- COMMAND` starts a job like this:

1. The base image is a read-only directory (`/var/lib/locachy/compute/images/NAME/rootfs`), imported from a directory, a `.tar`, `.tar.gz`, `.tar.xz`, or `.tar.zst`, or a Docker/OCI save archive. Nothing is written to it.
2. The environment (`envs/NAME/`) holds the writable OverlayFS `upper/` and `work/` layers.
3. `unshare --mount --pid --fork --kill-child` creates the namespaces. The overlay is mounted with the image as `lowerdir`.
4. `/proc`, `/sys` (read-only), a minimal `/dev`, and `/tmp` and `/run` as tmpfs are mounted. Configured host paths are bind-mounted.
5. The process calls `pivot_root` and detaches the old root with `umount2`, all in-process. The guest image needs no `umount`, `rmdir`, or matching C library.
6. PID 1 is the static `/usr/local/lib/locachy/app-init` when it exists (it reaps children and forwards signals). Otherwise a built-in reaper does the same job without anything from the guest.

Wayland, X11, DBus, and seat variables are cleared, and the sockets are never mounted.

## Reset and data

- `locachy-compute env reset NAME` stops the environment's jobs and deletes `upper/` and `work/`. The base image and every mounted dataset stay intact, so the reset takes milliseconds.
- Host data is mounted with `--mount HOST:CONTAINER[:ro|:rw]`, or set on the environment with `env create --mount`. Any absolute, existing path is allowed (for example `/mnt/nvme/...`, `/data/...`, or `/var/lib/locachy/compute/data/...`), except these roots: `/`, `/proc`, `/sys`, `/dev`, the peer store, `/var/lib/locachy/compute` (apart from its `data/` area), `/run/locachy/compute`, and the extended-display broker state.
- Remote peers cannot choose mounts. A job submitted over the network can run only inside an environment the node operator configured.

## GPU

When the environment is created with `--gpu` (and `compute_gpu` is not `"0"`), the container receives `/dev/nvidiactl`, `/dev/nvidia-modeset`, `/dev/nvidia-uvm*`, `/dev/nvidiaN`, `/dev/nvidia-caps/*`, and `/dev/dri/renderD*`, plus read-only bind mounts of the host NVIDIA user-space libraries (`libcuda.so*`, `libnvidia-*.so*`, `libnvcuvid.so*`, `libnvoptix.so*`) in `/usr/local/lib/locachy-gpu`. `locachy-compute gpu-probe` lists what the host has. Devices that are absent are skipped.

## KVM micro-VMs

`locachy-compute vm create NAME --disk BASE.qcow2 [--share DIR:TAG] [--cloud-init SEED.img]` creates a copy-on-write overlay (`qemu-img create -b BASE -F qcow2`). `vm start` runs `qemu-system-x86_64` with `-enable-kvm` when `/dev/kvm` exists (TCG otherwise): a virtio disk, a serial console socket, a QEMU monitor socket, a virtio-9p share per `--share`, and optional cloud-init. `vm reset` throws the overlay away. Each VM runs in its own cgroup, `/sys/fs/cgroup/locachy-compute/vm-NAME`.

## Cgroups

Everything lives under `/sys/fs/cgroup/locachy-compute/`, separate from the desktop's `app_*` groups:

- `service` for the control daemon
- `job-ID` for each container job (optional `memory.max` and `cpu.max`)
- `vm-NAME` for each micro-VM

Creating a cgroup is required. If it fails, the job does not run. Only the test harness sets `LOCACHY_COMPUTE_ALLOW_NO_CGROUP=1`.

## Pairing

Pairing is off by default. No device is trusted until you add it.

```sh
locachy-pair init
locachy-pair add --name "Work Mac" --scope extdisplay,compute   # asks once on the terminal (--yes for scripts); prints a one-time token
locachy-pair list
locachy-pair revoke "Work Mac"
```

- The store is `/etc/locachy/peers/` (directory `0700`, files `0600`). `host.json` holds the stable `host_id`. `host-cert.pem` and `host-key.pem` hold the node's ECDSA P-256 identity (self-signed, 20 years). Each `peers/PEER_ID.json` holds one paired device: its scopes, its extdisplay PSK (16 bytes), and, for `compute`, the certificate it presents.
- `add` prints a `locachy-pair-v1:` token once. It contains the client's certificate and private key (for `compute`), so copy it to the client once. The node does not keep the private key.
- A paired device stays trusted until `revoke` removes its file. Nothing re-pairs automatically. Revocation takes effect at the next connection: the compute server rebuilds its trust store when the store changes, and the extended-display broker reads the store on each attempt.
- Scopes are separate. An `extdisplay`-only device is refused by the compute API, and a `compute`-only device cannot open an extended-display session.

## Control plane

`locachy-compute serve` (started by `service_compute`) listens on HTTPS, port 27184, with TLS 1.3 and mandatory client certificates:

- The server trusts only certificates of paired devices with scope `compute`. With none paired, every handshake fails.
- It binds only to the addresses of `tailscale0`, and, when `compute_bind_tb="1"`, to the IPv6 link-local addresses of `thunderbolt-net` interfaces. It never binds `0.0.0.0` or `::`.
- The client pins the server's certificate by fingerprint (`host_cert_sha256` in the client's `peer.json`, checked against the connection's certificate).

Endpoints: `GET /v1/health`, `GET /v1/envs`, `POST /v1/envs/NAME/reset`, `GET /v1/jobs`, `POST /v1/jobs`, `GET /v1/jobs/ID`, `GET /v1/jobs/ID/log`, `POST /v1/jobs/ID/stop`, `GET /v1/vms`, `POST /v1/vms/NAME/start`, `POST /v1/vms/NAME/stop`, and `POST /v1/vms/NAME/reset`.

From another machine:

```sh
locachy-compute remote --creds-dir ./work-mac --host 100.x.y.z health
locachy-compute remote --creds-dir ./work-mac --host 'fe80::1%tb0' run ENV -- python3 train.py
```

## Network: Tailscale and Thunderbolt together

The compute node runs inside `locachy-net`. `compute-client` enters the namespace with `nsenter --net=/run/netns/locachy-net`, not `ip netns exec`. `ip netns exec` mounts a new `/sys` and hides `/sys/fs/cgroup`. `nsenter` changes only the network namespace, so the daemon keeps both the namespace's sockets and the host cgroup tree.

Thunderbolt/USB4 host-to-host links are shared, not handed over:

- With `extdisplay_tb_shared="1"`, or with `service_compute` and `compute_bind_tb="1"`, the extended-display broker leaves the `thunderbolt-net` interface in `locachy-net`. Its TLS pump is still confined to the link-local address and `SO_BINDTOIFINDEX` of that interface, with Landlock and seccomp.
- `network-client` keeps the inbound policy at drop. Link-local IPv6 traffic on a verified `thunderbolt-net` interface is accepted only for TCP 27183 (extended display), TCP 27184 (compute), and UDP 5353 (mDNS). Wi-Fi and Ethernet are never added to the allow-lists.
- With `extdisplay_tb_shared="0"` (high-isolation cable mode), the broker keeps the old behaviour: the interface moves into a private session namespace. Compute loses that link while the extended display is open.

## rc.conf keys

| Key | Meaning |
|---|---|
| `service_compute="/usr/local/lib/locachy/compute-client"` | Runs the compute node. |
| `compute_port="27184"` | Control-plane port (default 27184). |
| `compute_bind_tb="1"` | Also listen on Thunderbolt link-local addresses. |
| `compute_gpu="0"` | Never pass GPUs to containers. |
| `extdisplay_tb_shared="1"` or `"0"` | Shared link (default when compute binds Thunderbolt) or private session namespace. |
| `extdisplay_paired_only="1"` | Accept only paired devices; no cleartext code is advertised or accepted. |

## Tests

```sh
src/tools/test-peers.sh        # peer store, pairing permissions, broker PSK, shared Thunderbolt link, shipped filter
src/tools/test-compute.sh      # native containers, reset, mounts, policy, micro-VM overlay, mTLS authentication
src/tools/test-extdisplay-broker.sh
```

Both new tests run in unprivileged user, network, and mount namespaces, and install nothing. Test seams are environment variables with `LOCACHY_` prefixes (`LOCACHY_PEERS_DIR`, `LOCACHY_COMPUTE_BASE`, `LOCACHY_COMPUTE_RUN`, `LOCACHY_COMPUTE_CGROUP`, `LOCACHY_COMPUTE_APP_INIT`, `LOCACHY_COMPUTE_NETNS`, `LOCACHY_COMPUTE_ALLOW_NO_CGROUP`). The extended-display broker's test fixtures are compile-time macros.

## Limits

- A static `app-init` is required to run as PID 1 inside foreign root filesystems. A host-built dynamic one is not used there, and the built-in reaper takes over.
- Revoking a device does not end connections it already has open. Each request uses its own connection, so these end quickly.
- GPU passthrough is verified by the device-node and library probe. The NVIDIA user-space stack itself was not exercised on hardware in these tests.
- Thunderbolt link discovery, and the nftables allow-list being filled by `sync_tb_filter`, need a real `thunderbolt-net` device to exercise. The tests use a veth pair with the same rules.
