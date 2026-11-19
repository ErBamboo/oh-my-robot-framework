import os
import socket
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import server


class PortGuardTest(unittest.TestCase):
    def test_listening_port_is_detected(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            sock.listen(1)
            port = sock.getsockname()[1]
            self.assertTrue(server.port_in_use("127.0.0.1", port))

    def test_unused_port_is_free(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        self.assertFalse(server.port_in_use("127.0.0.1", port))


class ContractInfoTest(unittest.TestCase):
    def test_server_identity_is_exposed(self):
        info = server.contract_info()
        self.assertEqual(info["server"]["pid"], os.getpid())
        self.assertTrue(info["server"]["startedAt"])


if __name__ == "__main__":
    unittest.main()
