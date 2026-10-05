import copy
import math
import pickle
import unittest

from ml_engine.models import Candidate
from ml_engine.replay import replay
from ml_engine.tests.online_helpers import config, events, snapshot


class RiverModels(unittest.TestCase):
    def test_all_estimators_zero_duration_delayed_determinism(self):
        for name in ("linear_raw", "linear_log", "tree_raw", "tree_log", "adaptive_tree_raw"):
            with self.subTest(name=name):
                spec = config()
                a = replay(events(), Candidate(name, spec), spec)
                b = replay(events(), Candidate(name, spec), spec, profile=True)
                self.assertEqual(a["cohorts"], b["cohorts"])
                self.assertEqual(a["updates"], 4)
                self.assertGreater(b["resources"]["peak_model_state"]["accounted_state_bytes"], 0)

    def test_numeric_only_scaling_keeps_category_signal_and_prediction_causal(self):
        model = Candidate("linear_raw", config())
        for _ in range(500):
            model.learn(snapshot("class0"), "synthetic-job-v1", 1)
            model.learn(snapshot("class2"), "synthetic-job-v1", 20)
        self.assertLess(model.predict(snapshot("class0"), "synthetic-job-v1").duration_ms, 3)
        self.assertGreater(model.predict(snapshot("class2"), "synthetic-job-v1").duration_ms, 17)
        self.assertTrue(all(key[2] == "numeric" for key in model.scaler.counts))
        before = pickle.dumps(model.estimator), dict(model.scaler.counts), model.updates
        model.predict(snapshot("unseen"), "synthetic-job-v1")
        self.assertEqual(before, (pickle.dumps(model.estimator), dict(model.scaler.counts), model.updates))

    def test_tree_category_zero_completion_learns_different_jobs(self):
        spec = config(); spec["tree"]["grace_period"] = 20
        for name in ("tree_raw", "adaptive_tree_raw"):
            model = Candidate(name, spec)
            # Complementary one-hot coordinates tie in split merit; the frozen
            # delta/tau needs several thousand samples before breaking that tie.
            for _ in range(2000):
                model.learn(snapshot("class0"), "synthetic-job-v1", 1)
                model.learn(snapshot("class2"), "synthetic-job-v1", 20)
            self.assertLess(model.predict(snapshot("class0"), "synthetic-job-v1").duration_ms, 3)
            self.assertGreater(model.predict(snapshot("class2"), "synthetic-job-v1").duration_ms, 17)

    def test_invalid_regression_and_log_overflow_fallback(self):
        for name in ("linear_raw", "linear_log"):
            model = Candidate(name, config())
            model.learn(snapshot(), "synthetic-job-v1", 0)
            for intercept in (-100, math.nan, math.inf, 10000 if name.endswith("log") else -1):
                model.estimator.intercept = intercept
                self.assertEqual(model.predict(snapshot(), "synthetic-job-v1").fallback, "invalid_prediction")
