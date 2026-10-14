"""Application ownership shared by firmware gates; packages.sh is the shell twin."""
import pathlib

APP_ROOTS = ("apps", "shell/apps")


def app_part(rel):
    """The app directory as a repository path, or None outside an app."""
    path = pathlib.PurePosixPath(rel)
    for root in APP_ROOTS:
        try:
            parts = path.relative_to("launcher/main/" + root).parts
        except ValueError:
            continue
        if len(parts) > 1:
            return "launcher/main/" + root + "/" + parts[0]
    return None
