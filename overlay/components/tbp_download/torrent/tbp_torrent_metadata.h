// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_TORRENT_METADATA_H_
#define COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_TORRENT_METADATA_H_

#include <stdint.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "components/tbp_download/torrent/tbp_bencode.h"

namespace tbp_download {

// One file within a torrent. Present even for a single-file torrent (one
// entry, path equal to the torrent's name) so callers never need two code
// paths for single- vs multi-file mode.
struct TorrentFile {
  // "/"-joined relative path, forward slashes always, already validated to
  // contain no ".." component, no absolute/drive-letter prefix, and no empty
  // or null-containing component. Safe to join onto a download directory
  // without a caller re-checking it.
  std::string path;
  int64_t length = 0;
};

struct TorrentMetadata {
  // SHA-1 of the raw, original bytes of the "info" dictionary — this is what
  // trackers and peers identify the torrent by. Deliberately not the SHA-1 of
  // a re-encoded value; see BencodeFindRawValue for why that would be wrong.
  std::array<uint8_t, 20> info_hash{};

  std::string name;
  int64_t piece_length = 0;
  // Each entry is one piece's SHA-1, in piece order. piece_hashes.size() is
  // the piece count.
  std::vector<std::array<uint8_t, 20>> piece_hashes;
  // Concatenation order matches how bytes are laid out across pieces:
  // files[0] starting at offset 0, files[1] immediately after, etc.
  std::vector<TorrentFile> files;
  int64_t total_length = 0;
  bool is_private = false;

  // Flattened from "announce" + "announce-list" (BEP 12), tier order
  // preserved, duplicates removed.
  std::vector<std::string> trackers;
};

// Limits applied on top of BencodeLimits — bound what a hostile .torrent can
// cost us structurally, independent of the raw byte-level limits already
// enforced during bencode decoding.
struct TorrentMetadataLimits {
  BencodeLimits bencode_limits;
  // Real torrents rarely exceed a few thousand files; a torrent claiming
  // millions of zero-length files is a resource-exhaustion attempt, not
  // content.
  size_t max_files = 100000;
  size_t max_pieces = 10000000;
  size_t max_trackers = 1000;
};

// Parses a complete `.torrent` file's bytes. Returns nullopt on any
// malformed, inconsistent, or limit-exceeding input — never partially fills
// the result.
std::optional<TorrentMetadata> ParseTorrentFile(
    base::span<const uint8_t> raw_bytes,
    const TorrentMetadataLimits& limits = TorrentMetadataLimits());

}  // namespace tbp_download

#endif  // COMPONENTS_TBP_DOWNLOAD_TORRENT_TBP_TORRENT_METADATA_H_
