"""Bounded schema validation, ingress fixtures, and sparse model encoding."""

from dataclasses import dataclass
import hashlib
import math
import re
from typing import Mapping

MAX_HEADERS = 16
MAX_KEY_BYTES = 64
MAX_VALUE_BYTES = 256
MAX_VOCABULARY = 1024
MAX_FEATURE_BYTES = 8192
MAX_UINT64 = (1 << 64) - 1
DECIMAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?", re.ASCII)
MISSING_REASONS = frozenset({"absent", "invalid_type", "out_of_range", "invalid_encoding", "oversized"})


class FeatureError(ValueError):
    """Invalid schema, incompatible snapshot, or resource limit violation."""


def _utf8(value: str, cap: int, *, nonempty: bool = False) -> None:
    if not isinstance(value, str) or len(value) > cap or (nonempty and not value):
        raise FeatureError("invalid bounded string")
    try:
        if len(value.encode("utf-8")) > cap:
            raise FeatureError("oversized UTF-8 string")
    except UnicodeEncodeError as exc:
        raise FeatureError("invalid UTF-8 string") from exc


def _number(value: object) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise FeatureError("expected binary64 number")
    try:
        number = float(value)
    except OverflowError as exc:
        raise FeatureError("out-of-range number") from exc
    if not math.isfinite(number):
        raise FeatureError("nonfinite number")
    return 0.0 if number == 0 else number


@dataclass(frozen=True)
class HeaderFeature:
    name: str
    type: str
    minimum: float = 0.0
    maximum: float = 0.0
    encoding: str = "vocabulary"
    vocabulary: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        _utf8(self.name, MAX_KEY_BYTES, nonempty=True)
        if self.name.startswith("__"):
            raise FeatureError("reserved header")
        if not isinstance(self.type, str) or not isinstance(self.encoding, str):
            raise FeatureError("invalid feature type or encoding")
        if not isinstance(self.vocabulary, (list, tuple)):
            raise FeatureError("vocabulary must be a list or tuple")
        if len(self.vocabulary) > MAX_VOCABULARY:
            raise FeatureError("vocabulary too large")
        object.__setattr__(self, "vocabulary", tuple(self.vocabulary))
        if self.type == "numeric":
            minimum, maximum = _number(self.minimum), _number(self.maximum)
            if minimum > maximum or self.vocabulary or self.encoding != "vocabulary":
                raise FeatureError("invalid numeric specification")
            object.__setattr__(self, "minimum", minimum)
            object.__setattr__(self, "maximum", maximum)
        elif self.type == "categorical":
            if _number(self.minimum) != 0 or _number(self.maximum) != 0:
                raise FeatureError("categorical field cannot specify numeric range")
            if self.encoding not in {"vocabulary", "hash"} or (self.encoding == "hash" and self.vocabulary):
                raise FeatureError("invalid categorical encoding")
            for value in self.vocabulary:
                _utf8(value, MAX_VALUE_BYTES)
            if len(set(self.vocabulary)) != len(self.vocabulary):
                raise FeatureError("duplicate vocabulary value")
        else:
            raise FeatureError("unknown feature type")


@dataclass(frozen=True)
class FeatureSchema:
    version: str
    headers: tuple[HeaderFeature, ...] = ()

    def __post_init__(self) -> None:
        _utf8(self.version, 128, nonempty=True)
        if not isinstance(self.headers, (list, tuple)):
            raise FeatureError("headers must be a list or tuple")
        if len(self.headers) > MAX_HEADERS:
            raise FeatureError("too many feature headers")
        object.__setattr__(self, "headers", tuple(self.headers))
        if not all(isinstance(field, HeaderFeature) for field in self.headers):
            raise FeatureError("invalid header specification")
        if len({field.name for field in self.headers}) != len(self.headers):
            raise FeatureError("duplicate header key")

    @classmethod
    def from_dict(cls, specification: dict) -> "FeatureSchema":
        if not isinstance(specification, dict) or len(specification) != 2 or set(specification) != {"version", "headers"}:
            raise FeatureError("invalid schema specification")
        headers = specification["headers"]
        if not isinstance(headers, list) or len(headers) > MAX_HEADERS:
            raise FeatureError("invalid header specifications")
        fields = []
        for field in headers:
            if not isinstance(field, dict) or len(field) > 6 or not {"name", "type"} <= set(field):
                raise FeatureError("invalid field specification")
            allowed = {"name", "type", "minimum", "maximum", "encoding", "vocabulary"}
            if not set(field) <= allowed:
                raise FeatureError("unknown field specification")
            if field["type"] == "numeric" and not {"minimum", "maximum"} <= set(field):
                raise FeatureError("numeric range must be explicit")
            vocabulary = field.get("vocabulary", [])
            if not isinstance(vocabulary, list):
                raise FeatureError("invalid vocabulary")
            fields.append(HeaderFeature(**field))
        return cls(specification["version"], tuple(fields))


def _payload_size(value: object) -> int:
    if not isinstance(value, str) or len(value) > 20 or not re.fullmatch(r"0|[1-9][0-9]*", value, re.ASCII):
        raise FeatureError("invalid exact payload size")
    size = int(value)
    if size > MAX_UINT64:
        raise FeatureError("payload size outside uint64")
    return size


def snapshot_json(snapshot: dict) -> str:
    """Bounded deterministic feature-object JSON matching the C++ serializer."""
    if not isinstance(snapshot, dict) or len(snapshot) != 3 or set(snapshot) != {"payload_size_bytes", "headers", "missing_reasons"}:
        raise FeatureError("invalid snapshot fields")
    _payload_size(snapshot["payload_size_bytes"])
    values, reasons = snapshot["headers"], snapshot["missing_reasons"]
    if not isinstance(values, dict) or not isinstance(reasons, dict) or len(values) > MAX_HEADERS or len(reasons) > MAX_HEADERS:
        raise FeatureError("invalid feature maps")
    for key, value in values.items():
        _utf8(key, MAX_KEY_BYTES, nonempty=True)
        if key.startswith("__"):
            raise FeatureError("reserved snapshot key")
        if value is None:
            if not isinstance(reasons.get(key), str) or reasons[key] not in MISSING_REASONS:
                raise FeatureError("null feature requires missing reason")
        elif key in reasons:
            raise FeatureError("present feature has missing reason")
        elif isinstance(value, str):
            _utf8(value, MAX_VALUE_BYTES)
        else:
            _number(value)
    if not set(reasons) <= set(values):
        raise FeatureError("unknown missing key")

    parts, size = [], 0

    def add(text: str) -> None:
        nonlocal size
        size += len(text.encode("utf-8"))
        if size > MAX_FEATURE_BYTES:
            raise FeatureError("feature_limit")
        parts.append(text)

    def quoted(text: str) -> None:
        add('"')
        for char in text:
            if char == '"':
                add('\\"')
            elif char == "\\":
                add("\\\\")
            elif ord(char) < 32:
                add(f"\\u{ord(char):04x}")
            else:
                add(char)
        add('"')

    add('{"payload_size_bytes":')
    quoted(snapshot["payload_size_bytes"])
    add(',"headers":{')
    for index, key in enumerate(sorted(values)):
        if index:
            add(",")
        quoted(key)
        add(":")
        value = values[key]
        if value is None:
            add("null")
        elif isinstance(value, str):
            quoted(value)
        else:
            add(format(_number(value), ".16e"))
    add('},"missing_reasons":{')
    for index, key in enumerate(sorted(reasons)):
        if index:
            add(",")
        quoted(key)
        add(":")
        quoted(reasons[key])
    add("}}")
    return "".join(parts)


def validate_snapshot(schema: FeatureSchema, version: str, snapshot: dict) -> dict:
    """Validate a typed ingress snapshot against an explicitly registered schema."""
    if version != schema.version:
        raise FeatureError("incompatible_version")
    if not isinstance(snapshot, dict) or len(snapshot) != 3 or set(snapshot) != {"payload_size_bytes", "headers", "missing_reasons"}:
        raise FeatureError("invalid snapshot fields")
    _payload_size(snapshot["payload_size_bytes"])
    values, reasons = snapshot["headers"], snapshot["missing_reasons"]
    if not isinstance(values, dict) or not isinstance(reasons, dict):
        raise FeatureError("invalid feature maps")
    if len(values) > MAX_HEADERS or len(reasons) > MAX_HEADERS:
        raise FeatureError("too many feature keys")
    names = {field.name for field in schema.headers}
    if set(values) != names or not set(reasons) <= names:
        raise FeatureError("unapproved or missing feature keys")
    checked = {}
    for field in schema.headers:
        value = values[field.name]
        if value is None:
            if not isinstance(reasons.get(field.name), str) or reasons[field.name] not in MISSING_REASONS:
                raise FeatureError("null feature requires a known missing reason")
        else:
            if field.name in reasons:
                raise FeatureError("present feature has missing reason")
            if field.type == "numeric":
                value = _number(value)
                if not field.minimum <= value <= field.maximum:
                    raise FeatureError("numeric feature outside schema range")
            else:
                _utf8(value, MAX_VALUE_BYTES)
        checked[field.name] = value
    result = {"payload_size_bytes": snapshot["payload_size_bytes"], "headers": checked,
              "missing_reasons": dict(reasons)}
    snapshot_json(result)
    return result


def extract_features(schema: FeatureSchema, payload_size_bytes: int,
                     headers: Mapping[str, str | bytes]) -> dict | None:
    """Reference extraction for shared fixtures; production extraction is C++."""
    if isinstance(payload_size_bytes, bool) or not isinstance(payload_size_bytes, int) or not 0 <= payload_size_bytes <= MAX_UINT64:
        raise FeatureError("invalid payload size")
    values, reasons = {}, {}
    for field in schema.headers:
        raw = headers.get(field.name)
        reason = None
        value = None
        if raw is None:
            reason = "absent"
        else:
            if isinstance(raw, str):
                if len(raw) > MAX_VALUE_BYTES:
                    reason = "oversized"
                else:
                    try:
                        raw = raw.encode("utf-8")
                    except UnicodeEncodeError:
                        reason = "invalid_encoding"
            if reason is None:
                if not isinstance(raw, bytes):
                    reason = "invalid_type"
                elif len(raw) > MAX_VALUE_BYTES:
                    reason = "oversized"
                else:
                    try:
                        text = raw.decode("utf-8")
                    except UnicodeDecodeError:
                        reason = "invalid_encoding"
                    else:
                        if field.type == "categorical":
                            value = text
                        elif not DECIMAL.fullmatch(text):
                            reason = "invalid_type"
                        else:
                            value = float(text)
                            nonzero = any(c in "123456789" for c in re.split("[eE]", text)[0])
                            if not math.isfinite(value) or (nonzero and value == 0) or not field.minimum <= value <= field.maximum:
                                value, reason = None, "out_of_range"
                            elif value == 0:
                                value = 0.0
        values[field.name] = value
        if reason:
            reasons[field.name] = reason
    result = {"payload_size_bytes": str(payload_size_bytes), "headers": values, "missing_reasons": reasons}
    try:
        snapshot_json(result)
    except FeatureError:
        return None
    return result


def encode(schema: FeatureSchema, version: str, snapshot: dict) -> dict[tuple[str, str, str, str], float]:
    """Encode into sparse features with collision-free structured namespaces."""
    validated = validate_snapshot(schema, version, snapshot)
    encoded = {("payload", "size_bytes", "numeric", ""): float(_payload_size(validated["payload_size_bytes"]))}
    for field in schema.headers:
        value = validated["headers"][field.name]
        if value is None:
            key, number = ("header", field.name, "missing", ""), 1.0
        elif field.type == "numeric":
            key, number = ("header", field.name, "numeric", ""), value
        elif field.encoding == "hash":
            index = int.from_bytes(hashlib.sha256(value.encode("utf-8")).digest()[:8], "big") % 1024
            key, number = ("header", field.name, "bin", str(index)), 1.0
        elif value in field.vocabulary:
            key, number = ("header", field.name, "category", str(field.vocabulary.index(value))), 1.0
        else:
            key, number = ("header", field.name, "unknown", ""), 1.0
        encoded[key] = number
    return encoded
