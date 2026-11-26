# LOCachy Compute Node

[中文](COMPUTE.zh.md)

For NVIDIA experiments, use **regular OpenSSH over Tailscale**: `ssh` for the
node's `locachy-compute` CLI, and `scp` or `sftp` for files. Use a native Linux
container for NVIDIA access; a KVM micro-VM is an alternative when the workload
needs its own kernel. Headless compute does not depend on a graphical session or
follow the desktop's freeze/stop lifecycle. HTTPS with mutual TLS (mTLS) is an
optional API. Thunderbolt/USB4 is a separate, optional host-to-host path; it is
not required for NVIDIA experiments.

## NVIDIA experiments over SSH

### Access and trust

1. Join the node and client to the intended tailnet. In its [network access
   policy](https://tailscale.com/kb/1324/grants), grant only the intended
   operators/devices `tcp:22` to the node and check that no broader existing rule
   allows it. Keep Tailscale SSH disabled on the node: this is ordinary OpenSSH,
   and a Tailscale `ssh` rule does not enroll an OpenSSH user key.
2. In Settings (**Network → SSH Server (sshd)**), choose **Tailscale only**
   (or **LAN & Tailscale** when LAN access is wanted). The opt-in
   `service_tailnet_ssh` runs OpenSSH inside `locachy-net` in its own cgroup
   (`locachy-tailnet-ssh`), following the addresses of `tailscale0` (and, in
   `lan` mode, global addresses of active non-`lo`, non-`mihomo`,
   non-`thunderbolt-net` interfaces). Per-interface nftables rules drop traffic
   arriving on any other interface (including loopback), and in `lan` mode only
   the exact bound `(interface, address, port)` tuples are opened in
   `locachy-filter`. By default it listens on port 22 (`ssh_port`) and requires
   public-key authentication; password login is optional (`ssh_password="1"`,
   off by default), while root login and empty passwords are always forbidden.
   Local TCP forwarding (`AllowTcpForwarding local`) is enabled for `ProxyJump`
   and `-L` tunnels; remote forwarding (`-R`), agent forwarding, X11, tunnels,
   and user environments stay disabled, and internal SFTP (also used by current
   `scp`) is enabled.
3. Enroll operator **public** keys for the local account either in Settings
   (**Network → SSH Server (sshd) → sshd AuthorizedKeys**) or on the node
   console as an administrator (replace `USER` and the source filename):

   ```sh
   sudo /usr/local/lib/locachy/tailnet-ssh-client --keys-add USER < /path/to/operator.pub
   # or replace the whole file directly:
   sudo install -o root -g root -m 0644 /path/to/operator.pub \
     /etc/locachy/tailnet-ssh/authorized_keys/USER
   ```

   Only root may write `/etc/locachy/tailnet-ssh/authorized_keys/USER`; the
   unprivileged key lookup needs to read it. Remove keys via Settings or
   `--keys-remove USER INDEX` (or delete the file) to deny future logins. The
   service never reads the user's `~/.ssh/authorized_keys`. Keep the private
   key on the client.
4. First start creates the node's persistent Ed25519 host key if absent. It
   does not replace an existing key unless you explicitly regenerate it in
   Settings (**sshd host key**) or with `--host-key-regen` (in which case
   clients must remove the old entry from `known_hosts`). Verify the SHA-256
   fingerprint before accepting it on the client:

   ```sh
   sudo /usr/local/lib/locachy/tailnet-ssh-client --host-key-fingerprint
   ```

   Tailnet access, an enrolled SSH user key, the node's SSH host key, mTLS
   credentials, and the extended-display PSK are independent.
5. Log in as the normal LOCachy user (uid 1000), not root. File transfers use
   that account's ordinary permissions. Use its existing password-based `sudo`
   for privileged compute operations; SSH key access does not grant passwordless
   `sudo`. The examples use an interactive SSH shell (`ssh -t` if invoking
   `sudo` as a remote command). Do not send the sudo password in a script or
   CLI argument.

The host key is generated on the running node, not in the source or the ISO.

In the examples, `locachy-gpu` is a client SSH config alias for the normal user
and the node's tailnet IP or MagicDNS name. `service_compute`, `locachy-pair`,
and a Thunderbolt cable are not needed for this workflow.

### Prepare storage, run, and retrieve

Use a mounted `compute` or `shared` volume with room for the image, overlay,
datasets, model weights, checkpoints, results, and logs. In this example the
volume is named `experiments`. Confirm the mount before placing files there;
`scp` and `sftp` do not themselves check whether a path is on the intended disk.
Keep the SSH user's staging directories outside `locachy-compute/`, which holds
managed state, and bind only the directories the workload needs.

From an SSH shell on the node, run `findmnt --mountpoint /srv/storage/experiments`.
Stop if it does not show the intended mounted volume. Then configure storage
and prepare directories writable by the SSH user:

```sh
sudo locachy-storage set-compute-storage experiments
sudo locachy-compute storage
sudo install -d -m 0700 -o "$(id -u)" -g "$(id -g)" \
  /srv/storage/experiments/work \
  /srv/storage/experiments/work/probe-001/in \
  /srv/storage/experiments/work/probe-001/out
```

For this example, use a trusted Linux rootfs archive with Python and a
CUDA-enabled PyTorch build compatible with the node's NVIDIA driver. Save
this probe as `probe.py` on the client:

```python
from pathlib import Path

import torch

x = torch.arange(1, 5, dtype=torch.float32, device="cuda").reshape(2, 2)
y = x @ x
torch.cuda.synchronize()
result = y.cpu().tolist()
assert y.is_cuda and result == [[7.0, 10.0], [15.0, 22.0]]
text = f"{torch.cuda.get_device_name(y.device)}: {result}\n"
print(text, end="")
output = Path("/output/probe.txt")
output.write_text(text, encoding="utf-8")
output.chmod(0o644)
```

```sh
scp probe.py locachy-gpu:/srv/storage/experiments/work/probe-001/in/
scp torch-rootfs.tar.zst locachy-gpu:/srv/storage/experiments/work/
ssh locachy-gpu
```

On the node, import the rootfs and create the environment:

```sh
sudo locachy-compute image import torch-rootfs /srv/storage/experiments/work/torch-rootfs.tar.zst
sudo locachy-compute env create torch-probe --image torch-rootfs --gpu --net none \
  --mount /srv/storage/experiments/work/probe-001/in:/input:ro \
  --mount /srv/storage/experiments/work/probe-001/out:/output:rw
```

`--net none` is sufficient because this example stages all inputs first. On the
node, submit the job and save the returned `job_id`. Use a new job ID and
output directory for each run so old logs or results cannot be mistaken for new
ones. Disconnect after submission and reconnect to check it:

```sh
sudo locachy-compute run torch-probe --job-id probe-001 --detach -- python3 /input/probe.py
sudo locachy-compute job list
sudo locachy-compute job status probe-001
sudo locachy-compute job log probe-001
```

To cancel a running job, use `sudo locachy-compute job stop JOB_ID`, then check
its state. A detached job must belong to its own compute cgroup and survive
logout, but not a node shutdown. The submission response is not the workload
result: require a recorded terminal state with the workload exit code on normal
completion or failure, captured stdout/stderr, and no remaining workload
processes after cancellation. A bare `exited` record with no exit code is not a
pass. Host file permissions must let the SSH user retrieve results without a
privileged file transfer; the example writes a readable file in the user's
private output directory.

After a successful job, fetch the result from the client (or use `sftp`):

```sh
scp locachy-gpu:/srv/storage/experiments/work/probe-001/out/probe.txt .
```

## Components

| File | Role |
|---|---|
| `link/locachy-compute` | CLI and daemon: images, environments, native containers, jobs, KVM micro-VMs, `serve`, and `remote` (the client). |
| `link/tailnet-ssh-client` | Managed OpenSSH server (`sshd`) in `locachy-net`: Tailscale-only or LAN & Tailscale, configurable port, optional password auth, managed `authorized_keys`, and host key lifecycle. |
| `link/compute-client` | Optional `service_compute` launcher. Runs `locachy-compute serve` with `nsenter --net` into `locachy-net`. |
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

## Interactive shell and automatic ProxyJump SSH

To work inside a container or micro-VM like an ordinary machine:

- `sudo locachy-compute shell NAME [-- COMMAND...]` attaches an interactive TTY directly to a compute container (`--kind env`) or connects to a running micro-VM (`--kind vm`) over its loopback SSH port (`--console` attaches to the QEMU serial socket with `Ctrl+]` to detach).
- Under `--net user`, `vm create` automatically assigns a loopback-only SSH forward (`hostfwd=tcp:127.0.0.1:<port>-:22`, auto-allocated in `2222..2999`, or explicit `--ssh-port`, or `--ssh-port 0` to disable; optional `--ssh-user`). It never binds `0.0.0.0`.
- `locachy-compute ssh-config [--host IP] [--user USER] [--port PORT] [--prefix NAME] [--no-sudo] [--json]` prints a ready-to-paste client `~/.ssh/config` snippet with a jump host entry, `RemoteCommand sudo locachy-compute shell ENV` entries for compute containers, and `ProxyJump` entries for VMs with SSH forwarding.

## Reset and data

- `locachy-compute env reset NAME` stops the environment's jobs and deletes `upper/` and `work/`. The base image and every mounted dataset stay intact, so the reset takes milliseconds.
- Host data is mounted with `--mount HOST:CONTAINER[:ro|:rw]`, or set on the environment with `env create --mount`. Any absolute, existing path is allowed (for example `/mnt/nvme/...`, `/data/...`, `/srv/storage/<vol>` for `shared`/`compute` volumes, or `<compute base>/data/...`), except these roots: `/`, `/proc`, `/sys`, `/dev`, `/srv/storage` itself or any `data`-only volume under it, the peer store, the compute base (apart from its `data/` area), `/run/locachy/compute`, and the extended-display broker state.
- Managed multi-disk storage volumes (`tools/STORAGE.md`) can be bound by name with `--volume NAME[:CONTAINER_PATH[:ro|rw]]` (default container path `/volumes/NAME`, default `rw`) on `env create` or `run`. Volume bindings resolve through `/etc/locachy/volumes.json`, require `role` to be `shared` or `compute` (`data`-only volumes are refused), and require the volume to be mounted at `/srv/storage/NAME`.
- Setting `compute_storage="NAME"` in `/etc/locachy/rc.conf` (via `locachy-storage set-compute-storage NAME`) selects `/srv/storage/NAME/locachy-compute` instead of `/var/lib/locachy/compute` for `images/`, `envs/`, `jobs/`, `vms/`, and `data/`, unless `LOCACHY_COMPUTE_BASE` is set explicitly. The setting does not migrate existing files. If the configured volume is not mounted, `locachy-compute` fails closed rather than writing to the root disk. `locachy-compute storage [--json]` and the optional API's `GET /v1/health` (`storage_volume`, `compute_base`) report the active backing volume.
- Peers using the optional API cannot choose mounts or volumes. An SSH operator invoking the privileged CLI uses the same mount controls as an operator at the node.

## GPU

When the environment is created with `--gpu` (and `compute_gpu` is not `"0"`), the container receives `/dev/nvidiactl`, `/dev/nvidia-modeset`, `/dev/nvidia-uvm*`, `/dev/nvidiaN`, `/dev/nvidia-caps/*`, and `/dev/dri/renderD*`, plus read-only bind mounts of the host NVIDIA user-space libraries (`libcuda.so*`, `libnvidia-*.so*`, `libnvcuvid.so*`, `libnvoptix.so*`) in `/usr/local/lib/locachy-gpu`. `locachy-compute gpu-probe` lists what the host has. Devices that are absent are skipped.

## KVM micro-VMs

`locachy-compute vm create NAME --disk BASE.qcow2 [--share DIR:TAG[:ro|rw]] [--volume NAME[:TAG[:ro|rw]]] [--cloud-init SEED.img]` creates a copy-on-write overlay (`qemu-img create -b BASE -F qcow2`). `vm start` runs `qemu-system-x86_64` with `-enable-kvm` when `/dev/kvm` exists (TCG otherwise): a `virtio-blk-pci` disk, a serial console socket, a monitor socket, a QMP socket, and optional cloud-init. `vm reset` throws the overlay away. Each VM runs in its own cgroup, `/sys/fs/cgroup/locachy-compute/vm-NAME`.

The base image stays a read-only qcow2 with a copy-on-write overlay per VM, attached as `virtio-blk-pci` on its own iothread (emulated NVMe is not used):

```
-object iothread,id=iothread0
-drive file=/var/lib/locachy/compute/vms/NAME/overlay.qcow2,format=qcow2,if=none,id=hd0,cache=none,aio=io_uring,discard=unmap
-device virtio-blk-pci,drive=hd0,iothread=iothread0
```

`LOCACHY_COMPUTE_AIO=threads` replaces `aio=io_uring` on hosts where io_uring is disabled (for example by `kernel.io_uring_disabled`). The default is `io_uring`.

Directory shares (`--share`) and storage volumes (`--volume`, see `tools/STORAGE.md`) use **virtiofs**. The VM has one `virtiofsd` per share, started by `locachy-compute` as the VM's own uid, and QEMU's guest RAM is a shared memfd. For a VM with one ro share and one volume, the QEMU arguments are:

```
-object memory-backend-memfd,id=mem,size=1024M,share=on
-machine q35,accel=kvm,memory-backend=mem
-chardev socket,id=vfs0,path=/run/locachy/compute/vm-NAME/vfs-0.sock
-device vhost-user-fs-pci,queue-size=1024,chardev=vfs0,tag=datasets
-chardev socket,id=vfs1,path=/run/locachy/compute/vm-NAME/vfs-1.sock
-device vhost-user-fs-pci,queue-size=1024,chardev=vfs1,tag=vol-shared
```

Each share's daemon is started first, and its socket must exist before QEMU starts:

```
virtiofsd --socket-path=/run/locachy/compute/vm-NAME/vfs-0.sock --shared-dir=/data/dataset --sandbox=namespace --cache=auto --readonly
virtiofsd --socket-path=/run/locachy/compute/vm-NAME/vfs-1.sock --shared-dir=/srv/storage/vol-shared --sandbox=namespace --cache=auto
```

`--readonly` is added only for `:ro` shares. virtiofs is the only share transport (9p is not used).

- **Identity mapping.** `--sandbox=namespace` makes virtiofsd create its own user namespace that maps the caller's uid to itself (upstream `virtiofsd/src/sandbox.rs`, used when virtiofsd runs unprivileged). No `/etc/subuid` entry and no `--translate-uid` option is used. The guest sees files owned by the VM's uid (1000) as uid 1000, and files owned by other host uids as `nobody`. Writes by guest root are not remapped. Not yet run against a real virtiofsd on hardware.
- **Lifecycle.** `vm run` and `vm start` start the daemons before QEMU and stop them after QEMU exits. A failed start stops whatever already started. `vm stop` stops QEMU, then the daemons listed in the run directory, so guest writes are flushed first.
- **Environment seams.** `LOCACHY_VIRTIOFSD` overrides the binary (default: `virtiofsd` on `PATH`, then `/usr/lib/virtiofsd`, `/usr/libexec/virtiofsd`, `/usr/bin/virtiofsd`). `LOCACHY_VIRTIOFSD_SANDBOX` takes `namespace`, `chroot`, or `none`. `LOCACHY_COMPUTE_APP_UID` (default 1000) is the uid root hands a VM's files to. `LOCACHY_COMPUTE_KVM=0` forces TCG.

Foreground run and app export:

- `locachy-compute vm run NAME [--display none|gtk|sdl] [--qmp-sock PATH]` runs QEMU in the foreground. Its virtiofsd daemons are its own children. SIGTERM and SIGINT are forwarded to QEMU, which powers the guest off. The command creates no cgroup; `locachy-app` runs it inside its `app_NAME` cgroup, and the daemons inherit that cgroup. `--display gtk|sdl` adds `virtio-vga`, `qemu-xhci`, and `usb-tablet`, and sets `window-close=off` on the display, so closing the window (its button, the bar, the overview) never quits QEMU and never cuts the guest's power. QEMU's `-name` is `locachy-compute[NAME]`. The window title carries it, `QEMU (locachy-compute[NAME])` for GTK or `QEMU (locachy-compute[NAME]-N)` for SDL, and the desktop uses it to tell that VM app's window apart from other QEMU windows (`base/open.c`, `open_window_matches`).
- `locachy-compute vm export-app NAME [--app-name APP] [--display gtk|sdl] [--freeze qmp|none] [--network 0|1]` writes `/etc/locachy/containers/APP.conf` (default `APP` is `vm-NAME`) with `vm="1"`, `vm_kind="compute"`, `vm_name`, `vm_display`, `vm_freeze`, and `network`. `locachy-app` reads these keys directly (`vm_command` / `vm_check_keys` in `link/locachy-app`). On real host root it prepares `/run/locachy/compute/vm-<vm_name>` owned by uid 1000 and runs `locachy-compute vm run <vm_name> --display <display> --qmp-sock <sock>` as uid 1000 inside `app_APP` (see [VM-APPS.md](VM-APPS.md)).

Ownership for a VM run as uid 1000 (no root on the `vm run` path):

| Path | What uid 1000 needs |
|---|---|
| `LOCACHY_COMPUTE_RUN` (`/run/locachy/compute`) | Traversable: `compute-client` sets it to `0711` on every start, so uid 1000 reaches its own `vm-NAME` without listing the others. Either it is also writable by uid 1000, or `vm-NAME` is created beforehand and owned by uid 1000 (`vm run` refuses a directory it does not own) |
| Run directory `…/compute/vm-NAME/` | Owned by uid 1000, mode `0700` (`vm run` sets it). A directory that `vm start` creates as root is also `0700` |
| VM directory `…/compute/vms/NAME/` | Owned by uid 1000 (`vm create` and `vm reset` do this as real root) |
| `vm.json` | Owned by uid 1000, mode 0600 |
| `overlay.qcow2` | Owned by uid 1000 (read and write) |
| `--disk` base image | Readable by uid 1000 |
| `--share` directories and volume trees | Readable and writable by uid 1000 |
| `/dev/kvm` | Group `kvm` |
| `qemu-system-x86_64`, `virtiofsd` | Executable by uid 1000 |
| `/etc/locachy/containers/APP.conf` | Written by root (`export-app`), readable by `locachy-app` |

Root is still needed for `vm create` (the overlay), `vm reset` (only to hand files to the app uid on a real host), `export-app`, cgroup creation (`locachy-app`), and image creation.

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

`locachy-compute serve` (started by `service_compute`) provides an **optional** HTTPS control API on port 27184, with TLS 1.3 and mandatory client certificates:

- The server trusts only certificates of paired devices with scope `compute`. With none paired, every handshake fails.
- It binds only to the addresses of `tailscale0`, and, when `compute_bind_tb="1"`, to the IPv6 link-local addresses of `thunderbolt-net` interfaces. It never binds `0.0.0.0` or `::`.
- The client pins the server's certificate by fingerprint (`host_cert_sha256` in the client's `peer.json`, checked against the connection's certificate).

Endpoints: `GET /v1/health`, `GET /v1/envs`, `POST /v1/envs/NAME/reset`, `GET /v1/jobs`, `POST /v1/jobs`, `GET /v1/jobs/ID`, `GET /v1/jobs/ID/log`, `POST /v1/jobs/ID/stop`, `GET /v1/vms`, `POST /v1/vms/NAME/start`, `POST /v1/vms/NAME/stop`, and `POST /v1/vms/NAME/reset`.

From another machine:

```sh
locachy-compute remote --creds-dir ./work-mac --host 100.x.y.z health
locachy-compute remote --creds-dir ./work-mac --host 'fe80::1%tb0' run ENV -- python3 train.py
```

## Network: Tailscale and optional Thunderbolt

The normal NVIDIA experiment path uses Tailscale and OpenSSH. The compute node
runs inside `locachy-net`; `compute-client` enters the namespace with
`nsenter --net=/run/netns/locachy-net`, not `ip netns exec`. `ip netns exec`
mounts a new `/sys` and hides `/sys/fs/cgroup`; `nsenter` changes only the
network namespace, so the daemon keeps both the namespace's sockets and the
host cgroup tree.

Thunderbolt/USB4 is a **separate optional heterogeneous path**, not a
prerequisite for SSH, NVIDIA access, or the mTLS API. It may provide a direct
host-to-host link when the optional binding is enabled:

- With `extdisplay_tb_shared="1"`, or with `service_compute` and
  `compute_bind_tb="1"`, the extended-display broker leaves the
  `thunderbolt-net` interface in `locachy-net`. Its TLS pump is still confined
  to the link-local address and `SO_BINDTOIFINDEX` of that interface, with
  Landlock and seccomp.
- `network-client` keeps inbound policy at drop. Link-local IPv6 traffic on a
  verified `thunderbolt-net` interface is accepted only for TCP 27183 (extended
  display), TCP 27184 (optional compute API), and UDP 5353 (mDNS). Wi-Fi and
  Ethernet are never added to the allow-lists.
- With `extdisplay_tb_shared="0"` (high-isolation cable mode), the broker keeps
  the old behaviour: the interface moves into a private session namespace, and
  compute loses that link while the extended display is open.

Use the backend's own IP transport; do not treat the LOCachy HTTPS API as its
scheduler. The current Thunderbolt allow-list above does not open arbitrary
backend ports. The Open MPI 5 TCP fixture below verifies that an existing
backend can split a numerical workload across two isolated IP peers. It uses
IPv4 on veth, not the link-local IPv6 path above, and does not verify GPU work.
A real Thunderbolt deployment needs two addressed cable endpoints, a supported
backend on both platforms, and narrow access for its actual transport.

For hardware acceptance, verify the route uses the cable and that the intended
remote accelerator produces the expected result. Unplug the cable, verify the
workload does not silently fall back to another network or to CPU, then replug
and verify that a new run succeeds without re-entering the peer address or
restarting the node. A link, ping, or `iperf` is not this test. The complete
LOCachy workflows over real Tailscale and Thunderbolt remain unverified.

## rc.conf keys

| Key | Meaning |
|---|---|
| `service_tailnet_ssh="/usr/local/lib/locachy/tailnet-ssh-client"` | Enables managed OpenSSH (`sshd`); disabled by default. |
| `ssh_listen="tailscale"` or `"lan"` | Bind `sshd` to `tailscale0` only (default) or to active LAN interfaces plus `tailscale0`. |
| `ssh_port="22"` | `sshd` listening port (`1..65535`, default `22`). |
| `ssh_password="0"` or `"1"` | Allow PAM password login for regular accounts (`0` default: public key only; root and empty passwords remain disabled). |
| `service_compute="/usr/local/lib/locachy/compute-client"` | Enables the optional HTTPS API. |
| `compute_port="27184"` | Control-plane port (default 27184). |
| `compute_bind_tb="1"` | Also listen on Thunderbolt link-local addresses. |
| `compute_gpu="0"` | Never pass GPUs to containers. |
| `compute_storage="NAME"` | Store compute images/envs/jobs/VMs on `/srv/storage/NAME/locachy-compute` (requires `compute` or `shared` role; fails closed if unmounted). |
| `extdisplay_tb_shared="1"` or `"0"` | Shared link (default when compute binds Thunderbolt) or private session namespace. |
| `extdisplay_paired_only="1"` | Accept only paired devices; no cleartext code is advertised or accepted. |

## Tests

```sh
src/tools/test-peers.sh        # peer store, pairing permissions, broker PSK, shared Thunderbolt link, shipped filter
src/tools/test-compute.sh      # image import, native containers with zero Wayland/compositor env leakage, host data bind-mounts and path-policy rejection, instant upperdir reset, GPU device/library probe and wildcard bind rejection, KVM micro-VM config, qcow2 CoW overlay creation, reset and QEMU argv verification, mTLS HTTPS control plane (paired, unpaired, extdisplay-only and revoked peers; remote job run, log and reset)
src/tools/test-thunderbolt-backend.sh # isolated Open MPI TCP split-sum fixture; not physical Thunderbolt or GPU
src/tools/test-extdisplay-broker.sh
```

`src/tools/test-tailnet-ssh.py` must run as PID 1 in a disposable Linux guest
with the kernel command-line flag `locachy.tailnet-ssh-test=1`. It uses real
OpenSSH, SCP, SFTP and `sudo locachy-compute` on a mock Tailscale interface,
not a real tailnet. It checks that detached jobs report their actual exit
codes and logs after SSH rebind, and that their cgroups are removed.

The compute shell test uses unprivileged Linux namespaces; its writable-cgroup
checks require a delegated cgroup v2 subtree. Test overrides use `LOCACHY_` environment variables: `locachy-compute` reads `LOCACHY_PEERS_DIR` (via `locachy_peers.py`), `LOCACHY_COMPUTE_BASE`, `LOCACHY_COMPUTE_RUN`, `LOCACHY_COMPUTE_CGROUP`, `LOCACHY_COMPUTE_RC`, `LOCACHY_COMPUTE_APP_INIT`, `LOCACHY_COMPUTE_ALLOW_NO_CGROUP`, and `LOCACHY_CONTAINERS_DIR` (plus the VM seams listed above); `compute-client` also reads `LOCACHY_COMPUTE_NETNS`, `LOCACHY_COMPUTE_BIN`, and `LOCACHY_NETWORK_CLIENT`. The extended-display broker's test fixtures are compile-time macros.

## Limits

- A static `app-init` is required to run as PID 1 inside foreign root filesystems. A host-built dynamic one is not used there, and the built-in reaper takes over.
- Revoking a device does not end connections it already has open. Each request uses its own connection, so these end quickly.
- GPU passthrough is verified by the device-node and library probe. The NVIDIA user-space stack itself was not exercised on hardware in these tests.
- Thunderbolt link discovery, and the nftables allow-list being filled by `sync_tb_filter`, need a real `thunderbolt-net` device to exercise. The tests use a veth pair with the same rules.
