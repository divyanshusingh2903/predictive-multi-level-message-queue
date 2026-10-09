import unittest

from analysis.models import Candidate, FeatureAdapter
from analysis.features import FeatureSchema
from analysis.tests.online_helpers import config, snapshot


class Models(unittest.TestCase):
    def test_statistics_and_category_backoff(self):
        spec = config()
        spec["group_min_labels"] = 2
        for name in ("global_mean", "job_mean", "job_ewma"):
            model = Candidate(name, spec)
            self.assertEqual(model.predict(snapshot(), "synthetic-job-v1").fallback, "unready")
            model.learn(snapshot(), "synthetic-job-v1", 0)
            model.learn(snapshot("class1"), "synthetic-job-v1", 10)
            self.assertEqual(model.predict(snapshot(), "synthetic-job-v1").duration_ms, 5)
            model.learn(snapshot(), "synthetic-job-v1", 4)
            expected = 14/3 if name == "global_mean" else 2 if name == "job_mean" else .2
            self.assertAlmostEqual(model.predict(snapshot(), "synthetic-job-v1").duration_ms, expected)
            self.assertEqual(model.predict(snapshot("new"), "synthetic-job-v1").duration_ms, 14/3)

    def test_fixed_indicator_zeros_distinct_missing_unknown_and_limits(self):
        adapter = FeatureAdapter(FeatureSchema.from_dict(config()["schema"]), 20)
        known = adapter.vector("synthetic-job-v1", snapshot())
        unknown = adapter.vector("synthetic-job-v1", snapshot("unseen"))
        missing = adapter.vector("synthetic-job-v1", snapshot(None))
        self.assertEqual(set(known), set(unknown))
        self.assertEqual(set(known), set(missing))
        self.assertNotEqual(known, unknown)
        self.assertNotEqual(unknown, missing)
        self.assertEqual(known[("header", "job", "category", "1")], 0)
        with self.assertRaises(ValueError):
            FeatureAdapter(adapter.schema, 2)

    def test_incompatibility_and_absent_features(self):
        model = Candidate("global_mean", config())
        self.assertEqual(model.predict(None, "wrong").fallback, "incompatible_version")
        self.assertEqual(model.predict(None, "synthetic-job-v1").fallback, "absent_features")
        self.assertEqual(model.predict({}, "synthetic-job-v1").fallback, "invalid_features")
        with self.assertRaises(ValueError):
            model.learn(snapshot(), "synthetic-job-v1", -1)
        spec = config(); spec["adapter_version"] = "unsupported"
        with self.assertRaisesRegex(ValueError, "preprocessing"):
            Candidate("global_mean", spec)

    def test_state_versions_include_configuration_run_scope_and_updates(self):
        first = Candidate("job_mean", config(), "dataset:run1")
        second = Candidate("job_mean", config(), "dataset:run2")
        self.assertNotEqual(first.version, second.version)
        previous = first.version
        first.learn(snapshot(), "synthetic-job-v1", 0)
        self.assertNotEqual(previous, first.version)
        self.assertLessEqual(len(first.version.encode()), 128)
