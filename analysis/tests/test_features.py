import copy
import json
from pathlib import Path
import struct
import unittest

from analysis.features import (
    FeatureError, FeatureSchema, HeaderFeature, MAX_FEATURE_BYTES,
    encode, extract_features, snapshot_json, validate_snapshot,
)


FIXTURES = Path(__file__).resolve().parents[2] / "tests/fixtures/ml/features_v1.json"


class FeatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = json.loads(FIXTURES.read_text(encoding="utf-8"))
        cls.schemas = {name: FeatureSchema.from_dict(spec) for name, spec in cls.fixtures["schemas"].items()}

    def test_shared_ingress_fixtures(self):
        for case in self.fixtures["cases"]:
            with self.subTest(case=case["id"]):
                schema = self.schemas[case["schema"]]
                headers = dict(case["headers"])
                headers.update({key: bytes.fromhex(value) for key, value in case.get("raw_hex", {}).items()})
                snapshot = extract_features(schema, int(case["payload_size_bytes"]), headers)
                self.assertEqual(snapshot, case["expected"])
                self.assertEqual(validate_snapshot(schema, schema.version, snapshot), snapshot)
                if "json" in case:
                    self.assertEqual(snapshot_json(snapshot), case["json"])
                self.assertNotIn("secret", snapshot_json(snapshot))
                self.assertNotIn("__producer_id", snapshot_json(snapshot))
                if "encoded" in case:
                    expected = {tuple(item["key"]): float(item["value"]) for item in case["encoded"]}
                    self.assertEqual(encode(schema, schema.version, snapshot), expected)

    def test_shared_binary64_fixtures(self):
        schema = self.schemas["numbers"]
        for case in self.fixtures["numeric_cases"]:
            with self.subTest(raw=case["input"]):
                snapshot = extract_features(schema, 0, {"n": case["input"]})
                if "reason" in case:
                    self.assertIsNone(snapshot["headers"]["n"])
                    self.assertEqual(snapshot["missing_reasons"], {"n": case["reason"]})
                else:
                    self.assertEqual(struct.pack(">d", snapshot["headers"]["n"]).hex(), case["bits"])
                    self.assertEqual(snapshot["missing_reasons"], {})
                    self.assertEqual(snapshot_json(snapshot),
                                     '{"payload_size_bytes":"0","headers":{"n":' + case["json_number"] + '},"missing_reasons":{}}')

    def test_schema_rejects_malformed_specs(self):
        for name in ("", "__producer_id", "x" * 65, "\ud800"):
            with self.subTest(name=name), self.assertRaises(FeatureError):
                HeaderFeature(name, "categorical")
        for version in ("", "x" * 129, "\ud800"):
            with self.subTest(version=version), self.assertRaises(FeatureError):
                FeatureSchema(version)
        for headers in ("abc", None, [HeaderFeature("h", "categorical")] * 2):
            with self.subTest(headers=headers), self.assertRaises(FeatureError):
                FeatureSchema("v1", headers)
        for vocabulary in ("abc", None, ["same", "same"], ["x" * 257], ["\ud800"]):
            with self.subTest(vocabulary=vocabulary), self.assertRaises(FeatureError):
                HeaderFeature("h", "categorical", vocabulary=vocabulary)
        for kwargs in (
            {"type": []}, {"type": "unknown"}, {"type": "numeric", "minimum": True},
            {"type": "numeric", "minimum": float("nan")},
            {"type": "numeric", "minimum": 2, "maximum": 1},
            {"type": "numeric", "vocabulary": ["bad"]},
            {"type": "categorical", "encoding": "unknown"},
            {"type": "categorical", "encoding": "hash", "vocabulary": ["bad"]},
            {"type": "categorical", "maximum": 1},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(FeatureError):
                HeaderFeature("h", **kwargs)
        with self.assertRaises(FeatureError):
            FeatureSchema.from_dict({"version": "v1", "headers": [{"name": "n", "type": "numeric"}]})

    def test_schema_owns_immutable_vocabulary(self):
        vocabulary = ["known", ""]
        headers = [HeaderFeature("h", "categorical", vocabulary=vocabulary)]
        schema = FeatureSchema("v1", headers)
        vocabulary[0] = "changed"
        headers.clear()
        self.assertEqual(schema.headers[0].vocabulary, ("known", ""))
        with self.assertRaises(AttributeError):
            schema.version = "changed"

    def test_limits_and_escape_expansion(self):
        vocabulary = [str(i) for i in range(1024)]
        HeaderFeature("h", "categorical", vocabulary=vocabulary)
        with self.assertRaises(FeatureError):
            HeaderFeature("h", "categorical", vocabulary=vocabulary + ["too-many"])
        fields = [HeaderFeature("k" * 64 if i == 0 else f"h{i}", "categorical") for i in range(16)]
        schema = FeatureSchema("v1", fields)
        with self.assertRaises(FeatureError):
            FeatureSchema("v1", fields + [HeaderFeature("extra", "categorical")])
        headers = {field.name: "x" * 256 for field in fields}
        self.assertIsNotNone(extract_features(schema, (1 << 64) - 1, headers))
        headers[fields[0].name] = b"\xff" * 257
        result = extract_features(schema, 0, headers)
        self.assertEqual(result["missing_reasons"][fields[0].name], "oversized")
        headers = {field.name: "\0" * 256 for field in fields}
        self.assertIsNone(extract_features(schema, 0, headers))

        boundary = {"payload_size_bytes": "0", "headers": {f"h{i}": "\0" * 192 for i in range(7)}, "missing_reasons": {}}
        boundary["headers"]["tail"] = ""
        remaining = MAX_FEATURE_BYTES - len(snapshot_json(boundary).encode("utf-8"))
        self.assertLessEqual(remaining, 256)
        boundary["headers"]["tail"] = "x" * remaining
        self.assertEqual(len(snapshot_json(boundary).encode("utf-8")), MAX_FEATURE_BYTES)
        boundary["headers"]["tail"] += "x"
        with self.assertRaisesRegex(FeatureError, "feature_limit"):
            snapshot_json(boundary)

    def test_shared_encoded_size_admission_boundaries(self):
        schema = FeatureSchema("v1", [HeaderFeature(f"h{i}", "categorical") for i in range(7)] +
                               [HeaderFeature("tail", "categorical")])
        for case in self.fixtures["size_boundaries"]:
            with self.subTest(case=case):
                headers = {f"h{i}": "\0" * 192 for i in range(7)}
                headers["tail"] = "x" * case["tail_bytes"]
                snapshot = extract_features(schema, 0, headers)
                self.assertEqual(snapshot is not None, case["accepted"])
                if snapshot is not None:
                    self.assertEqual(len(snapshot_json(snapshot).encode("utf-8")), case["expected_size"])

    def test_typed_snapshot_rejection(self):
        schema = self.schemas["main"]
        good = extract_features(schema, 1, {"job_type": "resize", "units": "2", "format": "png"})
        with self.assertRaisesRegex(FeatureError, "incompatible_version"):
            encode(schema, "other-schema", good)
        changes = [
            lambda s: s["headers"].update(authorization="secret"),
            lambda s: s["headers"].pop("format"),
            lambda s: s["headers"].update(units=True),
            lambda s: s["headers"].update(units=float("inf")),
            lambda s: s["headers"].update(units=1000001),
            lambda s: s["headers"].update(job_type="\ud800"),
            lambda s: s["headers"].update(job_type=None),
            lambda s: s["missing_reasons"].update(job_type="absent"),
            lambda s: s["missing_reasons"].update(unapproved="absent"),
        ]
        for change in changes:
            bad = copy.deepcopy(good)
            change(bad)
            with self.subTest(snapshot=bad), self.assertRaises(FeatureError):
                encode(schema, schema.version, bad)
        for size in ("-1", "01", "1.0", str(1 << 64), "9" * 1000, True, 1):
            bad = copy.deepcopy(good)
            bad["payload_size_bytes"] = size
            with self.subTest(size=size), self.assertRaises(FeatureError):
                validate_snapshot(schema, schema.version, bad)

    def test_namespaces_and_missing_unknown_are_distinct(self):
        schema = FeatureSchema("v1", [HeaderFeature("size_bytes", "categorical", vocabulary=("", "a")),
                                     HeaderFeature("category", "categorical", encoding="hash")])
        snapshot = extract_features(schema, 5, {"size_bytes": "unseen", "category": "resize"})
        self.assertEqual(encode(schema, "v1", snapshot), {
            ("payload", "size_bytes", "numeric", ""): 5.0,
            ("header", "size_bytes", "unknown", ""): 1.0,
            ("header", "category", "bin", "724"): 1.0,
        })
        missing = encode(schema, "v1", extract_features(schema, 5, {}))
        self.assertIn(("header", "size_bytes", "missing", ""), missing)
        self.assertNotIn(("header", "size_bytes", "unknown", ""), missing)

    def test_utf8_bytes_not_character_counts(self):
        schema = FeatureSchema("v1", [HeaderFeature("h", "categorical")])
        self.assertIsNotNone(extract_features(schema, 0, {"h": "😀" * 64})["headers"]["h"])
        self.assertEqual(extract_features(schema, 0, {"h": "😀" * 65})["missing_reasons"], {"h": "oversized"})
        for raw in (b"\xf4\x90\x80\x80", b"\xe2\x82", b"\xed\xa0\x80", b"\x80"):
            self.assertEqual(extract_features(schema, 0, {"h": raw})["missing_reasons"], {"h": "invalid_encoding"})


if __name__ == "__main__":
    unittest.main()
