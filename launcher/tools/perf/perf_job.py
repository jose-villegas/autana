"""Expected board hold duration for complete performance jobs."""
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/device"))
from device_lock import duration_history  # noqa: E402

# Fallback when the board has no flash history.
DEFAULT_FLASH_SECONDS = 60


def job_estimate(flashes, captures, timeout, restore=True):
    """Seconds a job expects to hold the board: recorded flash and capture durations,
    else DEFAULT_FLASH_SECONDS and the capture deadline."""
    durations = duration_history()
    seconds = (flashes + int(restore)) * durations.get("flash", DEFAULT_FLASH_SECONDS)
    return math.ceil(seconds + captures * durations.get("run-suite", timeout))
