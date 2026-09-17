#!/bin/bash
# no SERVER directive: runs standalone with BUILD_DIR set
# EXPECT: fail
#
# Deliberate failure with no server: the runner must run this script, observe
# a non-zero exit, and report exactly one FAIL with a non-zero overall status.
# Lives in the off-by-default selftest/ category so it is never swept by the
# default run or CI -- invoke it explicitly to check the runner's FAIL path:
#   ./run-system-tests.py selftest/fail-standalone

echo "selftest: intentional non-zero exit to verify the runner reports FAIL"
exit 1
