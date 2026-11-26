# foot over ssh

foot sets `TERM=foot` (or `foot-direct`, `foot-extra`). A remote host that does not have foot's terminfo entry cannot draw correctly for it: its shell and line editor use the wrong control sequences, and the screen breaks. macOS has no foot entry (checked with `infocmp`).

LOCachy installs `/etc/ssh/ssh_config.d/50-locachy-foot-term.conf` (source: `src/link/`). When the local `TERM` is `foot`, `foot-direct` or `foot-extra`, the ssh client sends `TERM=xterm-256color` to the remote side. Other local values pass unchanged. The local shell keeps `TERM=foot` for its own programs.

## Requirements

- The ssh client is OpenSSH 8.7 or newer, which honors `SetEnv TERM` in the pty request. The `openssh` package in LOCachy's base manifest provides it.
- The remote sshd needs no change: the client sends the value.

## Per host

An entry in `~/.ssh/config` is read before the system drop-in, so a host that has foot's terminfo keeps `TERM=foot`:

```
Host my-host
    SetEnv TERM=foot
```

To install foot's terminfo on a host you control, copy `foot` and `foot-direct` into `~/.terminfo/f/` on that host (see foot(1)).

## Trade-off

Remote programs see `xterm-256color`: 256 colours and the xterm key set, not foot's extras. Local programs are unaffected.

## Verification

`src/tools/test-foot-ssh-term.sh` checks the mapping with `ssh -G`. It also starts an unprivileged OpenSSH server with no `AcceptEnv` and confirms what the remote shell receives for `foot`, `foot-direct`, `linux`, and a host-level override.

Not verified: how fish, zsh and vim render on a real remote host from LOCachy's foot window. The test checks the TERM value, not the screen.
