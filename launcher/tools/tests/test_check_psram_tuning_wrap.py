"""check_psram_tuning_wrap.py on canned `objdump -d` text: at 120 MHz the PSRAM
driver must reach the tuning through the wrapper, and below it nothing wraps."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "build"))

import check_psram_tuning_wrap as gate  # noqa: E402

WRAPPED = """
40386000 <esp_psram_impl_enable>:
40386103:\tf5afe5      \tcall8\t4037bc00 <__wrap_mspi_timing_psram_tuning>
4037bc00 <__wrap_mspi_timing_psram_tuning>:
4037bc69:\t07e1a5      \tcall8\t40383a84 <mspi_timing_psram_tuning>
40383a84 <mspi_timing_psram_tuning>:
40383a90:\t000000      \tcall8\t40383b00 <s_do_tuning>
"""

UNWRAPPED = """
40386000 <esp_psram_impl_enable>:
40386103:\tf5afe5      \tcall8\t40383a84 <mspi_timing_psram_tuning>
40383a84 <mspi_timing_psram_tuning>:
40383a90:\t000000      \tcall8\t40383b00 <s_do_tuning>
"""

BOTH = WRAPPED + """
40390000 <psram_retune>:
40390010:\t000000      \tcall8\t40383a84 <mspi_timing_psram_tuning>
"""


TUNING_ADDRESS = (0x40383A84).to_bytes(4, "little")
WRAPPER_ADDRESS = (0x4037BC00).to_bytes(4, "little")

INDIRECT_WRAPPER = """
40386000 <esp_psram_impl_enable>:
40386100:	000000      	callx8	a8
4037bc00 <__wrap_mspi_timing_psram_tuning>:
4037bc69:	07e1a5      	call8	40383a84 <mspi_timing_psram_tuning>
40383a84 <mspi_timing_psram_tuning>:
40383a90:	000000      	call8	40383b00 <s_do_tuning>
"""


class PsramTuningWrapGate(unittest.TestCase):
    def test_a_driver_calling_the_wrapper_passes(self):
        self.assertEqual([], gate.problems(WRAPPED, sweeps=True))

    def test_a_lost_wrap_fails_by_both_symptoms(self):
        found = gate.problems(UNWRAPPED, sweeps=True)
        self.assertEqual(2, len(found))
        self.assertIn("nothing calls __wrap_mspi_timing_psram_tuning", found)
        self.assertIn("esp_psram_impl_enable", found[1])

    def test_a_second_direct_caller_fails_even_with_the_wrapper_in_use(self):
        found = gate.problems(BOTH, sweeps=True)
        self.assertEqual(1, len(found))
        self.assertIn("psram_retune", found[0])
        self.assertNotIn("__wrap_", found[0].split("by ")[1])

    def test_a_taken_tuning_address_fails_though_every_direct_call_is_wrapped(self):
        found = gate.problems(WRAPPED, sweeps=True, image=bytes(64) + TUNING_ADDRESS)
        self.assertEqual(1, len(found))
        self.assertIn("indirect call", found[0])

    def test_tuning_address_bytes_across_two_words_pass(self):
        """Two neighbouring words can hold the address's bytes at an
        unaligned offset; no pointer sits there."""
        self.assertEqual([], gate.problems(WRAPPED, sweeps=True, image=bytes(62) + TUNING_ADDRESS + bytes(2)))

    def test_a_wrapper_reached_only_through_its_address_passes(self):
        self.assertEqual([], gate.problems(INDIRECT_WRAPPER, sweeps=True, image=WRAPPER_ADDRESS))

    def test_a_wrapper_with_no_call_and_no_taken_address_fails(self):
        self.assertEqual(["nothing calls __wrap_mspi_timing_psram_tuning"],
                         gate.problems(INDIRECT_WRAPPER, sweeps=True, image=b""))

    def test_without_the_sweep_an_unwrapped_driver_passes(self):
        self.assertEqual([], gate.problems(UNWRAPPED, sweeps=False))

    def test_without_the_sweep_a_linked_wrapper_fails(self):
        self.assertEqual(1, len(gate.problems(WRAPPED, sweeps=False)))


if __name__ == "__main__":
    unittest.main()
