"""What a lock holder started, and stopping it, whichever OS this is.

One mechanism per OS behind one interface: a kill-on-close job object on
Windows (lock_job), a token in the environment plus a watchdog on Linux
(lock_group), nothing elsewhere. The lock is the promise of the serial port,
so it is not released while anything started under it can still hold the
port."""

import os

if os.name == "nt":
    import lock_job as backend
else:
    import lock_group as backend

enter = backend.enter
leave = backend.leave
members = backend.members
reap = backend.reap
survivors = backend.survivors
