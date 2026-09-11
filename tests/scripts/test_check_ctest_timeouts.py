"""Regression cases for the CTest timeout coverage audit."""

import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from check_ctest_timeouts import timeout_violations


def timeout_test(name, value):
    return {"name": name, "properties": [{"name": "TIMEOUT", "value": value}]}


def payload(*tests):
    return {"tests": list(tests)}


class CTestTimeoutCoverageTests(unittest.TestCase):
    def test_missing_timeout_is_rejected(self):
        missing, invalid = timeout_violations(payload(
            {"name": "missing", "properties": []},
            {"name": "also-missing", "properties": [{"name": "LABELS", "value": "x"}]},
        ))
        self.assertEqual(missing, ["missing", "also-missing"])
        self.assertEqual(invalid, [])

    def test_invalid_timeout_values_are_rejected(self):
        missing, invalid = timeout_violations(payload(
            timeout_test("text", "not-a-number"),
            timeout_test("none", None),
            timeout_test("zero", 0),
            timeout_test("negative", "-1"),
        ))
        self.assertEqual(missing, [])
        self.assertEqual(invalid, [
            ("text", "not-a-number"),
            ("none", None),
            ("zero", 0),
            ("negative", "-1"),
        ])

    def test_non_finite_timeout_values_are_rejected(self):
        missing, invalid = timeout_violations(payload(
            timeout_test("nan-string", "NaN"),
            timeout_test("inf-string", "inf"),
            timeout_test("neg-inf-string", "-inf"),
            timeout_test("nan-float", math.nan),
            timeout_test("inf-float", math.inf),
        ))
        self.assertEqual(missing, [])
        self.assertEqual([name for name, _ in invalid], [
            "nan-string",
            "inf-string",
            "neg-inf-string",
            "nan-float",
            "inf-float",
        ])
        self.assertEqual(invalid[0][1], "NaN")
        self.assertEqual(invalid[1][1], "inf")
        self.assertEqual(invalid[2][1], "-inf")
        self.assertTrue(math.isnan(invalid[3][1]))
        self.assertEqual(invalid[4][1], math.inf)

    def test_positive_finite_timeout_values_pass(self):
        missing, invalid = timeout_violations(payload(
            timeout_test("integer-string", "30"),
            timeout_test("fraction-string", "0.5"),
            timeout_test("integer", 60),
            timeout_test("float", 1.25),
            timeout_test("scientific", "1e3"),
        ))
        self.assertEqual(missing, [])
        self.assertEqual(invalid, [])


if __name__ == "__main__":
    unittest.main()
