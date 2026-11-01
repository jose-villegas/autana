"""The CI steps a local hook runs: workflow steps that carry a marker in their env.

A hook reads the repository it runs in, so whichever repository marks a step
gets that step run before its commit or push, and the hook names no gate.
"""
import shlex
import shutil
import sys

WORKFLOWS = '.github/workflows/'
PYTHONS = ('python', 'python3')


def is_workflow(path):
    return path.startswith(WORKFLOWS) and path.endswith(('.yml', '.yaml'))


def marked_commands(workflows, marker, allowed, required=True):
    """(argv, marker value) for each step of the parsed `workflows` whose env
    sets `marker`, in the order the workflows list them.

    A Python step runs on this interpreter; any other program is found on
    PATH, since Windows looks in System32 first and finds WSL's bash before
    Git's. A value outside `allowed` raises ValueError, and so does no step
    at all when `required`: a hook that silently ran nothing would pass
    every commit.
    """
    commands = []
    for workflow in workflows:
        for job in (workflow or {}).get('jobs', {}).values():
            for step in job.get('steps', []):
                if marker not in step.get('env', {}):
                    continue
                value = str(step['env'][marker])
                if value not in allowed:
                    raise ValueError(f'{marker}: "{value}" is none of {", ".join(allowed)}: {step["run"]}')
                argv = shlex.split(step['run'])
                argv[0] = sys.executable if argv[0] in PYTHONS else shutil.which(argv[0]) or argv[0]
                commands.append((argv, value))
    if required and not commands:
        raise ValueError(f'No step in {WORKFLOWS} is marked {marker}')
    return commands
