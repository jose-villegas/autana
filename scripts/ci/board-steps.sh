#!/bin/bash
# Steps run inside a board hold: the self-test, then the console tests on the held port.
# BOARD_CI_LOG overrides the self-test log path; DEVICE_TESTS_VENV the venv board-prepare.sh made.
set -eo pipefail
venv=${DEVICE_TESTS_VENV:-$RUNNER_TEMP/device-tests}
python=$venv/bin/python
[ -x "$python" ] || python=$venv/Scripts/python
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
mkdir -p .records/device/pytest-logs
selftest_status=0
autana selftest | tee "${BOARD_CI_LOG:-$RUNNER_TEMP/board-ci-selftest.log}" || selftest_status=$?
console_status=0
autana flash diag || console_status=$?
if [ "$console_status" -eq 0 ]; then
  "$python" -m pytest launcher/test/device --embedded-services idf,serial \
    --build-dir build.diag --port "$("$python" scripts/device/device.py resolve-port | tr -d "\r")" \
    --skip-autoflash y --root-logdir .records/device/pytest-logs \
    --junitxml=.records/device/pytest-logs/results.xml \
    | tee .records/device/pytest-logs/pytest.log || console_status=$?
fi
echo "Console pytest exit status: $console_status" >> "$summary"
if [ -f .records/device/pytest-logs/pytest.log ]; then
  tail -n 1 .records/device/pytest-logs/pytest.log >> "$summary"
fi
if [ "$selftest_status" -ne 0 ]; then exit "$selftest_status"; fi
exit "$console_status"
