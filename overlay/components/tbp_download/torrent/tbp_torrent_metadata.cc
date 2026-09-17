// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/torrent/tbp_torrent_metadata.h"

#include "base/containers/span.h"
#include "base/hash/sha1.h"

namespace tbp_download {

namespace {

// True if `component` is safe to use as one segment of a file path derived
// from attacker-controlled torrent data: non-empty, no path traversal, no
// separator characters (it is meant to be exactly one segment), no NUL, and
// not a bare Windows drive designator.
bool IsSafePathComponent(const std::string& component) {
  if (component.empty()) {
    return false;
  }
  if (component == "." || component == "..") {
    return false;
  }
  for (char c : component) {
    if (c == '/' || c == '\\' || c == '\0') {
      return false;
    }
  }
  // Reject "C:"-style or trailing-colon segments so a joined path can never
  // be reinterpreted as an absolute Windows path or an alternate data stream.
  if (component.find(':') != std::string::npos) {
    return false;
  }
  return true;
}

// Joins already-validated path components with '/'. Callers must validate
// every component with IsSafePathComponent first.
std::string JoinPath(const std::vector<std::string>& components) {
  std::string result;
  for (const auto& component : components) {
    if (!result.empty()) {
      result += '/';
    }
    result += component;
  }
  return result;
}

// Parses one entry of the "files" list in a multi-file torrent's info
// dictionary. `torrent_name` is prepended so the returned path always sits
// inside a directory named after the torrent, matching what every real
// client materializes on disk.
std::optional<TorrentFile> ParseFileEntry(const BencodeValue& entry,
                                           const std::string& torrent_name) {
  const auto* dict = entry.GetDictionary();
  if (!dict) {
    return std::nullopt;
  }

  std::optional<int64_t> length = entry.FindInteger("length");
  if (!length.has_value() || *length < 0) {
    return std::nullopt;
  }

  const auto* path_list = entry.FindList("path");
  if (!path_list || path_list->empty()) {
    return std::nullopt;
  }

  std::vector<std::string> components = {torrent_name};
  for (const auto& part : *path_list) {
    const auto* part_string = part.GetString();
    if (!part_string || !IsSafePathComponent(*part_string)) {
      return std::nullopt;
    }
    components.push_back(*part_string);
  }

  TorrentFile file;
  file.path = JoinPath(components);
  file.length = *length;
  return file;
}

void AppendTrackerIfNew(const std::string& tracker,
                         std::vector<std::string>& trackers,
                         size_t max_trackers) {
  if (tracker.empty() || trackers.size() >= max_trackers) {
    return;
  }
  for (const auto& existing : trackers) {
    if (existing == tracker) {
      return;
    }
  }
  trackers.push_back(tracker);
}

}  // namespace

std::optional<TorrentMetadata> ParseTorrentFile(
    base::span<const uint8_t> raw_bytes,
    const TorrentMetadataLimits& limits) {
  std::optional<BencodeValue> root =
      BencodeDecode(raw_bytes, limits.bencode_limits);
  if (!root.has_value() || !root->is_dictionary()) {
    return std::nullopt;
  }

  const BencodeValue* info = root->Find("info");
  if (!info || !info->is_dictionary()) {
    return std::nullopt;
  }

  // The info-hash must be the SHA-1 of the exact original bytes of the
  // "info" value, not a re-encoding of the parsed structure — see
  // BencodeFindRawValue's own documentation for why. This is looked up
  // against the ORIGINAL input, independently of the decode above.
  std::optional<base::span<const uint8_t>> info_span =
      BencodeFindRawValue(raw_bytes, "info", limits.bencode_limits);
  if (!info_span.has_value()) {
    return std::nullopt;
  }

  TorrentMetadata metadata;
  metadata.info_hash = base::SHA1Hash(*info_span);

  const std::string* name = info->FindString("name");
  if (!name || !IsSafePathComponent(*name)) {
    return std::nullopt;
  }
  metadata.name = *name;

  std::optional<int64_t> piece_length = info->FindInteger("piece length");
  if (!piece_length.has_value() || *piece_length <= 0) {
    return std::nullopt;
  }
  metadata.piece_length = *piece_length;

  const std::string* pieces = info->FindString("pieces");
  if (!pieces || pieces->size() % 20 != 0) {
    return std::nullopt;
  }
  size_t piece_count = pieces->size() / 20;
  if (piece_count > limits.max_pieces) {
    return std::nullopt;
  }
  metadata.piece_hashes.reserve(piece_count);
  base::span<const uint8_t> piece_bytes = base::as_byte_span(*pieces);
  for (size_t i = 0; i < piece_count; ++i) {
    std::array<uint8_t, 20> hash;
    base::span(hash).copy_from(piece_bytes.subspan(i * 20, size_t{20}));
    metadata.piece_hashes.push_back(hash);
  }

  const BencodeValue::List* files_list = info->FindList("files");
  if (files_list) {
    // Multi-file mode.
    if (files_list->empty() || files_list->size() > limits.max_files) {
      return std::nullopt;
    }
    metadata.files.reserve(files_list->size());
    for (const auto& entry : *files_list) {
      std::optional<TorrentFile> file = ParseFileEntry(entry, metadata.name);
      if (!file.has_value()) {
        return std::nullopt;
      }
      metadata.total_length += file->length;
      metadata.files.push_back(std::move(*file));
    }
  } else {
    // Single-file mode.
    std::optional<int64_t> length = info->FindInteger("length");
    if (!length.has_value() || *length < 0) {
      return std::nullopt;
    }
    TorrentFile file;
    file.path = metadata.name;
    file.length = *length;
    metadata.total_length = *length;
    metadata.files.push_back(std::move(file));
  }

  // The piece count must exactly match what the declared total length
  // implies. Any mismatch means the torrent is corrupt or was crafted to
  // desynchronize piece indexing from file layout.
  int64_t expected_pieces =
      metadata.total_length == 0
          ? 0
          : (metadata.total_length + metadata.piece_length - 1) /
                metadata.piece_length;
  if (static_cast<int64_t>(metadata.piece_hashes.size()) != expected_pieces) {
    return std::nullopt;
  }

  std::optional<int64_t> is_private = info->FindInteger("private");
  metadata.is_private = is_private.has_value() && *is_private != 0;

  // Trackers are informational, not integrity-critical — silently truncate
  // at the limit rather than rejecting an otherwise-valid torrent over an
  // oversized tracker list.
  const std::string* announce = root->FindString("announce");
  if (announce) {
    AppendTrackerIfNew(*announce, metadata.trackers, limits.max_trackers);
  }
  const BencodeValue::List* announce_list = root->FindList("announce-list");
  if (announce_list) {
    for (const auto& tier : *announce_list) {
      const auto* tier_list = tier.GetList();
      if (!tier_list) {
        continue;
      }
      for (const auto& url : *tier_list) {
        const auto* url_string = url.GetString();
        if (url_string) {
          AppendTrackerIfNew(*url_string, metadata.trackers,
                              limits.max_trackers);
        }
      }
    }
  }

  return metadata;
}

}  // namespace tbp_download
