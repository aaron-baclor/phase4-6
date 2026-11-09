"""Regression: real console interleaving must not invalidate a completed run."""
import unittest
import re
from tests.qemu_tools import ROOT, user_console, kernel_records, RECORD


class ConsoleTests(unittest.TestCase):
    def test_actual_interrupted_completion(self):
        path = ROOT / 'tests/fixtures/interleaved-console.log'
        self.assertIn('DONE|test_cpu', user_console(path.read_text()))
        self.assertTrue(any(line.startswith('TRACE|EXIT|3|') for line in kernel_records(path.read_text())))

    def test_kernel_inserts_do_not_hide_user_failure(self):
        raw = 'FATRACE|TICK|4|10|0\nIL: bad result\n'
        self.assertIn('FAIL: bad result', user_console(raw))

    def test_records_with_user_prefix(self):
        raw = 'W42|GROUP|0(100,7),[4]test:4(2,3)\nORKLOAD|test|4\n'
        self.assertEqual(user_console(raw), 'WORKLOAD|test|4\n')
        self.assertEqual(kernel_records(raw), ['42|GROUP|0(100,7),[4]test:4(2,3)'])

    def test_interrupted_shell_prompt(self):
        raw = '$TRACE|TICK|2|2|0\r\n '
        self.assertIsNotNone(re.fullmatch(r'\$(?:' + RECORD.pattern + r')* ', raw))


if __name__ == '__main__':
    unittest.main()
