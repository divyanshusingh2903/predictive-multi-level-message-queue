import unittest
from benchmarks.workloads import generate, canonical_hash


class Workloads(unittest.TestCase):
    def test_seed_bounds_controls_and_shift(self):
        for distribution in ("uniform", "bimodal", "heavy_tail"):
            spec = {"distribution": distribution, "count": 500, "rate_per_s": 100,
                    "min_cost_us": 100, "cap_cost_us": 10000}
            rows = generate(spec, 42)
            self.assertEqual(rows, generate(spec, 42))
            self.assertNotEqual(canonical_hash(rows), canonical_hash(generate(spec, 43)))
            control = generate({**spec, "informative": False}, 42)
            shifted = generate({**spec, "shift_index": 250}, 42)
            for a, b, c in zip(rows, control, shifted):
                self.assertTrue(100 <= a["cost_us"] <= 10000)
                for key in ("arrival_us", "cost_us", "failures", "abandons"):
                    self.assertEqual(a[key], b[key]); self.assertEqual(a[key], c[key])
            self.assertEqual(rows[:250], shifted[:250])
            self.assertNotEqual(rows[250:], shifted[250:])

    def test_invalid_and_burst(self):
        spec = {"distribution": "uniform", "count": 20, "rate_per_s": 100, "burst_size": 4}
        rows = generate(spec, 1)
        self.assertEqual([row["arrival_us"] for row in rows[:4]], [0] * 4)
        for change in ({"rate_per_s": float("inf")}, {"count": 0}, {"cap_cost_us": -1},
                       {"distribution": "normal"}, {"shift_index": 30}, {"failure_probability": -1}):
            with self.assertRaises(ValueError): generate({**spec, **change}, 1)


if __name__ == "__main__": unittest.main()
