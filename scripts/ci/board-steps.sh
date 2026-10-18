#!/bin/bash
# Steps run inside a board hold; BOARD_CI_LOG overrides the self-test log path.
set -eo pipefail
autana selftest | tee "${BOARD_CI_LOG:-$RUNNER_TEMP/board-ci-selftest.log}"
