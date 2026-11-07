#!/usr/bin/env python3
"""test-bar-keys.py DEVICE... -- PROGRAM [ARG...]: PROGRAM (locachy-bar) with
the keyboards DEVICE... as locachy-seat gives them: a socket at
LOCACHY_BAR_KEYS, a "keys" message with the open devices. PROGRAM keeps this
process's id (the tests signal it); a child holds the other end until
PROGRAM ends, as seat does while it runs, reading what PROGRAM sends on it
(the bar's "locks" after a key: seat takes them for process 1) until its end
is closed."""
import os
import socket
import sys

if "--" not in sys.argv[1:] or sys.argv.index("--") == len(sys.argv) - 1:
    sys.exit(__doc__)
split = sys.argv.index("--")
devices, program = sys.argv[1:split], sys.argv[split + 1:]
ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
fds = [os.open(d, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC) for d in devices]
if os.fork() == 0:
    theirs.close()
    socket.send_fds(ours, [b"keys"], fds)
    for fd in fds:
        os.close(fd)
    # A message is not the end: the bar reports its locks on this link at
    # the first key. Taking one as the end closed the link, and the bar
    # dropped the keyboards ("watching 0").
    while ours.recv(4096):
        pass
    os._exit(0)
ours.close()
for fd in fds:
    os.close(fd)
os.set_inheritable(theirs.fileno(), True)
os.environ["LOCACHY_BAR_KEYS"] = str(theirs.fileno())
os.execv(program[0], program)
