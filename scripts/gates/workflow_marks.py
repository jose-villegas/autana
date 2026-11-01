"""The CI steps a local hook runs: workflow steps that carry a marker in their env.

A hook reads the repository it runs in, so whichever repository marks a step
gets that step run before its commit or push, and the hook names no gate.
"""
import shlex

WORKFLOWS = '.github/workflows/'


def is_workflow(path):
    return path.startswith(WORKFLOWS) and path.endswith(('.yml', '.yaml'))


def marked_steps(workflows, marker):
    """(command as argv, marker value) for each step of the parsed `workflows`
    whose env sets `marker`."""
    return [(shlex.split(step['run']), str(step['env'][marker]))
            for workflow in workflows for job in (workflow or {}).get('jobs', {}).values()
            for step in job.get('steps', []) if marker in step.get('env', {})]
