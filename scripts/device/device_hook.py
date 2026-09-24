"""Optional shell command for device lock events."""

import os
import subprocess
import sys


HOOK_TIMEOUT_SECONDS = 3


def emit(event, port, owner="", purpose="", note=""):
    command = os.environ.get("AUTANA_LOCK_HOOK")
    if not command:
        return
    environment = os.environ.copy()
    environment.update({
        "AUTANA_LOCK_EVENT": event,
        "AUTANA_LOCK_PORT": port,
        "AUTANA_LOCK_OWNER": owner,
        "AUTANA_LOCK_PURPOSE": purpose,
        "AUTANA_LOCK_NOTE": note,
    })
    try:
        result = subprocess.run(command, shell=True, env=environment,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True,
                                timeout=HOOK_TIMEOUT_SECONDS)
        if result.returncode:
            raise RuntimeError(f"exit status {result.returncode}")
    except Exception as error:
        message = str(error).splitlines()[0] if str(error).splitlines() else type(error).__name__
        print(f"warning: device lock hook failed: {message}", file=sys.stderr)
