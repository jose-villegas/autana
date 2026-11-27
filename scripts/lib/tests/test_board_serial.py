"""Tests the serial read policy without a port."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import board_serial


class ReadReadyTests(unittest.TestCase):
    def test_a_read_asks_for_what_is_waiting_at_least_one_byte_at_most_the_cap(self):
        for waiting, expected in ((0, 1), (5, 5), (board_serial.READ_BYTES * 2, board_serial.READ_BYTES)):
            with self.subTest(waiting=waiting):
                connection = Mock(in_waiting=waiting)
                self.assertIs(board_serial.read_ready(connection), connection.read.return_value)
                connection.read.assert_called_once_with(expected)
