#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "replay", Path(__file__).resolve().parents[1] / "tools/cbs-replay.py")
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


class ReplayTest(unittest.TestCase):
    def test_gsm_multipart(self):
        text = "TEST ONLY " + "X" * 75
        pages = replay.make_pdus(text, 4370, 0x6c10, "gsm")
        self.assertEqual([len(p) for p in pages], [88, 88, 88])
        self.assertEqual([p[:6].hex() for p in pages],
                         ["6c1011124813", "6c1011124823", "6c1011124833"])
        self.assertEqual("".join(p[6:].decode("utf-16-be").rstrip("\r")
                                 for p in pages), text)

    def test_umts_lengths(self):
        pdu, = replay.make_pdus("TEST ONLY " + "X" * 75, 4370, 0x6c10, "umts")
        self.assertEqual(pdu[:7].hex(), "0111126c104803")
        self.assertEqual(len(pdu), 256)
        self.assertEqual([pdu[7 + i * 83 + 82] for i in range(3)], [82, 82, 6])

    def test_invalid_input(self):
        for text in ("unlabelled", "TEST ONLY \U0001f600", "TEST ONLY " + "X" * 606):
            with self.assertRaises(ValueError):
                replay.make_pdus(text, 4370, 0, "gsm")
        for channel, serial in ((-1, 0), (65536, 0), (4370, 65536)):
            with self.assertRaises(ValueError):
                replay.make_pdus("TEST ONLY", channel, serial, "gsm")


if __name__ == "__main__":
    unittest.main()
