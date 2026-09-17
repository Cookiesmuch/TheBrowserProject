// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/torrent/tbp_bencode.h"

#include <limits>
#include <utility>

namespace tbp_download {

namespace {

// Computes the encoded byte length of the value starting at `pos` without
// building it. Iterative for the same reason the decoder is: depth is
// attacker-controlled. Returns false if the value is malformed or exceeds
// `limits`.
bool MeasureValue(base::span<const uint8_t> input,
                  size_t pos,
                  const BencodeLimits& limits,
                  size_t* end_out) {
  int depth = 0;
  // Number of container terminators still owed at each depth is implicit: we
  // simply keep going until depth returns to zero after having entered at
  // least one container, or until a scalar completes at depth zero.
  bool consumed_any = false;

  while (pos < input.size()) {
    const uint8_t c = input[pos];

    if (c == 'e') {
      if (depth == 0) {
        return false;  // Terminator with nothing open.
      }
      ++pos;
      --depth;
      consumed_any = true;
      if (depth == 0) {
        *end_out = pos;
        return true;
      }
      continue;
    }

    if (c == 'l' || c == 'd') {
      ++depth;
      if (depth > limits.max_depth) {
        return false;
      }
      ++pos;
      consumed_any = true;
      continue;
    }

    if (c == 'i') {
      const size_t start = pos;
      ++pos;
      while (pos < input.size() && input[pos] != 'e') {
        ++pos;
      }
      if (pos >= input.size()) {
        return false;  // Unterminated integer.
      }
      ++pos;  // Consume 'e'.
      if (pos == start + 2) {
        return false;  // "ie" has no digits.
      }
      consumed_any = true;
      if (depth == 0) {
        *end_out = pos;
        return true;
      }
      continue;
    }

    if (c >= '0' && c <= '9') {
      size_t length = 0;
      size_t digits = 0;
      while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
        const size_t digit = static_cast<size_t>(input[pos] - '0');
        // Same guard as ParseString: subtract from size_t max, not from the
        // cap, or this underflows for small caps and stops checking anything.
        if (length > (std::numeric_limits<size_t>::max() - digit) / 10) {
          return false;
        }
        length = length * 10 + digit;
        if (length > limits.max_string_length) {
          return false;
        }
        ++pos;
        ++digits;
      }
      if (digits == 0 || pos >= input.size() || input[pos] != ':') {
        return false;
      }
      ++pos;  // Consume ':'.
      if (length > input.size() - pos) {
        return false;  // Claims more bytes than exist.
      }
      pos += length;
      consumed_any = true;
      if (depth == 0) {
        *end_out = pos;
        return true;
      }
      continue;
    }

    return false;  // Not a valid value start.
  }

  // Ran out of input with containers still open, or with nothing consumed.
  (void)consumed_any;
  return false;
}

// Parses "i<digits>e" at `pos`, advancing it past the terminator.
bool ParseInteger(base::span<const uint8_t> input,
                  size_t* pos,
                  int64_t* out) {
  size_t i = *pos;
  if (i >= input.size() || input[i] != 'i') {
    return false;
  }
  ++i;

  bool negative = false;
  if (i < input.size() && input[i] == '-') {
    negative = true;
    ++i;
  }

  const size_t digits_start = i;
  int64_t value = 0;
  while (i < input.size() && input[i] >= '0' && input[i] <= '9') {
    const int64_t digit = input[i] - '0';
    // Range-check before multiplying so a long digit run cannot overflow.
    if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    ++i;
  }
  const size_t digit_count = i - digits_start;
  if (digit_count == 0) {
    return false;
  }
  // BEP 3: no leading zeros, and "-0" is not a number.
  if (input[digits_start] == '0' && digit_count > 1) {
    return false;
  }
  if (negative && value == 0) {
    return false;
  }
  if (i >= input.size() || input[i] != 'e') {
    return false;
  }
  ++i;

  *out = negative ? -value : value;
  *pos = i;
  return true;
}

// Parses "<length>:<bytes>" at `pos`, advancing it past the bytes.
bool ParseString(base::span<const uint8_t> input,
                 size_t* pos,
                 const BencodeLimits& limits,
                 std::string* out) {
  size_t i = *pos;
  const size_t digits_start = i;
  size_t length = 0;
  while (i < input.size() && input[i] >= '0' && input[i] <= '9') {
    const size_t digit = static_cast<size_t>(input[i] - '0');
    // Guard the multiply against both overflow and the configured cap. Note
    // the subtraction is done on the left-hand side: writing this as
    // `(max_string_length - digit) / 10` underflows when the cap is smaller
    // than a single digit, which silently disabled the check entirely.
    if (length > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;
    }
    length = length * 10 + digit;
    if (length > limits.max_string_length) {
      return false;  // Refuse before sizing anything from this number.
    }
    ++i;
  }
  const size_t digit_count = i - digits_start;
  if (digit_count == 0) {
    return false;
  }
  if (input[digits_start] == '0' && digit_count > 1) {
    return false;  // Leading zeros in a length are malformed.
  }
  if (i >= input.size() || input[i] != ':') {
    return false;
  }
  ++i;

  // Validate against what actually remains before sizing any allocation, so a
  // claimed 4 GB string in a 100-byte file costs nothing.
  if (length > input.size() - i) {
    return false;
  }

  // Copy through a subspan rather than pointer arithmetic: Chromium builds with
  // -Wunsafe-buffer-usage as an error, and for a parser fed by untrusted peers
  // the bounds-checked form is what we want regardless of the compiler.
  const base::span<const uint8_t> bytes = input.subspan(i, length);
  out->assign(bytes.begin(), bytes.end());
  *pos = i + length;
  return true;
}

// One open container while decoding.
struct Frame {
  bool is_dictionary = false;
  BencodeValue::List list;
  BencodeValue::Dictionary dictionary;
  std::string pending_key;
  bool has_pending_key = false;
};

}  // namespace

BencodeValue::BencodeValue() : type_(Type::kInteger), integer_(0) {}
BencodeValue::BencodeValue(int64_t value)
    : type_(Type::kInteger), integer_(value) {}
BencodeValue::BencodeValue(String value)
    : type_(Type::kString), string_(std::move(value)) {}
BencodeValue::BencodeValue(List value)
    : type_(Type::kList), list_(std::move(value)) {}
BencodeValue::BencodeValue(Dictionary value)
    : type_(Type::kDictionary), dictionary_(std::move(value)) {}

BencodeValue::BencodeValue(const BencodeValue&) = default;
BencodeValue& BencodeValue::operator=(const BencodeValue&) = default;
BencodeValue::BencodeValue(BencodeValue&&) noexcept = default;
BencodeValue& BencodeValue::operator=(BencodeValue&&) noexcept = default;
BencodeValue::~BencodeValue() = default;

std::optional<int64_t> BencodeValue::GetInteger() const {
  if (type_ != Type::kInteger) {
    return std::nullopt;
  }
  return integer_;
}

const BencodeValue::String* BencodeValue::GetString() const {
  return type_ == Type::kString ? &string_ : nullptr;
}

const BencodeValue::List* BencodeValue::GetList() const {
  return type_ == Type::kList ? &list_ : nullptr;
}

const BencodeValue::Dictionary* BencodeValue::GetDictionary() const {
  return type_ == Type::kDictionary ? &dictionary_ : nullptr;
}

const BencodeValue* BencodeValue::Find(std::string_view key) const {
  if (type_ != Type::kDictionary) {
    return nullptr;
  }
  const auto it = dictionary_.find(std::string(key));
  return it == dictionary_.end() ? nullptr : &it->second;
}

const BencodeValue::String* BencodeValue::FindString(
    std::string_view key) const {
  const BencodeValue* value = Find(key);
  return value ? value->GetString() : nullptr;
}

std::optional<int64_t> BencodeValue::FindInteger(std::string_view key) const {
  const BencodeValue* value = Find(key);
  return value ? value->GetInteger() : std::nullopt;
}

const BencodeValue::List* BencodeValue::FindList(std::string_view key) const {
  const BencodeValue* value = Find(key);
  return value ? value->GetList() : nullptr;
}

const BencodeValue::Dictionary* BencodeValue::FindDictionary(
    std::string_view key) const {
  const BencodeValue* value = Find(key);
  return value ? value->GetDictionary() : nullptr;
}

std::string BencodeValue::Encode() const {
  std::string out;
  // Iterative emit would complicate this for no security benefit: Encode only
  // ever runs on values we built ourselves, not on attacker input.
  switch (type_) {
    case Type::kInteger:
      out += 'i';
      out += std::to_string(integer_);
      out += 'e';
      break;
    case Type::kString:
      out += std::to_string(string_.size());
      out += ':';
      out += string_;
      break;
    case Type::kList:
      out += 'l';
      for (const BencodeValue& item : list_) {
        out += item.Encode();
      }
      out += 'e';
      break;
    case Type::kDictionary:
      out += 'd';
      // std::map orders by byte-wise string comparison, which is exactly the
      // ordering BEP 3 requires, so iteration order is already correct.
      for (const auto& [key, value] : dictionary_) {
        out += std::to_string(key.size());
        out += ':';
        out += key;
        out += value.Encode();
      }
      out += 'e';
      break;
  }
  return out;
}

std::optional<BencodeValue> BencodeDecodePrefix(base::span<const uint8_t> input,
                                                size_t* bytes_consumed,
                                                const BencodeLimits& limits) {
  if (input.empty()) {
    return std::nullopt;
  }

  std::vector<Frame> stack;
  std::optional<BencodeValue> result;
  size_t pos = 0;

  // Attaches a finished value to whatever is currently open, or makes it the
  // result when nothing is open.
  auto attach = [&](BencodeValue value) -> bool {
    if (stack.empty()) {
      result = std::move(value);
      return true;
    }
    Frame& frame = stack.back();
    if (!frame.is_dictionary) {
      if (frame.list.size() >= limits.max_container_entries) {
        return false;
      }
      frame.list.push_back(std::move(value));
      return true;
    }
    // Dictionary: values alternate key, value, key, value...
    if (!frame.has_pending_key) {
      const std::string* key = value.GetString();
      if (!key) {
        return false;  // BEP 3: dictionary keys must be strings.
      }
      if (frame.dictionary.size() >= limits.max_container_entries) {
        return false;
      }
      frame.pending_key = *key;
      frame.has_pending_key = true;
      return true;
    }
    // Duplicate keys are malformed and, worse, ambiguous — which one a peer
    // honors would differ between implementations.
    if (frame.dictionary.count(frame.pending_key) != 0) {
      return false;
    }
    frame.dictionary.emplace(std::move(frame.pending_key), std::move(value));
    frame.has_pending_key = false;
    return true;
  };

  while (pos < input.size()) {
    const uint8_t c = input[pos];

    if (c == 'l' || c == 'd') {
      if (static_cast<int>(stack.size()) >= limits.max_depth) {
        return std::nullopt;
      }
      Frame frame;
      frame.is_dictionary = (c == 'd');
      stack.push_back(std::move(frame));
      ++pos;
      continue;
    }

    if (c == 'e') {
      if (stack.empty()) {
        return std::nullopt;  // Terminator with nothing open.
      }
      Frame frame = std::move(stack.back());
      stack.pop_back();
      if (frame.is_dictionary && frame.has_pending_key) {
        return std::nullopt;  // Key with no value.
      }
      BencodeValue finished =
          frame.is_dictionary ? BencodeValue(std::move(frame.dictionary))
                              : BencodeValue(std::move(frame.list));
      if (!attach(std::move(finished))) {
        return std::nullopt;
      }
      ++pos;
      if (stack.empty()) {
        break;  // Closed the outermost container.
      }
      continue;
    }

    if (c == 'i') {
      int64_t value = 0;
      if (!ParseInteger(input, &pos, &value)) {
        return std::nullopt;
      }
      if (!attach(BencodeValue(value))) {
        return std::nullopt;
      }
      if (stack.empty()) {
        break;
      }
      continue;
    }

    if (c >= '0' && c <= '9') {
      std::string value;
      if (!ParseString(input, &pos, limits, &value)) {
        return std::nullopt;
      }
      if (!attach(BencodeValue(std::move(value)))) {
        return std::nullopt;
      }
      if (stack.empty()) {
        break;
      }
      continue;
    }

    return std::nullopt;  // Not a valid value start.
  }

  if (!stack.empty() || !result.has_value()) {
    return std::nullopt;  // Truncated input.
  }

  if (bytes_consumed) {
    *bytes_consumed = pos;
  }
  return result;
}

std::optional<BencodeValue> BencodeDecode(base::span<const uint8_t> input,
                                          const BencodeLimits& limits) {
  size_t consumed = 0;
  std::optional<BencodeValue> value =
      BencodeDecodePrefix(input, &consumed, limits);
  if (!value.has_value()) {
    return std::nullopt;
  }
  // A whole-input decode must consume everything; trailing bytes mean the
  // input is not what the caller thought it was.
  if (consumed != input.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<base::span<const uint8_t>> BencodeFindRawValue(
    base::span<const uint8_t> encoded_dictionary,
    std::string_view key,
    const BencodeLimits& limits) {
  if (encoded_dictionary.empty() || encoded_dictionary[0] != 'd') {
    return std::nullopt;
  }

  size_t pos = 1;  // Past the opening 'd'.
  while (pos < encoded_dictionary.size()) {
    if (encoded_dictionary[pos] == 'e') {
      return std::nullopt;  // End of dictionary, key not present.
    }

    // Keys are always byte strings.
    std::string current_key;
    if (!ParseString(encoded_dictionary, &pos, limits, &current_key)) {
      return std::nullopt;
    }

    size_t value_end = 0;
    if (!MeasureValue(encoded_dictionary, pos, limits, &value_end)) {
      return std::nullopt;
    }

    if (current_key == key) {
      return encoded_dictionary.subspan(pos, value_end - pos);
    }
    pos = value_end;
  }

  return std::nullopt;
}

}  // namespace tbp_download
