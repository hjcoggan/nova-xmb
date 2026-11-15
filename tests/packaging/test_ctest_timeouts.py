"""Every ctest test must bound its runtime so a hung test fails fast."""
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE_LISTS = ROOT / 'CMakeLists.txt'


def add_test_names(text):
    return set(re.findall(r'add_test\s*\(\s*NAME\s+([A-Za-z0-9_${}]+)', text))


def timeout_covered_names(text):
    covered = {}
    for match in re.finditer(
            r'set_tests_properties\s*\((.*?)\)', text, re.DOTALL):
        body = match.group(1)
        names, _, props = body.partition('PROPERTIES')
        timeout = re.search(r'TIMEOUT\s+(\d+)', props)
        if timeout:
            for name in names.split():
                covered[name] = int(timeout.group(1))
    return covered


class CTestTimeoutTests(unittest.TestCase):
    def test_every_test_has_a_timeout(self):
        text = CMAKE_LISTS.read_text()
        defined = add_test_names(text)
        self.assertTrue(defined, 'no add_test entries found')
        covered = timeout_covered_names(text)
        missing = sorted(defined - set(covered))
        self.assertEqual(missing, [], f'tests without TIMEOUT: {missing}')

    def test_timeouts_are_sane_bounds(self):
        covered = timeout_covered_names(CMAKE_LISTS.read_text())
        self.assertTrue(covered, 'no TIMEOUT properties found')
        for name, seconds in sorted(covered.items()):
            self.assertTrue(1 <= seconds <= 600, f'{name} TIMEOUT {seconds}')

    def test_timeouts_name_real_tests(self):
        text = CMAKE_LISTS.read_text()
        defined = add_test_names(text)
        covered = timeout_covered_names(text)
        orphaned = sorted(set(covered) - defined)
        self.assertEqual(orphaned, [], f'TIMEOUT on unknown tests: {orphaned}')


if __name__ == '__main__':
    unittest.main()
