"""Command execution and line records for held audio captures."""
import shlex
import shutil
import subprocess


def autana_run(command, *words):
    executable = shutil.which(command)
    command = [executable] if executable else shlex.split(command)
    if not command:
        raise ValueError("empty autana command")
    command[0] = shutil.which(command[0]) or command[0]
    return subprocess.run([*command, *map(str, words)], check=True,
                          capture_output=True, text=True).stdout


def record_line(output, key, payload):
    output.write(f"{key}\t{payload}\n")
    output.flush()
