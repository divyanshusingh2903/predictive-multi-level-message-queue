import random
import unittest

from ml_engine.trace_analysis import analyse


def rows(seed=1, n=6000):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        key = rng.choice(["fast", "mid", "slow", "wild"])
        base = {"fast": 2, "mid": 40, "slow": 900}.get(key)
        duration = base * rng.uniform(0.8, 1.25) if base else (5000 if rng.random() < 0.15 else 1)  # rare long mode
        out.append((float(i), key, duration))
    return out


class TraceAnalysis(unittest.TestCase):
    def test_per_key_beats_global_when_keys_are_informative(self):
        result = analyse(rows())
        held = result["held_out"]
        self.assertLess(held["per_key_median_all_keys"]["median_abs_log2_error"],
                        held["global_median"]["median_abs_log2_error"])
        self.assertGreater(held["per_key_median_with_gates"]["tier_accuracy"], held["global_median"]["tier_accuracy"])
        # The bimodal key fails the spread gate, the three tight ones pass.
        self.assertEqual(result["keys_routable"], 3)
        self.assertAlmostEqual(result["test_share_unseen_key"], 0.0)

    def test_split_is_time_ordered_and_unseen_keys_counted(self):
        data = rows(n=1000) + [(2000.0 + i, "late", 7.0) for i in range(200)]
        result = analyse(data)
        self.assertGreater(result["test_share_unseen_key"], 0.2)
        with self.assertRaises(ValueError):
            analyse(rows(n=50))


if __name__ == "__main__":
    unittest.main()
