"""The environment without the variables git sets for one repository."""
import os
import subprocess


def env_without_repository(environ=os.environ):
    """`environ` less GIT_DIR, GIT_INDEX_FILE and the rest `git rev-parse
    --local-env-vars` lists. A git hook runs with them set for its own
    repository, which would answer a child's git call for that repository
    instead of the one the child is in."""
    local = set(subprocess.run(["git", "rev-parse", "--local-env-vars"], capture_output=True,
                               text=True).stdout.split())
    return {name: value for name, value in environ.items() if name not in local}
