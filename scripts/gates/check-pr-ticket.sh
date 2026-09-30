#!/bin/sh
# Passes when a pull request body carries a line `Ticket: <id>`, the bare
# backlog id (`cvpg`, `ems.12`); an `autana-` prefix is tolerated. Only the
# format is checked: CI has no access to the backlog database.
#
# Usage: check-pr-ticket.sh [<body-file>]     (stdin when no file is given)

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [<body-file>]" >&2
    exit 2
fi

if [ "$#" -eq 1 ]; then
    [ -r "$1" ] || { echo "cannot read '$1'" >&2; exit 2; }
    body=$(cat "$1")
else
    body=$(cat)
fi

# GitHub stores bodies typed in the web form with CRLF line ends.
if printf '%s\n' "$body" | tr -d '\r' | LC_ALL=C grep -Eq '^Ticket:[[:space:]]*(autana-)?[a-z0-9]+(\.[0-9]+)*[[:space:]]*$'; then
    exit 0
fi

echo "PR description needs a line 'Ticket: <id>' naming its backlog ticket, the bare id without the autana- prefix (for example 'Ticket: cvpg' or 'Ticket: ems.12')." >&2
exit 1
