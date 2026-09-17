// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/torrent/tbp_bencode.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

base::span<const uint8_t> AsBytes(std::string_view text) {
  return base::as_byte_span(text);
}

std::optional<BencodeValue> Decode(std::string_view text) {
  return BencodeDecode(AsBytes(text));
}

// --- integers -------------------------------------------------------------

TEST(TbpBencodeTest, DecodesIntegers) {
  EXPECT_EQ(0, Decode("i0e")->GetInteger());
  EXPECT_EQ(42, Decode("i42e")->GetInteger());
  EXPECT_EQ(-42, Decode("i-42e")->GetInteger());
  EXPECT_EQ(9223372036854775807LL, Decode("i9223372036854775807e")->GetInteger());
}

TEST(TbpBencodeTest, RejectsMalformedIntegers) {
  // BEP 3 forbids leading zeros and negative zero. Accepting them would mean
  // two encodings of the same value, which breaks byte-stable re-encoding.
  EXPECT_FALSE(Decode("i03e").has_value()) << "leading zero";
  EXPECT_FALSE(Decode("i-0e").has_value()) << "negative zero";
  EXPECT_FALSE(Decode("i007e").has_value()) << "leading zeros";
  EXPECT_FALSE(Decode("ie").has_value()) << "no digits";
  EXPECT_FALSE(Decode("i-e").has_value()) << "sign with no digits";
  EXPECT_FALSE(Decode("i42").has_value()) << "unterminated";
  EXPECT_FALSE(Decode("i4 2e").has_value()) << "embedded space";
  EXPECT_FALSE(Decode("iabce").has_value()) << "non-numeric";
}

TEST(TbpBencodeTest, RejectsIntegerOverflow) {
  // One past int64 max, and a digit run long enough to wrap naive arithmetic.
  EXPECT_FALSE(Decode("i9223372036854775808e").has_value());
  EXPECT_FALSE(Decode("i99999999999999999999999999999999e").has_value());
}

// --- strings --------------------------------------------------------------

TEST(TbpBencodeTest, DecodesStrings) {
  EXPECT_EQ("", *Decode("0:")->GetString());
  EXPECT_EQ("spam", *Decode("4:spam")->GetString());
}

TEST(TbpBencodeTest, DecodesBinaryStringsWithNulsAndHighBytes) {
  // Piece hashes are raw SHA-1: not UTF-8, frequently containing NUL. Treating
  // bencode strings as C strings would silently truncate them.
  const std::string payload("\x00\xFF\x01\x80\x00", 5);
  const std::string encoded = "5:" + payload;
  const auto value = Decode(encoded);
  ASSERT_TRUE(value.has_value());
  ASSERT_TRUE(value->GetString());
  EXPECT_EQ(5u, value->GetString()->size());
  EXPECT_EQ(payload, *value->GetString());
}

TEST(TbpBencodeTest, RejectsMalformedStrings) {
  EXPECT_FALSE(Decode("4:spa").has_value()) << "claims more bytes than exist";
  EXPECT_FALSE(Decode("4spam").has_value()) << "missing colon";
  EXPECT_FALSE(Decode(":spam").has_value()) << "missing length";
  EXPECT_FALSE(Decode("04:spam").has_value()) << "leading zero in length";
  EXPECT_FALSE(Decode("-1:x").has_value()) << "negative length";
}

TEST(TbpBencodeTest, RejectsHugeClaimedStringLengthCheaply) {
  // The classic decompression-bomb shape: a tiny input claiming an enormous
  // string. This must fail on arithmetic, never by attempting the allocation.
  EXPECT_FALSE(Decode("99999999999999999999:x").has_value());
  EXPECT_FALSE(Decode("4294967296:x").has_value());
}

// --- containers -----------------------------------------------------------

TEST(TbpBencodeTest, DecodesLists) {
  const auto value = Decode("l4:spami42ee");
  ASSERT_TRUE(value.has_value());
  const auto* list = value->GetList();
  ASSERT_TRUE(list);
  ASSERT_EQ(2u, list->size());
  EXPECT_EQ("spam", *(*list)[0].GetString());
  EXPECT_EQ(42, (*list)[1].GetInteger());
}

TEST(TbpBencodeTest, DecodesEmptyContainers) {
  EXPECT_TRUE(Decode("le")->GetList()->empty());
  EXPECT_TRUE(Decode("de")->GetDictionary()->empty());
}

TEST(TbpBencodeTest, DecodesDictionaries) {
  const auto value = Decode("d3:bar4:spam3:fooi42ee");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ("spam", *value->FindString("bar"));
  EXPECT_EQ(42, value->FindInteger("foo"));
  EXPECT_FALSE(value->Find("absent"));
}

TEST(TbpBencodeTest, TypedLookupsRejectWrongType) {
  // A hostile torrent can put any type under any key; callers must not be able
  // to read a string as an integer just because they asked for one.
  const auto value = Decode("d3:fooi42e3:bar4:spame");
  ASSERT_TRUE(value.has_value());
  EXPECT_FALSE(value->FindString("foo")) << "integer read as string";
  EXPECT_FALSE(value->FindInteger("bar").has_value()) << "string read as int";
  EXPECT_FALSE(value->FindList("foo"));
  EXPECT_FALSE(value->FindDictionary("foo"));
}

TEST(TbpBencodeTest, RejectsMalformedContainers) {
  EXPECT_FALSE(Decode("l4:spam").has_value()) << "unterminated list";
  EXPECT_FALSE(Decode("d3:foo").has_value()) << "key with no value";
  EXPECT_FALSE(Decode("di42e4:spame").has_value()) << "non-string dict key";
  EXPECT_FALSE(Decode("e").has_value()) << "terminator alone";
  EXPECT_FALSE(Decode("").has_value()) << "empty input";
}

TEST(TbpBencodeTest, RejectsDuplicateDictionaryKeys) {
  // Ambiguous: which value a peer honors would vary by implementation, so this
  // is a real interop and security hazard rather than a nicety.
  EXPECT_FALSE(Decode("d3:fooi1e3:fooi2ee").has_value());
}

TEST(TbpBencodeTest, RejectsTrailingData) {
  EXPECT_FALSE(Decode("i42ejunk").has_value());
  EXPECT_FALSE(Decode("4:spam4:spam").has_value());
}

// --- hostile input --------------------------------------------------------

TEST(TbpBencodeTest, RejectsDeeplyNestedInputWithoutStackOverflow) {
  // A recursive-descent parser dies here. Reaching the assertion at all is the
  // point of the test: a hostile .torrent must not be able to smash the stack.
  std::string deep(200000, 'l');
  EXPECT_FALSE(Decode(deep).has_value());

  std::string deep_closed = std::string(100000, 'l') + std::string(100000, 'e');
  EXPECT_FALSE(Decode(deep_closed).has_value()) << "exceeds depth limit";
}

TEST(TbpBencodeTest, RespectsConfiguredDepthLimit) {
  BencodeLimits limits;
  limits.max_depth = 3;
  // Both inputs are well-formed and balanced, so the only thing that can
  // distinguish them is the depth limit itself.
  EXPECT_TRUE(BencodeDecode(AsBytes("llleee"), limits).has_value())
      << "three levels is within the limit";
  EXPECT_FALSE(BencodeDecode(AsBytes("lllleeee"), limits).has_value())
      << "four levels should exceed the limit";
}

TEST(TbpBencodeTest, RespectsConfiguredStringLimit) {
  BencodeLimits limits;
  limits.max_string_length = 4;
  EXPECT_TRUE(BencodeDecode(AsBytes("4:spam"), limits).has_value());
  EXPECT_FALSE(BencodeDecode(AsBytes("5:spams"), limits).has_value());
}

TEST(TbpBencodeTest, TruncatedInputAtEveryOffsetIsRejectedCleanly) {
  // Feeding every prefix of a valid torrent-shaped structure catches parsers
  // that read one byte past the end on malformed input.
  const std::string valid =
      "d8:announce23:http://tracker/announce4:infod6:lengthi1024e4:name4:"
      "spam12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";
  for (size_t i = 1; i < valid.size(); ++i) {
    const std::string truncated = valid.substr(0, i);
    // Must return cleanly, not crash — the value of this test is that it runs.
    const auto result = Decode(truncated);
    if (i != valid.size()) {
      EXPECT_FALSE(result.has_value())
          << "prefix of length " << i << " decoded as complete";
    }
  }
  EXPECT_TRUE(Decode(valid).has_value()) << "the full input should be valid";
}

// --- round trip -----------------------------------------------------------

TEST(TbpBencodeTest, EncodeRoundTrips) {
  const std::string original =
      "d8:announce23:http://tracker/announce4:infod6:lengthi1024e4:name4:"
      "spam12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";
  const auto value = Decode(original);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(original, value->Encode());
}

TEST(TbpBencodeTest, EncodeSortsDictionaryKeys) {
  // BEP 3 requires sorted keys. Emitting them in insertion order would produce
  // a different info-hash than every other client computes.
  BencodeValue::Dictionary dict;
  dict.emplace("zebra", BencodeValue(int64_t{1}));
  dict.emplace("alpha", BencodeValue(int64_t{2}));
  dict.emplace("middle", BencodeValue(int64_t{3}));
  const BencodeValue value{std::move(dict)};
  EXPECT_EQ("d5:alphai2e6:middlei3e5:zebrai1ee", value.Encode());
}

// --- raw value extraction (info-hash support) -----------------------------

TEST(TbpBencodeTest, FindsRawValueSpanForInfoHashing) {
  // The info-hash is SHA-1 over the *original bytes* of the info dictionary.
  // Re-encoding a parsed value would normalize any quirks in the original and
  // produce a hash no other client agrees with, so the raw span is what counts.
  const std::string torrent =
      "d8:announce9:http://t/4:infod6:lengthi1024e4:name4:spamee";
  const auto raw = BencodeFindRawValue(AsBytes(torrent), "info");
  ASSERT_TRUE(raw.has_value());

  const std::string extracted(reinterpret_cast<const char*>(raw->data()),
                              raw->size());
  EXPECT_EQ("d6:lengthi1024e4:name4:spame", extracted);
}

TEST(TbpBencodeTest, RawValueSpanPreservesNonNormalizedOrdering) {
  // Deliberately out-of-spec key order. A correct implementation hands back
  // these exact bytes rather than a tidied-up re-encoding.
  const std::string torrent = "d4:infod4:name4:spam6:lengthi7eee";
  const auto raw = BencodeFindRawValue(AsBytes(torrent), "info");
  ASSERT_TRUE(raw.has_value());
  const std::string extracted(reinterpret_cast<const char*>(raw->data()),
                              raw->size());
  EXPECT_EQ("d4:name4:spam6:lengthi7ee", extracted)
      << "raw extraction must not normalize key order";
}

TEST(TbpBencodeTest, RawValueLookupHandlesMissingKeyAndBadInput) {
  const std::string torrent = "d8:announce9:http://t/e";
  EXPECT_FALSE(BencodeFindRawValue(AsBytes(torrent), "info").has_value());
  EXPECT_FALSE(BencodeFindRawValue(AsBytes("i42e"), "info").has_value())
      << "not a dictionary";
  EXPECT_FALSE(BencodeFindRawValue(AsBytes(""), "info").has_value());
  EXPECT_FALSE(BencodeFindRawValue(AsBytes("d4:info"), "info").has_value())
      << "truncated";
}

TEST(TbpBencodeTest, RawValueLookupWorksForNestedContainers) {
  const std::string torrent = "d1:ald1:bi1eed1:ci2eee1:di3ee";
  const auto raw = BencodeFindRawValue(AsBytes(torrent), "a");
  ASSERT_TRUE(raw.has_value());
  const std::string extracted(reinterpret_cast<const char*>(raw->data()),
                              raw->size());
  EXPECT_EQ("ld1:bi1eed1:ci2eee", extracted);
}

// --- prefix decoding ------------------------------------------------------

TEST(TbpBencodeTest, DecodePrefixReportsConsumedBytes) {
  // The peer wire protocol puts a bencoded header immediately before binary
  // payload in the same buffer, so the decoder has to say where it stopped.
  const std::string input = "d3:fooi42eeBINARYDATA";
  size_t consumed = 0;
  const auto value = BencodeDecodePrefix(AsBytes(input), &consumed);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(42, value->FindInteger("foo"));
  EXPECT_EQ(std::string("d3:fooi42ee").size(), consumed);
  EXPECT_EQ("BINARYDATA", input.substr(consumed));
}

}  // namespace
}  // namespace tbp_download
