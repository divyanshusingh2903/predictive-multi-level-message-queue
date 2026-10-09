import math
import unittest

from analysis.policy import DurationPolicy
from analysis.tests.online_helpers import config


class Policy(unittest.TestCase):
    def test_equality_zero_and_levels(self):
        policy = DurationPolicy(config()["policy"])
        self.assertEqual([policy.bucket(v) for v in (0, 2.99, 3, 9.99, 10, 100)], [0, 0, 1, 1, 2, 2])
        one = DurationPolicy({"version": "one", "num_levels": 1, "default_priority": 0, "boundaries_ms": [], "ready_labels": 1})
        self.assertEqual(one.bucket(100), 0)
        maximum = DurationPolicy({"version": "maximum", "num_levels": 255, "default_priority": 254,
                                  "boundaries_ms": list(range(1, 255)), "ready_labels": 1})
        self.assertEqual(maximum.bucket(254), 254)

    def test_reject_invalid_boundaries_and_fallback(self):
        for change in ({"boundaries_ms": [3, 3]}, {"boundaries_ms": [0, 10]}, {"boundaries_ms": [3, math.inf]},
                       {"num_levels": True}, {"default_priority": 3}, {"ready_labels": 0}):
            with self.assertRaises(ValueError):
                DurationPolicy({**config()["policy"], **change})
        policy = DurationPolicy(config()["policy"])
        for value in (-1, math.inf, math.nan, None, True):
            self.assertEqual(policy.result(value, 100).fallback, "invalid_prediction")
        self.assertEqual(policy.result(0, 0).fallback, "unready")
        self.assertEqual(policy.result(0, 100).duration_ms, 0)
