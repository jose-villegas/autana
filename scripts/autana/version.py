"""autana's own version: `autana --version` prints it, and it rides along in a
device lock record as "autana_version", purely informational, so a refusal
message can name what is holding the board. Never compared for compatibility;
LOCK_PROTOCOL in scripts/device/device_lock.py is what a claim checks."""

__version__ = "0.1.0"
