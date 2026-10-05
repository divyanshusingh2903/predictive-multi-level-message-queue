"""Score the broker's C++ per-key predictor with the offline harness metrics."""
from __future__ import annotations

from bisect import bisect_right
import json
from pathlib import Path
import subprocess
import tempfile

from .metrics import Quality
from .policy import Prediction


def run_replay(binary: str | Path, export: Path, args: list[str] | None = None) -> tuple[dict, dict]:
    """Run harbinger_predictor_replay; return per-message predictions and per-run summaries."""
    with tempfile.TemporaryDirectory(prefix="harbinger-cpp-replay-") as scratch:
        output = Path(scratch) / "predictions.jsonl"
        subprocess.run([str(binary), "--export", str(export), "--output", str(output), *(args or [])],
                       check=True, capture_output=True, text=True)
        predictions, summaries = {}, {}
        for line in output.read_text().splitlines():
            row = json.loads(line)
            if row["kind"] == "prediction":
                key = (row["run"], row["broker_instance_id"], row["message_id"])
                if key in predictions:
                    raise ValueError("duplicate prediction")
                predictions[key] = row
            elif row["kind"] == "summary":
                summaries[row["run"]] = row
    return predictions, summaries


def score(events, predictions: dict, summaries: dict, levels: int) -> dict:
    """Score stored ingress predictions against eligible evaluation labels, per run then pooled."""
    cohorts: dict[str, Quality] = {}
    pending = {}
    for event in events:
        key = (event["run"], event["broker_instance_id"], event["message_id"])
        if event["event_type"] == "ingress":
            row = predictions.get(key)
            if row is None:
                raise ValueError("missing C++ prediction for ingress")
            prediction = Prediction(row["duration_ms"], row["bucket"], row["fallback"], row["updates"])
            pending[key] = prediction
            if event["split"] == "evaluation":
                cohorts.setdefault("evaluation", Quality(levels)).predict(prediction)
            continue
        if event["label_status"] != "eligible" or event["split"] != "evaluation":
            continue
        prediction = pending.get(key)
        if prediction is None:
            raise ValueError("label without ingress prediction")
        actual = event["processing_time_ms"]
        boundaries = summaries[event["run"]]["boundaries_ms"]
        cohorts.setdefault("evaluation", Quality(levels)).score(prediction, actual, bisect_right(boundaries, actual))
    return {name: quality.report() for name, quality in sorted(cohorts.items())}
