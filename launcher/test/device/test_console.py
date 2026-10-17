"""Registered Unity suites over the shell console."""
from console_pytest import REPLY_TIMEOUT_S


def test_console_suite(console):
    dut = console.dut
    dut.write("RUNSUITE suite_console")
    dut.expect_unity_test_output(timeout=REPLY_TIMEOUT_S)
    dut.expect(rb"RUNSUITE_COMPLETE name=suite_console found=1[^\r\n]*")
    assert dut.testsuite.testcases, "Unity returned no cases"
    assert not dut.testsuite.failed_cases
