import unittest
from benchmarks.evaluate import metrics, quantiles, paired_interval, aggregate
from benchmarks.export_feedback import partition


class Accounting(unittest.TestCase):
    def test_nearest_rank_and_empty(self):
        self.assertEqual(quantiles([1, 2, 3, 4])["p50"], 2)
        self.assertEqual(quantiles([1, 2, 3, 4])["p95"], 4)
        self.assertIsNone(quantiles([])["p99"])

    def test_expired_unfinished_and_never_dispatched_count(self):
        jobs = [{"sequence": i, "arrival_us": 0, "job": "class0"} for i in range(3)]
        observations = [{"sequence": i, "attempt": 0, "arrival_ns": 0, "time_ns": 0,
                         "retries": 0, "kind": "ingress", "message_id": str(i)} for i in range(3)]
        observations += [{**observations[0], "time_ns": 10000000, "kind": "ttl"}]
        submissions = [{"sequence": i, "message_id": str(i), "status": 0, "start_ns": 1, "end_ns": 2} for i in range(3)]
        summary = {"cutoff_ns": 100000000, "observation_drops": 0, "queued_at_cutoff": 2, "in_flight_at_cutoff": 0, "pull_errors": 0}
        measured, records = metrics(jobs, observations, submissions, [], summary, 0, 50)
        self.assertEqual(measured["counts"]["ttl"], 1)
        self.assertEqual(measured["counts"]["unfinished"], 2)
        self.assertEqual(measured["classes"]["class0"]["starved"], 2)
        # Terminal never-dispatched TTL messages use their terminal age, not the later drain cutoff.
        self.assertIsNone(records[0]["first_dispatch_wait_ms"])
        self.assertEqual(measured["terminal_latency_ms"]["p99"], 10)
        self.assertEqual(measured["unfinished_age_ms"]["count"], 2)
        self.assertFalse(measured["full_drain"])

    def test_grouped_temporal_embargo(self):
        self.assertEqual(partition(10, [20, 90], 100), "train")
        self.assertEqual(partition(10, [20, 110], 100), "embargo")
        self.assertEqual(partition(100, [150], 100), "evaluation")
        self.assertEqual(partition(100, [], 100), "evaluation")
        self.assertEqual(partition(10, [], 100), "unresolved")
        self.assertEqual(partition(None, [150], 100), "unresolved")

    def test_repeat_means_not_pseudoreplicates(self):
        runs = [{"case": "c", "seed": seed, "policy": policy, "repeat": repeat,
                 "metrics": {"valid": True, "success_per_s": value,
                             "terminal_latency_ms": {"p50": value, "p95": value, "p99": value}}}
                for seed in (1, 2) for policy, repeat, value in (("static", 0, 10), ("static", 1, 20), ("fifo", 0, 5), ("fifo", 1, 15))]
        result = aggregate(runs, 100, 1)[0]
        self.assertEqual(result["paired_seeds"], 2)
        self.assertEqual(result["mean"], 5)
        self.assertEqual(paired_interval([], 100, 1)["ci95"], None)


if __name__ == "__main__": unittest.main()
