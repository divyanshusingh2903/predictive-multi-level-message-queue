import math
import unittest

from ml_engine.metrics import Quality, deep_size, paired_interval
from ml_engine.policy import Prediction


class Metrics(unittest.TestCase):
    def test_zero_missing_and_confusion_denominators(self):
        quality = Quality(3)
        for prediction, actual, bucket in ((Prediction(0, 0, None, 100), 0, 0),
                                           (Prediction(2, 0, None, 101), 8, 1),
                                           (Prediction(None, 1, "invalid_prediction", 102), 8, 1)):
            quality.predict(prediction)
            quality.score(prediction, actual, bucket)
        report = quality.report()
        self.assertEqual(report["eligible_labels"], 3)
        self.assertEqual(report["prediction_coverage"], 2/3)
        self.assertEqual(report["mae_ms"], 3)
        self.assertEqual(report["rmse_ms"], math.sqrt(18))
        self.assertEqual(report["severe_underestimate_rate"], 1)
        self.assertEqual(report["bucket_accuracy"], 2/3)
        self.assertIsNone(Quality(1).report()["mae_ms"])

    def test_seed_level_intervals_and_cycle_memory(self):
        self.assertEqual(paired_interval([1]*4, 200, 42)["status"], "insufficient_seeds")
        self.assertEqual(paired_interval([2]*5, 200, 42)["ci95"], [2, 2])
        self.assertEqual(paired_interval([1,2,3,4,5], 200, 42), paired_interval([1,2,3,4,5], 200, 42))
        cycle = []; cycle.append(cycle)
        self.assertGreater(deep_size(cycle), 0)
