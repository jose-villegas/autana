"""Tests the serial read policy without a port."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import board_serial


class ReadReadyTests(unittest.TestCase):
    def test_read_requests_available_bytes_up_to_the_limit_or_one_when_empty(self):
        for waiting, expected, kwargs in (
                (0, 1, {}),
                (5, 5, {}),
                (8192, 4096, {}),
                (5, 2, {"limit": 2})):
            with self.subTest(waiting=waiting, **kwargs):
                connection = Mock(in_waiting=waiting)
                self.assertIs(board_serial.read_ready(connection, **kwargs), connection.read.return_value)
                connection.read.assert_called_once_with(expected)
