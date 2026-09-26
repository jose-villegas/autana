"""Optional shell command for device lock events."""

import os
import subprocess


HOOK_TIMEOUT_SECONDS = 3


def emit(event, board, owner="", purpose="", note=""):
    command = os.environ.get("AUTANA_LOCK_HOOK")
    if not command:
        return
    environment = os.environ.copy()
    environment.update({
        "AUTANA_LOCK_EVENT": event,
        "AUTANA_LOCK_BOARD": board,
        "AUTANA_LOCK_OWNER": owner,
        "AUTANA_LOCK_PURPOSE": purpose,
        "AUTANA_LOCK_NOTE": note,
    })
    try:
        subprocess.run(command, shell=True, env=environment,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL,
                       timeout=HOOK_TIMEOUT_SECONDS)
    except Exception:
        pass
