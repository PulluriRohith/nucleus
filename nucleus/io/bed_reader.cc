/*
 * Copyright 2018 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Implementation of bed_reader.h
#include "nucleus/io/bed_reader.h"

#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"

#include "nucleus/protos/bed.pb.h"
#include "nucleus/util/utils.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "nucleus/platform/types.h"

namespace nucleus {

namespace tf = tensorflow;

// 256 KB read buffer.
constexpr int READER_BUFFER_SIZE = 256 * 1024;

// BED-specific attributes.
constexpr char BED_COMMENT_PREFIX[] = "#";

// -----------------------------------------------------------------------------
//
// Reader for BED format data.
//
// -----------------------------------------------------------------------------

namespace {

bool ValidNumBedFields(const int fields) {
  return (fields == 3 || fields == 4 || fields == 5 || fields == 6 ||
          fields == 8 || fields == 9 || fields == 12);
}

// Read the next non-comment line.
tf::Status NextNonCommentLine(TextReader& text_reader,
                              string* line) {
  CHECK(line != nullptr);
  string tmp;
  do {
    StatusOr<string> line_or = text_reader.ReadLine();
    TF_RETURN_IF_ERROR(line_or.status());
    tmp = line_or.ValueOrDie();
  } while (absl::StartsWith(tmp, BED_COMMENT_PREFIX));

  *line = tmp;
  return tf::Status::OK();
}

tf::Status ConvertToPb(const string& line, const int desiredNumFields,
                       int* numTokensSeen,
                       nucleus::genomics::v1::BedRecord* record) {
  CHECK(record != nullptr) << "BED record cannot be null";
  record->Clear();

  std::vector<string> tokens = absl::StrSplit(line, '\t');
  int numTokens = static_cast<int>(tokens.size());
  *numTokensSeen = numTokens;
  if (!ValidNumBedFields(numTokens)) {
    return tf::errors::Unknown("BED record has invalid number of fields");
  }
  int numFields =
      desiredNumFields == 0 ? numTokens : std::min(numTokens, desiredNumFields);
  int64 int64Value;
  record->set_reference_name(tokens[0]);
  CHECK(absl::SimpleAtoi(tokens[1], &int64Value));
  record->set_start(int64Value);
  CHECK(absl::SimpleAtoi(tokens[2], &int64Value));
  record->set_end(int64Value);
  if (numFields > 3) record->set_name(tokens[3]);
  if (numFields > 4) {
    double value;
    CHECK(absl::SimpleAtod(tokens[4].c_str(), &value));
    record->set_score(value);
  }
  if (numFields > 5) {
    if (tokens[5] == "+")
      record->set_strand(nucleus::genomics::v1::BedRecord::FORWARD_STRAND);
    else if (tokens[5] == "-")
      record->set_strand(nucleus::genomics::v1::BedRecord::REVERSE_STRAND);
    else if (tokens[5] == ".")
      record->set_strand(nucleus::genomics::v1::BedRecord::NO_STRAND);
    else
      return tf::errors::Unknown("Invalid BED record with unknown strand");
  }
  if (numFields > 7) {
    CHECK(absl::SimpleAtoi(tokens[6], &int64Value));
    record->set_thick_start(int64Value);
    CHECK(absl::SimpleAtoi(tokens[7], &int64Value));
    record->set_thick_end(int64Value);
  }
  if (numFields > 8) record->set_item_rgb(tokens[8]);
  if (numFields >= 12) {
    int32 int32Value;
    CHECK(absl::SimpleAtoi(tokens[9], &int32Value));
    record->set_block_count(int32Value);
    record->set_block_sizes(tokens[10]);
    record->set_block_starts(tokens[11]);
  }

  return tf::Status::OK();
}

// Peeks into the path to the first BED record and returns the number of fields
// in the record.
// NOTE: This is quite heavyweight. Reading upon initialization and then
// rewinding the stream to 0 would be a nicer solution.
tf::Status GetNumFields(const string& path, int* numFields) {
  CHECK(numFields != nullptr);
  string line;
  StatusOr<std::unique_ptr<TextReader>> status_or = TextReader::FromFile(path);
  TF_RETURN_IF_ERROR(status_or.status());
  std::unique_ptr<TextReader> text_reader = std::move(status_or.ValueOrDie());
  TF_RETURN_IF_ERROR(NextNonCommentLine(*text_reader, &line));
  TF_RETURN_IF_ERROR(text_reader->Close());
  std::vector<string> tokens = absl::StrSplit(line, '\t');
  *numFields = static_cast<int>(tokens.size());
  return tf::Status::OK();
}
}  // namespace

// Iterable class for traversing all BED records in the file.
class BedFullFileIterable : public BedIterable {
 public:
  // Advance to the next record.
  StatusOr<bool> Next(nucleus::genomics::v1::BedRecord* out) override;

  // Constructor is invoked via BedReader::Iterate.
  BedFullFileIterable(const BedReader* reader);
  ~BedFullFileIterable() override;
};

StatusOr<std::unique_ptr<BedReader>> BedReader::FromFile(
    const string& bed_path,
    const nucleus::genomics::v1::BedReaderOptions& options) {
  int numFieldsInBed;
  TF_RETURN_IF_ERROR(GetNumFields(bed_path, &numFieldsInBed));
  nucleus::genomics::v1::BedHeader header;
  header.set_num_fields(numFieldsInBed);
  // Ensure options are valid.
  if (options.num_fields() != 0 && (options.num_fields() > numFieldsInBed ||
                                    !ValidNumBedFields(options.num_fields()))) {
    return tf::errors::InvalidArgument(
        "Invalid requested number of fields to parse");
  }
  StatusOr<std::unique_ptr<TextReader>> status_or =
      TextReader::FromFile(bed_path);
  TF_RETURN_IF_ERROR(status_or.status());
  return std::unique_ptr<BedReader>(
      new BedReader(std::move(status_or.ValueOrDie()), options, header));
}

BedReader::BedReader(std::unique_ptr<TextReader> text_reader,
                     const nucleus::genomics::v1::BedReaderOptions& options,
                     const nucleus::genomics::v1::BedHeader& header)
    : options_(options), header_(header), text_reader_(std::move(text_reader))
{}

BedReader::~BedReader() {
  if (text_reader_) {
    TF_CHECK_OK(Close());
  }
}

tf::Status BedReader::Close() {
  if (!text_reader_) {
    return tf::errors::FailedPrecondition("BedReader already closed");
  }
  tf::Status status = text_reader_->Close();
  text_reader_ = nullptr;
  return status;
}

// Ensures the number of fields is consistent across all records in the BED.
tf::Status BedReader::Validate(const int numTokens) const {
  if (header_.num_fields() != numTokens) {
    return tf::errors::Unknown(
        "Invalid BED with varying number of fields in file");
  }
  return tf::Status::OK();
}

StatusOr<std::shared_ptr<BedIterable>> BedReader::Iterate() const {
  if (!text_reader_)
    return tf::errors::FailedPrecondition("Cannot Iterate a closed BedReader.");
  return StatusOr<std::shared_ptr<BedIterable>>(
      MakeIterable<BedFullFileIterable>(this));
}

// Iterable class definitions.
StatusOr<bool> BedFullFileIterable::Next(
    nucleus::genomics::v1::BedRecord* out) {
  TF_RETURN_IF_ERROR(CheckIsAlive());
  const BedReader* bed_reader = static_cast<const BedReader*>(reader_);
  string line;
  tf::Status status = NextNonCommentLine(*bed_reader->text_reader_, &line);
  if (tf::errors::IsOutOfRange(status)) {
    return false;
  } else if (!status.ok()) {
    return status;
  }
  int numTokens;
  TF_RETURN_IF_ERROR(
      ConvertToPb(line, bed_reader->Options().num_fields(), &numTokens, out));
  TF_RETURN_IF_ERROR(bed_reader->Validate(numTokens));
  return true;
}

BedFullFileIterable::~BedFullFileIterable() {}

BedFullFileIterable::BedFullFileIterable(const BedReader* reader)
    : Iterable(reader) {}

}  // namespace nucleus
