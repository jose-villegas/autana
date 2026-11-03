# console

The battery verb, registered in the console's verb table from here, beside the PMU it reads.

`BATTERY` reads the PMU fresh and replies with the decoded snapshot and raw
status bytes; formatting is in `console_battery_reply.h`.

| Command | Reply |
|---|---|
| `BATTERY` | Fresh snapshot and raw status bytes |
| `BATTERY LOG` | Oldest-first `BATLOG` records, then `BATLOG end count=N` |
| `BATTERY LOG CLEAR` | Erases the history namespace; `BATLOG cleared` on success |

Log rows carry `seq`, `up_min`, `mv`, `percent`, `usb`, `battery`, `charge`,
`flow` and `reason`. Parsing and formatting share `console_battery_reply.h`.
Reasons are `periodic`, `transition`, `boot` and `shutdown-warning`.
Storage failures reply `BATLOG error read` or `BATLOG error clear`.
