# Process trees

Why a stopped command leaves no child holding the serial port or the device lock, and how
`scripts/lib/process_tree.py` guarantees it on each OS. The device, build and perf scripts
all start their commands through it.

## The problem

A script runs a build or a flash, which runs more processes. Killing only the first leaves the
rest running, still holding a pipe, the serial port or a lock. The module starts every command
as one *tree* that can be stopped whole.

| | Linux and macOS | Windows |
|---|---|---|
| Start | a new session, so the tree has its own process group | a job object, assigned while the process is still suspended, then resumed |
| Stop | `SIGTERM` to the group, wait 2 s, then `SIGKILL` | close the job; if no job was assigned, `taskkill /F /T` |
| Parent dies first | opt in with `parent_lifetime=True`: the child gets `SIGKILL` through `PR_SET_PDEATHSIG` (Linux only) | the job is a kill-on-close job, so closing the job's last handle ends the tree |

```mermaid
flowchart LR
    L["run_process_tree(command, timeout)"] --> W{"Windows?"}
    W -->|"yes"| J["create job, start suspended,<br/>assign, resume"]
    W -->|"no"| S["start in a new session"]
    J --> R["wait, polling watch,<br/>handing lines to on_line"]
    S --> R
    R -->|"deadline, watch or on_line raises,<br/>error, Ctrl+C"| X["stop_process_tree()"]
    R -->|"finished"| C["close_process_tree()"]
    X --> C
```

## The functions

| Function | Does |
|---|---|
| `run_process_tree(command, timeout=None, *, watch=None, on_line=None, **options)` | runs the command to its end like `subprocess.run`; past `timeout` raises `TimeoutExpired`, `watch` is called at least every `WATCH_SECONDS` and stops the run by raising, `on_line` gets each output line in the caller's thread; any exception stops the tree |
| `launch_process_tree(command, parent_lifetime=False, new_session=True, **options)` | starts the command as a stoppable tree; `options` go to `Popen` |
| `stop_process_tree(process)` | terminates the tree, waits up to 10 s, then releases the job |
| `close_process_tree(process)` | releases the Windows job, killing any descendant still in it; a no-op on POSIX |
| `install_console_interrupt_handler()` | turns Windows Ctrl-Break into `KeyboardInterrupt` so locks and ports unwind |

`run_to_end()` in `scripts/device/device.py` is `run_process_tree` with a `watch` that raises the
moment the held lock is lost. `guard_stage()` in `scripts/lib/local_schedule.py` is the `watch` of a GPU stage:
it raises once the stage stops progressing or passes its backstop.

## Using it

| Do | Do not |
|---|---|
| Run a command to its end with `run_process_tree`; launch and close by hand only a tree you must signal mid-run | use bare `subprocess.Popen` for a command that may spawn children |
| On Linux, start a `parent_lifetime=True` child from the thread that waits for it | start it from a worker thread that may exit first; the signal follows that thread |
| Treat the "using tree kill" message as a downgrade | rely on a job object when the message appeared |

The [device lock](../cli/Device-Lock.md) depends on this: a lock is released when its holder ends,
and the holder's children must end with it.
