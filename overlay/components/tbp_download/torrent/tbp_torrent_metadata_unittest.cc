// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/torrent/tbp_torrent_metadata.h"

#include "base/hash/sha1.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace tbp_download {
namespace {

std::vector<uint8_t> ToBytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

BencodeValue::String MakePieces(size_t count) {
  // Deterministic, distinguishable fake piece hashes: piece i is 20 copies
  // of byte i (mod 256).
  std::string pieces;
  for (size_t i = 0; i < count; ++i) {
    pieces.append(20, static_cast<char>(i % 256));
  }
  return pieces;
}

// Builds a single-file torrent's top-level dictionary and returns its
// encoded bytes, along with the exact bytes of the info sub-dictionary (for
// independently verifying the info-hash).
struct BuiltTorrent {
  std::vector<uint8_t> full_bytes;
  std::vector<uint8_t> info_bytes;
};

BuiltTorrent BuildSingleFileTorrent(const std::string& name,
                                     int64_t piece_length,
                                     int64_t length,
                                     size_t piece_count,
                                     const std::string& announce = "") {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(name));
  info.emplace("piece length", BencodeValue(piece_length));
  info.emplace("pieces", BencodeValue(MakePieces(piece_count)));
  info.emplace("length", BencodeValue(length));

  BencodeValue info_value(info);
  BuiltTorrent result;
  result.info_bytes = ToBytes(info_value.Encode());

  BencodeValue::Dictionary root;
  root.emplace("info", std::move(info_value));
  if (!announce.empty()) {
    root.emplace("announce", BencodeValue(announce));
  }
  result.full_bytes = ToBytes(BencodeValue(root).Encode());
  return result;
}

TEST(TbpTorrentMetadataTest, ParsesSingleFileTorrent) {
  // One full piece plus a partial second piece: 2 pieces for 150 bytes at
  // piece_length 100.
  BuiltTorrent torrent = BuildSingleFileTorrent("movie.mkv", 100, 150, 2);

  std::optional<TorrentMetadata> metadata = ParseTorrentFile(torrent.full_bytes);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->name, "movie.mkv");
  EXPECT_EQ(metadata->piece_length, 100);
  EXPECT_EQ(metadata->piece_hashes.size(), 2u);
  EXPECT_EQ(metadata->total_length, 150);
  ASSERT_EQ(metadata->files.size(), 1u);
  EXPECT_EQ(metadata->files[0].path, "movie.mkv");
  EXPECT_EQ(metadata->files[0].length, 150);
  EXPECT_FALSE(metadata->is_private);
}

TEST(TbpTorrentMetadataTest, ComputesInfoHashFromRawInfoBytesNotReencoding) {
  BuiltTorrent torrent = BuildSingleFileTorrent("a.bin", 100, 100, 1);

  std::optional<TorrentMetadata> metadata = ParseTorrentFile(torrent.full_bytes);
  ASSERT_TRUE(metadata.has_value());

  // Independently compute SHA-1 over exactly the info dictionary's encoded
  // bytes and confirm it matches what the parser reports. This is the
  // property that actually matters — the browser's info_hash must equal what
  // any BEP-3-compliant tracker or peer would compute.
  base::SHA1Digest expected = base::SHA1Hash(base::span(torrent.info_bytes));
  EXPECT_EQ(metadata->info_hash, expected);
}

TEST(TbpTorrentMetadataTest, ParsesMultiFileTorrent) {
  BencodeValue::Dictionary file_a;
  file_a.emplace("length", BencodeValue(int64_t{50}));
  BencodeValue::List path_a = {BencodeValue(std::string("sub")),
                                BencodeValue(std::string("a.txt"))};
  file_a.emplace("path", BencodeValue(path_a));

  BencodeValue::Dictionary file_b;
  file_b.emplace("length", BencodeValue(int64_t{50}));
  BencodeValue::List path_b = {BencodeValue(std::string("b.txt"))};
  file_b.emplace("path", BencodeValue(path_b));

  BencodeValue::List files = {BencodeValue(file_a), BencodeValue(file_b)};

  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("MyTorrent")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(MakePieces(1)));
  info.emplace("files", BencodeValue(files));

  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));

  auto bytes = ToBytes(BencodeValue(root).Encode());
  std::optional<TorrentMetadata> metadata = ParseTorrentFile(bytes);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->total_length, 100);
  ASSERT_EQ(metadata->files.size(), 2u);
  EXPECT_EQ(metadata->files[0].path, "MyTorrent/sub/a.txt");
  EXPECT_EQ(metadata->files[1].path, "MyTorrent/b.txt");
}

TEST(TbpTorrentMetadataTest, RejectsPathTraversalInFileEntry) {
  for (const std::string& malicious : {"..", "../../etc/passwd", "C:", ""}) {
    BencodeValue::Dictionary file;
    file.emplace("length", BencodeValue(int64_t{10}));
    BencodeValue::List path = {BencodeValue(malicious)};
    file.emplace("path", BencodeValue(path));

    BencodeValue::List files = {BencodeValue(file)};

    BencodeValue::Dictionary info;
    info.emplace("name", BencodeValue(std::string("t")));
    info.emplace("piece length", BencodeValue(int64_t{100}));
    info.emplace("pieces", BencodeValue(MakePieces(1)));
    info.emplace("files", BencodeValue(files));

    BencodeValue::Dictionary root;
    root.emplace("info", BencodeValue(info));

    auto bytes = ToBytes(BencodeValue(root).Encode());
    EXPECT_FALSE(ParseTorrentFile(bytes).has_value())
        << "malicious path component not rejected: " << malicious;
  }
}

TEST(TbpTorrentMetadataTest, RejectsPathSeparatorSmuggledInsideComponent) {
  // A single path-list entry that itself contains a separator would let a
  // "safe-looking" single component escape its intended directory once
  // joined and used as a filesystem path.
  std::vector<std::string> malicious_components = {"a/b", "a\\b",
                                                    std::string("a\0b", 3)};
  for (const std::string& malicious : malicious_components) {
    BencodeValue::Dictionary file;
    file.emplace("length", BencodeValue(int64_t{10}));
    BencodeValue::List path = {BencodeValue(malicious)};
    file.emplace("path", BencodeValue(path));
    BencodeValue::List files = {BencodeValue(file)};

    BencodeValue::Dictionary info;
    info.emplace("name", BencodeValue(std::string("t")));
    info.emplace("piece length", BencodeValue(int64_t{100}));
    info.emplace("pieces", BencodeValue(MakePieces(1)));
    info.emplace("files", BencodeValue(files));
    BencodeValue::Dictionary root;
    root.emplace("info", BencodeValue(info));

    auto bytes = ToBytes(BencodeValue(root).Encode());
    EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
  }
}

TEST(TbpTorrentMetadataTest, RejectsUnsafeTorrentName) {
  for (const std::string& malicious : {"..", "a/b", ""}) {
    BuiltTorrent torrent = BuildSingleFileTorrent(malicious, 100, 100, 1);
    EXPECT_FALSE(ParseTorrentFile(torrent.full_bytes).has_value());
  }
}

TEST(TbpTorrentMetadataTest, RejectsMissingRequiredFields) {
  // No "info" key at all.
  {
    BencodeValue::Dictionary root;
    root.emplace("announce", BencodeValue(std::string("http://example.com")));
    auto bytes = ToBytes(BencodeValue(root).Encode());
    EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
  }
  // "info" present but missing "pieces".
  {
    BencodeValue::Dictionary info;
    info.emplace("name", BencodeValue(std::string("t")));
    info.emplace("piece length", BencodeValue(int64_t{100}));
    info.emplace("length", BencodeValue(int64_t{100}));
    BencodeValue::Dictionary root;
    root.emplace("info", BencodeValue(info));
    auto bytes = ToBytes(BencodeValue(root).Encode());
    EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
  }
  // Neither "files" nor "length" in info.
  {
    BencodeValue::Dictionary info;
    info.emplace("name", BencodeValue(std::string("t")));
    info.emplace("piece length", BencodeValue(int64_t{100}));
    info.emplace("pieces", BencodeValue(MakePieces(1)));
    BencodeValue::Dictionary root;
    root.emplace("info", BencodeValue(info));
    auto bytes = ToBytes(BencodeValue(root).Encode());
    EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
  }
}

TEST(TbpTorrentMetadataTest, RejectsNonPositivePieceLength) {
  for (int64_t bad_length : {0, -1, -100}) {
    BuiltTorrent torrent = BuildSingleFileTorrent("t", bad_length, 100, 1);
    EXPECT_FALSE(ParseTorrentFile(torrent.full_bytes).has_value());
  }
}

TEST(TbpTorrentMetadataTest, RejectsPiecesLengthNotMultipleOf20) {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(std::string(25, 'x')));  // not a multiple of 20
  info.emplace("length", BencodeValue(int64_t{100}));
  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  auto bytes = ToBytes(BencodeValue(root).Encode());
  EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
}

TEST(TbpTorrentMetadataTest, RejectsPieceCountMismatch) {
  // 250 bytes at piece_length 100 needs 3 pieces; declare only 2.
  BuiltTorrent torrent = BuildSingleFileTorrent("t", 100, 250, 2);
  EXPECT_FALSE(ParseTorrentFile(torrent.full_bytes).has_value());
}

TEST(TbpTorrentMetadataTest, ZeroLengthFileHasZeroPieces) {
  BuiltTorrent torrent = BuildSingleFileTorrent("empty.txt", 100, 0, 0);
  std::optional<TorrentMetadata> metadata = ParseTorrentFile(torrent.full_bytes);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->total_length, 0);
  EXPECT_TRUE(metadata->piece_hashes.empty());
}

TEST(TbpTorrentMetadataTest, HandlesPrivateFlag) {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(MakePieces(1)));
  info.emplace("length", BencodeValue(int64_t{100}));
  info.emplace("private", BencodeValue(int64_t{1}));
  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  auto bytes = ToBytes(BencodeValue(root).Encode());

  std::optional<TorrentMetadata> metadata = ParseTorrentFile(bytes);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_TRUE(metadata->is_private);
}

TEST(TbpTorrentMetadataTest, FlattensAnnounceListPreservingTierOrderAndDedupes) {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(MakePieces(1)));
  info.emplace("length", BencodeValue(int64_t{100}));

  BencodeValue::List tier1 = {BencodeValue(std::string("http://a")),
                               BencodeValue(std::string("http://b"))};
  BencodeValue::List tier2 = {BencodeValue(std::string("http://a")),  // duplicate
                               BencodeValue(std::string("http://c"))};
  BencodeValue::List announce_list = {BencodeValue(tier1), BencodeValue(tier2)};

  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  root.emplace("announce", BencodeValue(std::string("http://a")));  // also duplicate
  root.emplace("announce-list", BencodeValue(announce_list));
  auto bytes = ToBytes(BencodeValue(root).Encode());

  std::optional<TorrentMetadata> metadata = ParseTorrentFile(bytes);
  ASSERT_TRUE(metadata.has_value());
  ASSERT_EQ(metadata->trackers.size(), 3u);
  EXPECT_EQ(metadata->trackers[0], "http://a");
  EXPECT_EQ(metadata->trackers[1], "http://b");
  EXPECT_EQ(metadata->trackers[2], "http://c");
}

TEST(TbpTorrentMetadataTest, TruncatesTrackersAtLimitWithoutFailingParse) {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(MakePieces(1)));
  info.emplace("length", BencodeValue(int64_t{100}));

  BencodeValue::List tier;
  for (int i = 0; i < 10; ++i) {
    tier.push_back(BencodeValue("http://tracker" + std::to_string(i)));
  }
  BencodeValue::List announce_list = {BencodeValue(tier)};

  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  root.emplace("announce-list", BencodeValue(announce_list));
  auto bytes = ToBytes(BencodeValue(root).Encode());

  TorrentMetadataLimits limits;
  limits.max_trackers = 3;
  std::optional<TorrentMetadata> metadata = ParseTorrentFile(bytes, limits);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->trackers.size(), 3u);
}

TEST(TbpTorrentMetadataTest, RejectsTooManyFiles) {
  BencodeValue::List files;
  for (int i = 0; i < 5; ++i) {
    BencodeValue::Dictionary file;
    file.emplace("length", BencodeValue(int64_t{0}));
    BencodeValue::List path = {BencodeValue("f" + std::to_string(i))};
    file.emplace("path", BencodeValue(path));
    files.push_back(BencodeValue(file));
  }

  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(BencodeValue::String()));
  info.emplace("files", BencodeValue(files));
  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  auto bytes = ToBytes(BencodeValue(root).Encode());

  TorrentMetadataLimits limits;
  limits.max_files = 3;
  EXPECT_FALSE(ParseTorrentFile(bytes, limits).has_value());
}

TEST(TbpTorrentMetadataTest, RejectsEmptyFilesList) {
  BencodeValue::Dictionary info;
  info.emplace("name", BencodeValue(std::string("t")));
  info.emplace("piece length", BencodeValue(int64_t{100}));
  info.emplace("pieces", BencodeValue(BencodeValue::String()));
  info.emplace("files", BencodeValue(BencodeValue::List()));
  BencodeValue::Dictionary root;
  root.emplace("info", BencodeValue(info));
  auto bytes = ToBytes(BencodeValue(root).Encode());
  EXPECT_FALSE(ParseTorrentFile(bytes).has_value());
}

TEST(TbpTorrentMetadataTest, RejectsGarbageInput) {
  std::vector<uint8_t> garbage = {0xff, 0x00, 0x01, 'd', 'e'};
  EXPECT_FALSE(ParseTorrentFile(garbage).has_value());

  std::vector<uint8_t> empty;
  EXPECT_FALSE(ParseTorrentFile(empty).has_value());

  // A valid bencode value that isn't a dictionary at all.
  auto not_a_dict = ToBytes(BencodeValue(int64_t{42}).Encode());
  EXPECT_FALSE(ParseTorrentFile(not_a_dict).has_value());
}

}  // namespace
}  // namespace tbp_download
