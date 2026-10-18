#!/bin/bash
# Creates the console tests' venv before a board hold; DEVICE_TESTS_VENV overrides its path.
set -eo pipefail
venv=${DEVICE_TESTS_VENV:-$RUNNER_TEMP/device-tests}
PY=$(. scripts/lib/python.sh && find_python)
"$PY" -m venv "$venv"
python=$venv/bin/python
[ -x "$python" ] || python=$venv/Scripts/python
# The tests' packages go in this run's venv and pip cache, not the runner's.
PIP_CACHE_DIR="${RUNNER_TEMP:-$venv}/pip-cache" "$python" -m pip install -r launcher/test/device/requirements.txt
