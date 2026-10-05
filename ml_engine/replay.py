"""Stored-ingress-prediction scoring with independent-run delayed feedback."""
from __future__ import annotations

from dataclasses import asdict
import time

from .dataset import identity
from .metrics import Quality, deep_size, model_memory, quantiles
from .models import Candidate


def replay(events, candidate: Candidate, config: dict, emit=None, profile: bool = False) -> dict:
    pending = {}
    cohorts = {}
    prediction_times, update_times = [], []
    peak_pending = peak_pending_bytes = 0
    memory = model_memory(candidate) if profile else None
    last_time, last_kind, seen_feedback = -1, None, set()
    completed = set()
    evaluation_ingress = 0
    # Outcomes are already deduplicated by the dataset index. This run-local set
    # fences alternate callers and is bounded by the frozen finite run size.
    def metric(name):
        if name not in cohorts:
            cohorts[name] = Quality(candidate.policy.levels)
        return cohorts[name]

    for event in events:
        now = event["benchmark_time_ns"]
        if now < last_time:
            raise ValueError("replay input is not chronological")
        if now == last_time and last_kind == "outcome" and event["event_type"] == "ingress":
            raise ValueError("equal-time ingress must precede feedback")
        last_time, last_kind = now, event["event_type"]
        key = identity(event)
        if event["event_type"] == "ingress":
            if key in pending or key in completed:
                raise ValueError("duplicate ingress")
            start = time.perf_counter_ns() if profile else 0
            prediction = candidate.predict(event["routing"]["features"], event["routing"]["feature_schema_version"])
            if profile and candidate.updates >= candidate.policy.ready:
                prediction_times.append((time.perf_counter_ns() - start) / 1e6)
            names = [event["split"]]
            if event["split"] == "evaluation":
                names.append(f"evaluation_window:{evaluation_ingress // 100}")
                evaluation_ingress += 1
                if candidate.updates >= candidate.policy.ready:
                    names.append("evaluation_warm")
                header = event["routing"]["features"]
                if header is not None:
                    names.append("job:" + candidate.adapter.group(header, config["group_header"]))
                # Synthetic sequence is used only to define reporting cohorts, never a model input.
                shift_time = config.get("shift_time_ns")
                if shift_time is not None:
                    names.append("post_shift" if now >= shift_time else "pre_shift")
            windows = config["warmup_label_windows"]
            low = max(n for n in windows if candidate.updates >= n)
            names.append(f"updates:{low}")
            for name in names:
                metric(name).predict(prediction)
            if event["split"] in ("train", "evaluation"):
                if len(pending) >= config["pending_limit"]:
                    raise ValueError("pending prediction limit exceeded; comparison invalid")
                pending[key] = (event, prediction, names)
                peak_pending = max(peak_pending, len(pending))
                if profile and (len(pending) == peak_pending):
                    peak_pending_bytes = max(peak_pending_bytes, deep_size(pending))
            if emit:
                emit({"kind": "prediction", "run": key[0], "broker_instance_id": key[1], "message_id": key[2],
                      "split": event["split"], "arrival_ns": now, "model_version": candidate.version,
                      "policy_version": candidate.policy.version, **asdict(prediction)})
            continue

        feedback_key = (event["run"], event["event_id"])
        if feedback_key in seen_feedback:
            continue
        seen_feedback.add(feedback_key)
        if len(seen_feedback) > config["pending_limit"] * 4:
            raise ValueError("feedback identity limit exceeded; comparison invalid")
        stored = pending.get(key)
        if (event["split"] in ("train", "evaluation") and stored is None and
                (key not in completed or event["label_status"] == "eligible")):
            raise ValueError("feedback without stored ingress")
        if event["label_status"] == "eligible" and event["split"] in ("train", "evaluation"):
            ingress, prediction, names = stored
            if ingress["routing"]["features"] != event["routing"]["features"]:
                raise ValueError("label features differ from ingress")
            actual = event["processing_time_ms"]
            for name in names:
                metric(name).score(prediction, actual, candidate.policy.bucket(actual))
            if emit:
                emit({"kind": "score", "run": key[0], "broker_instance_id": key[1], "message_id": key[2],
                      "attempt_id": event["attempt_id"], "label_available_ns": now, "actual_ms": actual,
                      "ingress_prediction": asdict(prediction), "model_updates_before_learning": candidate.updates})
            start = time.perf_counter_ns() if profile else 0
            candidate.learn(ingress["routing"]["features"], ingress["routing"]["feature_schema_version"], actual)
            if profile:
                update_times.append((time.perf_counter_ns() - start) / 1e6)
                if candidate.updates % config["profiling"]["memory_every_labels"] == 0:
                    measured = model_memory(candidate)
                    if measured["accounted_state_bytes"] > memory["accounted_state_bytes"]:
                        memory = measured
        if event["outcome"] in ("ack", "dlq"):
            pending.pop(key, None)
            completed.add(key)
            if len(completed) > config["pending_limit"] * 4:
                raise ValueError("completion identity limit exceeded; comparison invalid")
    if pending:
        raise ValueError("missing terminal feedback at replay end")
    if profile:
        measured = model_memory(candidate)
        if measured["accounted_state_bytes"] > memory["accounted_state_bytes"]:
            memory = measured
    tree = candidate.estimator
    return {"model_version": candidate.version, "model_configuration_version": candidate.configuration_version,
            "updates": candidate.updates,
            "cohorts": {name: quality.report() for name, quality in sorted(cohorts.items())},
            "peak_pending": peak_pending, "peak_pending_bytes": peak_pending_bytes if profile else None,
            "feedback_identity_bytes": deep_size((seen_feedback, completed)) if profile else None,
            "resources": {"warm_prediction_ms": quantiles(prediction_times), "update_ms": quantiles(update_times),
                          "peak_model_state": memory} if profile else None,
            "tree": {name: getattr(tree, name) for name in
                     ("n_nodes", "height", "n_alternate_trees", "n_switch_alternate_trees", "n_pruned_alternate_trees")
                     if hasattr(tree, name)} if tree is not None else None}
