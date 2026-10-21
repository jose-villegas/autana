"""Command execution and line records for held audio captures."""
import shlex
import shutil
import subprocess
import sys
import time

AUTANA_RUN_ATTEMPTS = 3
AUTANA_RUN_RETRY_SECONDS = 1


class AutanaRunError(subprocess.CalledProcessError):
    def __str__(self):
        return f"{super().__str__()} stderr: {self.stderr}"


def autana_run(command, *words):
    executable = shutil.which(command)
    command = [executable] if executable else shlex.split(command)
    if not command:
        raise ValueError("empty autana command")
    command[0] = shutil.which(command[0]) or command[0]
    for attempt in range(AUTANA_RUN_ATTEMPTS):
        try:
            return subprocess.run([*command, *map(str, words)], check=True,
                                  capture_output=True, text=True).stdout
        except subprocess.CalledProcessError as error:
            detail = " ".join((error.stderr or "").splitlines())
            print(f"autana attempt {attempt + 1}/{AUTANA_RUN_ATTEMPTS} failed: {detail}",
                  file=sys.stderr, flush=True)
            if attempt + 1 == AUTANA_RUN_ATTEMPTS:
                raise AutanaRunError(error.returncode, error.cmd, output=error.output,
                                     stderr=error.stderr) from error
            time.sleep(AUTANA_RUN_RETRY_SECONDS)


def record_line(output, key, payload):
    output.write(f"{key}\t{payload}\n")
    output.flush()
