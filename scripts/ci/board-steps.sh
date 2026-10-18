#!/bin/bash
set -eo pipefail
autana selftest | tee "${BOARD_CI_LOG:-$RUNNER_TEMP/board-ci-selftest.log}"
