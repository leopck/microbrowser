from __future__ import annotations

import os
import threading
from types import SimpleNamespace
import unittest

from tools.qemu_smoke import QemuProcess


class QemuProcessTests(unittest.TestCase):
    def test_selftest_outcome_arrives_across_reads(self) -> None:
        receive_fd, send_fd = os.pipe()
        with os.fdopen(receive_fd, "rb", buffering=0) as receiver, os.fdopen(send_fd, "wb", buffering=0) as sender:
            probe = QemuProcess.__new__(QemuProcess)
            probe.process = SimpleNamespace(stdout=receiver, returncode=None, poll=lambda: None)
            probe.output = bytearray()
            probe.timeout_s = 1.0
            sender.write(b"SELFTEST PAS")
            remainder = threading.Timer(0.05, sender.write, args=(b"S page=dashboard\n",))
            try:
                remainder.start()
                probe.wait_text((b"SELFTEST PASS", b"SELFTEST FAIL"))
            finally:
                remainder.cancel()
                remainder.join()
            self.assertEqual(bytes(probe.output), b"SELFTEST PASS page=dashboard\n")


if __name__ == "__main__":
    unittest.main()
