# Cloud drives

[中文](CLOUD.zh.md)

Cloud drives are rclone remotes mounted with FUSE under `~/Drives/<remote>`. rclone owns every backend, its login and its tokens; LOCachy supervises the mounts, shows their state, refreshes folders and reports failures. There is no provider-specific code.

## Mounts

`link/rclone-client` is the `service_rclone` service. It lists remotes with `rclone listremotes --json` and runs one `rclone mount` per remote as the user (uid 1000, no supplementary groups), with a VFS write cache.

The mount must reach the host and every running container. `ip netns exec` cannot be used: it makes all mounts of a new mount namespace slaves, so a FUSE mount made there stays invisible to the host. The service therefore makes only `/` a slave in its own mount namespace, keeps the separately shared `~/Drives`, binds `locachy-net`'s resolver privately and enters that network namespace with `nsenter --net`. Without `locachy-net` or its resolver it does not mount, rather than going out directly. `rclone config` and `--reconnect` run in `locachy-net` for the same reason.

A remote whose name cannot be a safe folder name, or whose folder is already a mount point, is not empty, or is a symbolic link, is skipped; no local data is hidden. The USB drive service (`drives`) now adopts only real removable block devices at startup and never unmounts a cloud drive or other mount it did not make.

## State

`/run/locachy/cloud/status` (root, 0644) has one line per remote: name, path, state (`mounting`, `ready`, `refreshing`, `failed`, `unmounted`), the time of the last successful refresh, and a reason category (`authentication`, `rate-limit`, `permission`, `network`, `attention`, `busy`). Raw rclone output stays in a root-only log; neither the status nor a notification carries it.

Each mount has a private rclone control (RC) socket in a user-only directory. Applications never reach it. The service's own control socket accepts only root and the user, and only two operations on known mounts: refresh a folder and unmount a drive.

## Refresh

rclone caches directory listings. Reading a folder again shows that cache, not the remote, so F5 alone would not see changes made elsewhere. On a cloud folder, F5, the toolbar's Refresh button, the folder menu's Refresh and pulling down at the top of the list all ask the service to refresh that one folder with rclone's `vfs/refresh` (not recursively), check the result reported for that folder, and then read it again. Other folders reload as before.

Pull to refresh is a finger gesture: one finger on a touch screen, or a touchpad's finger scroll, starting with the list at its top. After three rows it says "Release to refresh"; lifting the finger refreshes once. Reversing, a second finger, a pinch or losing the screen cancels it. A mouse wheel, the TrackPoint's middle-button scrolling and file drags never pull. The pointer layer reports the scroll source and its begin, end and cancel only to programs that ask for it.

files reads a cloud folder in a separate process, as the user and without network, with a time limit, so the interface never waits for the network. A failure or time-out keeps the previous list and says so. Types come from file names, as KIO does for slow mounts, so drawing a folder downloads nothing; folder sizes are not counted. `e` on a drive in `~/Drives` unmounts a cloud drive through the service; uploads still pending or open files keep it mounted.

## Sign-in and notifications

rclone renews access tokens itself; an expiring token is not a reason to sign in again. Only rclone's own reconnect advice and the standard OAuth errors `invalid_grant` and `invalid_token` mark a remote as needing sign-in. Network failures, 401/403/429/5xx and other errors are never reported as a sign-in problem; unclassified failures say "needs attention". rclone has no backend-independent sign-in status, so nothing more is inferred.

A change of state is notified once through the standard notification service, which the bar now also serves for host programs on root's session bus. The notification is replaced, not repeated, while the state lasts, and closed on recovery. files and settings show the same state. In settings, Enter on the remotes line runs `rclone config reconnect` for a remote that needs sign-in, and `rclone config` otherwise. Nothing signs in, deletes credentials or changes the configuration by itself.

## Tests

- `tools/test-rclone.py`: the supervisor with stand-ins (16 tests).
- `tools/test-rclone-fuse.sh`: real rclone and FUSE in a disposable user and mount namespace: propagation to the host and to a namespace made before the mount, the private resolver, refresh, status, pending writes and unmount.
- `tools/test-files.sh`: files over a scratch home with the real folder worker and a stand-in service: places, folder reads, F5, pull, short and cancelled pulls, the TrackPoint, failure, unmount; plain and with ASan/UBSan.
- `tools/test-pointer.c`, `tools/test-host-notify.c`, `tools/test-drives-state.c`: scroll lifecycle, host notifications, drive adoption.

A stand-in local remote replaces a cloud backend in these tests. Real accounts, an expired real OAuth grant, and a finger on real hardware remain acceptance on a machine; see the repository's `REGRESSIONS.md`.
