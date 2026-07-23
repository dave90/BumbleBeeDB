//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// string_column_reader.cpp
//
// Identification: src/storage/parquet/string_column_reader.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/parquet/string_column_reader.h"

#include "type/vector/vector_data_mngr.h"
#include "utf8proc/utf8proc_wrapper.hpp"

namespace bumblebee {

/** Auxiliary Vector owner that keeps a decompressed page / dictionary buffer alive for as long
 * as any string_t view into it is reachable from the Vector. */
class ParquetStringVectorBuffer : public VectorDataMngr {
 public:
  explicit ParquetStringVectorBuffer(std::shared_ptr<ByteBuffer> buffer)
      : VectorDataMngr(VectorDataMngrType::STRING_BUFFER), buffer_(std::move(buffer)) {}

 private:
  std::shared_ptr<ByteBuffer> buffer_;
};

auto StringParquetValueConversion::DictRead(ByteBuffer &dict, uint32_t &offset, ColumnReader &reader) -> string_t {
  auto &dict_strings = static_cast<StringColumnReader &>(reader).dict_strings_;
  return dict_strings[offset];
}

auto StringParquetValueConversion::PlainRead(ByteBuffer &plain_data, ColumnReader &reader) -> string_t {
  auto &scr = static_cast<StringColumnReader &>(reader);
  uint32_t str_len = scr.fixed_width_string_length_ == 0 ? plain_data.Read<uint32_t>()
                                                         : scr.fixed_width_string_length_;
  plain_data.Available(str_len);
  auto actual_str_len = scr.VerifyString(plain_data.ptr_, str_len);
  auto ret_str = string_t(plain_data.ptr_, actual_str_len);
  plain_data.Inc(str_len);
  return ret_str;
}

void StringParquetValueConversion::PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
  auto &scr = static_cast<StringColumnReader &>(reader);
  uint32_t str_len = scr.fixed_width_string_length_ == 0 ? plain_data.Read<uint32_t>()
                                                         : scr.fixed_width_string_length_;
  plain_data.Available(str_len);
  plain_data.Inc(str_len);
}

auto StringParquetValueConversion::Null() -> string_t { return string_t(""); }

auto StringColumnReader::VerifyString(const char *str_data, uint32_t str_len) -> uint32_t {
  if (logical_type_.GetPhysicalType() != PhysicalType::STRING) {
    return str_len;
  }
  // Verify if a string is actually UTF8, and if there are no null bytes in the middle of it.
  UnicodeInvalidReason reason;
  size_t pos;
  auto utf_type = Utf8Proc::Analyze(str_data, str_len, &reason, &pos);
  if (utf_type == UnicodeType::INVALID) {
    if (reason == UnicodeInvalidReason::NULL_BYTE) {
      // For null bytes we just truncate the string.
      return pos;
    }
    throw Exception("Invalid string encoding found in Parquet file: value is not UTF8!");
  }
  return str_len;
}

void StringColumnReader::Dictionary(std::shared_ptr<ByteBuffer> data, idx_t num_entries) {
  dict_ = std::move(data);
  dict_strings_ = std::unique_ptr<string_t[]>(new string_t[num_entries]);
  for (idx_t dict_idx = 0; dict_idx < num_entries; dict_idx++) {
    uint32_t str_len = dict_->Read<uint32_t>();
    dict_->Available(str_len);

    auto actual_str_len = VerifyString(dict_->ptr_, str_len);
    dict_strings_[dict_idx] = string_t(dict_->ptr_, actual_str_len);
    dict_->Inc(str_len);
  }
}

void StringColumnReader::DictReference(Vector &result) {
  StringVector::AddBuffer(result, vector_data_mngr_ptr_t(new ParquetStringVectorBuffer(dict_)));
}

void StringColumnReader::PlainReference(std::shared_ptr<ByteBuffer> plain_data, Vector &result) {
  StringVector::AddBuffer(result, vector_data_mngr_ptr_t(new ParquetStringVectorBuffer(std::move(plain_data))));
}

}  // namespace bumblebee
