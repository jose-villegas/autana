"""Optional shell command for device lock events."""

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402

HOOK_TIMEOUT_SECONDS = 3


def emit(event, board, owner="", purpose="", note=""):
    """Runs the project's `lock_hook`, if it sets one, with the event in
    AUTANA_LOCK_* variables - the hook's own interface, so a hook reads them
    as it would git's."""
    command = autana_config.load().get("lock_hook")
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
