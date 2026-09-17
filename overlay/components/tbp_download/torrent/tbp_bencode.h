// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_BENCODE_H_
#define COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_BENCODE_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace tbp_download {

// Bencode (BEP 3) — the encoding used by .torrent files, tracker responses and
// several peer-wire extensions.
//
// SECURITY: every byte this sees comes from an untrusted source — a downloaded
// .torrent, a tracker that may be hostile, or an arbitrary peer on the internet.
// The parser is therefore written to fail rather than trust:
//   - iterative, not recursive, so a deeply nested value cannot smash the stack
//   - explicit depth and container-size ceilings
//   - every length is validated against the remaining input before it is used
//   - no allocation is sized directly from an attacker-supplied number
//   - integers are range-checked, with leading zeros and "-0" rejected per spec
//
// Decoding never throws and never partially mutates its output; it returns
// std::nullopt on any malformed input.
class BencodeValue {
 public:
  enum class Type {
    kInteger,
    kString,
    kList,
    kDictionary,
  };

  // Bencode strings are byte strings, not text — piece hashes are raw SHA-1 and
  // are not valid UTF-8 — so they are held as std::string used as a byte buffer.
  using String = std::string;
  using List = std::vector<BencodeValue>;
  // Ordered, because BEP 3 requires dictionary keys to be sorted and info-hash
  // computation depends on reproducing the exact original byte sequence.
  using Dictionary = std::map<std::string, BencodeValue>;

  BencodeValue();
  explicit BencodeValue(int64_t value);
  explicit BencodeValue(String value);
  explicit BencodeValue(List value);
  explicit BencodeValue(Dictionary value);

  BencodeValue(const BencodeValue&);
  BencodeValue& operator=(const BencodeValue&);
  BencodeValue(BencodeValue&&) noexcept;
  BencodeValue& operator=(BencodeValue&&) noexcept;
  ~BencodeValue();

  Type type() const { return type_; }
  bool is_integer() const { return type_ == Type::kInteger; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_list() const { return type_ == Type::kList; }
  bool is_dictionary() const { return type_ == Type::kDictionary; }

  // Typed accessors. Each returns nullptr/nullopt when the value is a different
  // type, so callers cannot accidentally read a string as an integer just
  // because a hostile .torrent said so.
  std::optional<int64_t> GetInteger() const;
  const String* GetString() const;
  const List* GetList() const;
  const Dictionary* GetDictionary() const;

  // Dictionary lookup shorthand. Returns nullptr when this is not a dictionary
  // or the key is absent.
  const BencodeValue* Find(std::string_view key) const;
  // Convenience lookups that also check the type of the value found.
  const String* FindString(std::string_view key) const;
  std::optional<int64_t> FindInteger(std::string_view key) const;
  const List* FindList(std::string_view key) const;
  const Dictionary* FindDictionary(std::string_view key) const;

  // Serializes back to bencode. Dictionary keys are emitted in sorted order as
  // the spec requires, so re-encoding a parsed value is byte-stable.
  std::string Encode() const;

 private:
  Type type_ = Type::kInteger;
  int64_t integer_ = 0;
  String string_;
  List list_;
  Dictionary dictionary_;
};

// Limits applied while decoding. The defaults are generous for real torrents and
// still bound what a hostile input can cost us.
struct BencodeLimits {
  // Nesting depth. Real torrents nest about three levels; anything approaching
  // this is an attack, not a torrent.
  int max_depth = 32;
  // Elements in a single list or dictionary.
  size_t max_container_entries = 1000000;
  // Length of a single byte string. A torrent's piece-hash string is 20 bytes
  // per piece, so this comfortably covers very large torrents.
  size_t max_string_length = 64u * 1024 * 1024;
};

// Decodes `input`. Returns nullopt if the input is malformed, exceeds a limit,
// or has trailing bytes after the first complete value.
std::optional<BencodeValue> BencodeDecode(
    base::span<const uint8_t> input,
    const BencodeLimits& limits = BencodeLimits());

// Same, but reports how many bytes the value consumed, allowing trailing data.
// Used by the peer wire protocol, where a bencoded payload is followed by
// binary data in the same buffer.
std::optional<BencodeValue> BencodeDecodePrefix(
    base::span<const uint8_t> input,
    size_t* bytes_consumed,
    const BencodeLimits& limits = BencodeLimits());

// Returns the byte range of the value stored at `key` within an encoded
// dictionary, without copying it.
//
// This exists for one specific reason: a torrent's info-hash is the SHA-1 of the
// *original bytes* of its "info" dictionary. Re-encoding a parsed value is not
// safe for that purpose — a torrent in the wild may contain keys out of order or
// other encoding quirks, and re-encoding would normalize them and produce an
// info-hash that no peer or tracker agrees with. The raw span must be hashed.
std::optional<base::span<const uint8_t>> BencodeFindRawValue(
    base::span<const uint8_t> encoded_dictionary,
    std::string_view key,
    const BencodeLimits& limits = BencodeLimits());

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_BENCODE_H_
