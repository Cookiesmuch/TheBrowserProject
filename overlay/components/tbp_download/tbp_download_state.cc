// Copyright 2026 The TheBrowserProject Contributors
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include "components/tbp_download/tbp_download_state.h"

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"

namespace tbp_download {

namespace {

// int64_t values are round-tripped as decimal strings rather than JSON
// numbers. A JSON number is a double under the hood, which only represents
// integers exactly up to 2^53 — comfortably enough for any real download,
// but "comfortably enough" is exactly the kind of assumption that should
// never be load-bearing in code whose entire purpose is not losing bytes.
std::string Int64ToJsonString(int64_t value) {
  return base::NumberToString(value);
}

std::optional<int64_t> Int64FromJsonString(const std::string* value) {
  if (!value) {
    return std::nullopt;
  }
  int64_t result;
  if (!base::StringToInt64(*value, &result)) {
    return std::nullopt;
  }
  return result;
}

int ProfileToInt(DownloadProfile profile) {
  return static_cast<int>(profile);
}

std::optional<DownloadProfile> ProfileFromInt(int value) {
  switch (value) {
    case static_cast<int>(DownloadProfile::kMaxPerformance):
    case static_cast<int>(DownloadProfile::kBalanced):
    case static_cast<int>(DownloadProfile::kBackground):
      return static_cast<DownloadProfile>(value);
    default:
      return std::nullopt;
  }
}

}  // namespace

bool DownloadState::operator==(const DownloadState& other) const {
  return url == other.url && total_size == other.total_size &&
         supports_ranges == other.supports_ranges &&
         profile == other.profile && etag == other.etag &&
         last_modified == other.last_modified &&
         completed_chunks == other.completed_chunks;
}

base::FilePath StateFilePathFor(const base::FilePath& destination) {
  return destination.AddExtension(FILE_PATH_LITERAL(".tbpstate"));
}

std::string SerializeDownloadState(const DownloadState& state) {
  base::ListValue chunks;
  for (const Chunk& chunk : state.completed_chunks) {
    base::DictValue chunk_dict;
    chunk_dict.Set("start", Int64ToJsonString(chunk.start));
    chunk_dict.Set("end", Int64ToJsonString(chunk.end));
    chunks.Append(std::move(chunk_dict));
  }

  base::DictValue root;
  root.Set("url", state.url);
  root.Set("total_size", Int64ToJsonString(state.total_size));
  root.Set("supports_ranges", state.supports_ranges);
  root.Set("profile", ProfileToInt(state.profile));
  root.Set("etag", state.etag);
  root.Set("last_modified", state.last_modified);
  root.Set("completed_chunks", std::move(chunks));

  // WriteJson only fails on excessive nesting depth or binary values; this
  // structure is flat and string/int/bool only, so failure here would mean
  // something is structurally wrong with the caller's input, not a runtime
  // condition to recover from gracefully.
  std::optional<std::string> json = base::WriteJson(root);
  return json.value_or(std::string());
}

std::optional<DownloadState> DeserializeDownloadState(
    const std::string& serialized) {
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(serialized, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  const base::DictValue* root = &*parsed;

  const std::string* url = root->FindString("url");
  const std::string* total_size_str = root->FindString("total_size");
  const std::optional<bool> supports_ranges = root->FindBool("supports_ranges");
  const std::optional<int> profile_int = root->FindInt("profile");
  const std::string* etag = root->FindString("etag");
  const std::string* last_modified = root->FindString("last_modified");
  const base::ListValue* chunks = root->FindList("completed_chunks");

  std::optional<int64_t> total_size = Int64FromJsonString(total_size_str);
  if (!url || !total_size.has_value() || !supports_ranges.has_value() ||
      !profile_int.has_value() || !etag || !last_modified || !chunks) {
    return std::nullopt;
  }
  std::optional<DownloadProfile> profile = ProfileFromInt(*profile_int);
  if (!profile.has_value()) {
    return std::nullopt;
  }

  DownloadState state;
  state.url = *url;
  state.total_size = *total_size;
  state.supports_ranges = *supports_ranges;
  state.profile = *profile;
  state.etag = *etag;
  state.last_modified = *last_modified;

  state.completed_chunks.reserve(chunks->size());
  for (const base::Value& entry : *chunks) {
    const base::DictValue* chunk_dict = entry.GetIfDict();
    if (!chunk_dict) {
      return std::nullopt;
    }
    std::optional<int64_t> start =
        Int64FromJsonString(chunk_dict->FindString("start"));
    std::optional<int64_t> end =
        Int64FromJsonString(chunk_dict->FindString("end"));
    if (!start.has_value() || !end.has_value()) {
      return std::nullopt;
    }
    Chunk chunk;
    chunk.start = *start;
    chunk.end = *end;
    state.completed_chunks.push_back(chunk);
  }

  return state;
}

bool SaveDownloadState(const base::FilePath& destination,
                        const DownloadState& state) {
  base::FilePath state_path = StateFilePathFor(destination);
  base::FilePath temp_path = state_path.AddExtension(FILE_PATH_LITERAL(".tmp"));

  std::string serialized = SerializeDownloadState(state);
  if (serialized.empty()) {
    return false;
  }

  // Write-to-temp-then-rename: base::ReplaceFile is atomic on the same
  // volume, so a crash between these two calls leaves either the old state
  // file intact or the new one fully written — never a half-written state
  // file that DeserializeDownloadState would need to detect as corrupt.
  if (!base::WriteFile(temp_path, serialized)) {
    base::DeleteFile(temp_path);
    return false;
  }
  base::File::Error error;
  if (!base::ReplaceFile(temp_path, state_path, &error)) {
    base::DeleteFile(temp_path);
    return false;
  }
  return true;
}

std::optional<DownloadState> LoadDownloadState(
    const base::FilePath& destination) {
  std::string serialized;
  if (!base::ReadFileToString(StateFilePathFor(destination), &serialized)) {
    return std::nullopt;
  }
  return DeserializeDownloadState(serialized);
}

void DeleteDownloadState(const base::FilePath& destination) {
  base::DeleteFile(StateFilePathFor(destination));
}

}  // namespace tbp_download
