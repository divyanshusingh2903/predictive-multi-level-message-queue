#include "ml/features.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace harbinger::ml {
namespace {

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count;
        uint32_t code;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

bool digit(char c) { return c >= '0' && c <= '9'; }

bool decimal_grammar(std::string_view text) {
    std::size_t i = 0;
    if (i < text.size() && text[i] == '-') ++i;
    if (i == text.size()) return false;
    if (text[i] == '0') ++i;
    else {
        if (text[i] < '1' || text[i] > '9') return false;
        while (i < text.size() && digit(text[i])) ++i;
    }
    if (i < text.size() && text[i] == '.') {
        const auto start = ++i;
        while (i < text.size() && digit(text[i])) ++i;
        if (i == start) return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        const auto start = i;
        while (i < text.size() && digit(text[i])) ++i;
        if (i == start) return false;
    }
    return i == text.size();
}

std::optional<MissingReason> parse_number(std::string_view raw, const HeaderFeature& field,
                                        FeatureValue& value) {
    if (!decimal_grammar(raw)) return MissingReason::InvalidType;
    bool nonzero = false;
    for (char c : raw) {
        if (c == 'e' || c == 'E') break;
        if (c >= '1' && c <= '9') nonzero = true;
    }
    double number = 0;
    if (nonzero) {
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), number);
        if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size() ||
            !std::isfinite(number) || number == 0) return MissingReason::OutOfRange;
    }
    if (number < field.minimum || number > field.maximum) return MissingReason::OutOfRange;
    value = number == 0 ? 0.0 : number;
    return std::nullopt;
}

class Json {
public:
    void add(std::string_view text) {
        if (text.size() > kMaxFeatureBytes - data.size()) throw std::length_error("feature_limit");
        data.append(text);
    }
    void quoted(std::string_view text) {
        add("\"");
        for (unsigned char c : text) {
            if (c == '"') add("\\\"");
            else if (c == '\\') add("\\\\");
            else if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                add(std::string_view(escaped, sizeof escaped));
            } else {
                const char byte = static_cast<char>(c);
                add(std::string_view(&byte, 1));
            }
        }
        add("\"");
    }
    std::string data;
};

} // namespace

void validate_version(std::string_view version) {
    if (version.empty() || version.size() > 128 || !valid_utf8(version))
        throw std::invalid_argument("ML version must be nonempty UTF-8 and at most 128 bytes");
}

FeatureExtractor::FeatureExtractor(FeatureSchema schema) : schema_(std::move(schema)) {
    static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
    validate_version(schema_.version);
    if (schema_.headers.size() > kMaxHeaders) throw std::invalid_argument("too many feature headers");
    std::set<std::string> names;
    for (const auto& field : schema_.headers) {
        if (field.name.empty() || field.name.size() > kMaxKeyBytes ||
            !valid_utf8(field.name) || field.name.starts_with("__") ||
            !names.insert(field.name).second) throw std::invalid_argument("invalid feature header key");
        if (field.type == FeatureType::Numeric) {
            if (!std::isfinite(field.minimum) || !std::isfinite(field.maximum) ||
                field.minimum > field.maximum || !field.vocabulary.empty() ||
                field.encoding != CategoricalEncoding::Vocabulary)
                throw std::invalid_argument("invalid numeric feature specification");
        } else if (field.type == FeatureType::Categorical) {
            if (field.minimum != 0 || field.maximum != 0)
                throw std::invalid_argument("categorical feature cannot declare numeric range");
            if (field.encoding == CategoricalEncoding::Hash) {
                if (!field.vocabulary.empty()) throw std::invalid_argument("hash feature cannot declare vocabulary");
            } else if (field.encoding == CategoricalEncoding::Vocabulary) {
                if (field.vocabulary.size() > kMaxVocabulary)
                    throw std::invalid_argument("feature vocabulary too large");
                std::set<std::string> values;
                for (const auto& value : field.vocabulary)
                    if (value.size() > kMaxValueBytes || !valid_utf8(value) || !values.insert(value).second)
                        throw std::invalid_argument("invalid feature vocabulary");
            } else throw std::invalid_argument("unknown categorical encoding");
        } else throw std::invalid_argument("unknown feature type");
    }
}

std::string_view to_string(MissingReason reason) {
    switch (reason) {
        case MissingReason::Absent: return "absent";
        case MissingReason::InvalidType: return "invalid_type";
        case MissingReason::OutOfRange: return "out_of_range";
        case MissingReason::InvalidEncoding: return "invalid_encoding";
        case MissingReason::Oversized: return "oversized";
    }
    throw std::invalid_argument("unknown missing reason");
}

std::string snapshot_json(const FeatureSnapshot& snapshot) {
    if (snapshot.headers.size() > kMaxHeaders || snapshot.missing_reasons.size() > kMaxHeaders)
        throw std::invalid_argument("too many snapshot headers");
    for (const auto& [key, value] : snapshot.headers) {
        if (key.empty() || key.size() > kMaxKeyBytes || !valid_utf8(key) || key.starts_with("__"))
            throw std::invalid_argument("invalid snapshot key");
        if (const auto* category = std::get_if<std::string>(&value))
            if (category->size() > kMaxValueBytes || !valid_utf8(*category))
                throw std::invalid_argument("invalid snapshot category");
        if (std::holds_alternative<std::monostate>(value) != snapshot.missing_reasons.contains(key))
            throw std::invalid_argument("inconsistent missing reason");
    }
    for (const auto& [key, reason] : snapshot.missing_reasons) {
        if (!snapshot.headers.contains(key)) throw std::invalid_argument("unknown missing key");
        (void)to_string(reason);
    }
    Json out;
    out.add("{\"payload_size_bytes\":");
    out.quoted(std::to_string(snapshot.payload_size_bytes));
    out.add(",\"headers\":{");
    bool first = true;
    for (const auto& [key, value] : snapshot.headers) {
        if (!first) out.add(",");
        first = false;
        out.quoted(key);
        out.add(":");
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number)) throw std::invalid_argument("nonfinite feature number");
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof buffer,
                *number == 0 ? 0.0 : *number, std::chars_format::scientific, 16);
            if (result.ec != std::errc{}) throw std::invalid_argument("feature number conversion failed");
            out.add(std::string_view(buffer, static_cast<std::size_t>(result.ptr - buffer)));
        } else if (const auto* category = std::get_if<std::string>(&value)) out.quoted(*category);
        else out.add("null");
    }
    out.add("},\"missing_reasons\":{");
    first = true;
    for (const auto& [key, reason] : snapshot.missing_reasons) {
        if (!first) out.add(",");
        first = false;
        out.quoted(key);
        out.add(":");
        out.quoted(to_string(reason));
    }
    out.add("}}");
    return std::move(out.data);
}

ExtractedFeatures FeatureExtractor::extract(
    uint64_t payload_size_bytes, const std::unordered_map<std::string, std::string>& headers) const {
    FeatureSnapshot snapshot;
    snapshot.payload_size_bytes = payload_size_bytes;
    for (const auto& field : schema_.headers) {
        auto& value = snapshot.headers[field.name];
        const auto found = headers.find(field.name);
        std::optional<MissingReason> reason;
        if (found == headers.end()) reason = MissingReason::Absent;
        else {
            const auto& raw = found->second;
            if (raw.size() > kMaxValueBytes) reason = MissingReason::Oversized;
            else if (!valid_utf8(raw)) reason = MissingReason::InvalidEncoding;
            else if (field.type == FeatureType::Numeric) reason = parse_number(raw, field, value);
            else value = raw;
        }
        if (reason) snapshot.missing_reasons.emplace(field.name, *reason);
    }
    try {
        (void)snapshot_json(snapshot);
    } catch (const std::length_error&) {
        return {.features = std::nullopt, .validity = FeatureValidity::FeatureLimit};
    }
    return {.features = std::move(snapshot), .validity = FeatureValidity::Valid};
}

} // namespace harbinger::ml
