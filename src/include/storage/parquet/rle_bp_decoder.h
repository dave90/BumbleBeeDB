//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// rle_bp_decoder.h
//
// Identification: src/include/storage/parquet/rle_bp_decoder.h
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/parquet/resizeable_buffer.h"

namespace bumblebee {

/**
 * @brief Decoder for parquet's hybrid RLE / bit-packed encoding (definition levels, repetition
 * levels, and dictionary offsets).
 */
class RleBpDecoder {
 public:
  RleBpDecoder(const uint8_t *buffer, uint32_t buffer_len, uint32_t bit_width)
      : buffer_(const_cast<char *>(reinterpret_cast<const char *>(buffer)), buffer_len),
        bit_width_(bit_width),
        current_value_(0),
        repeat_count_(0),
        literal_count_(0) {
    if (bit_width >= 64) {
      throw Exception("Decode bit width too large");
    }
    byte_encoded_len_ = ((bit_width_ + 7) / 8);
    max_val_ = (1 << bit_width_) - 1;
  }

  /** @brief Decode `batch_size` values of type `T` into `values_target_ptr`. */
  template <typename T>
  void GetBatch(char *values_target_ptr, uint32_t batch_size) {
    auto *values = reinterpret_cast<T *>(values_target_ptr);
    uint32_t values_read = 0;

    while (values_read < batch_size) {
      if (repeat_count_ > 0) {
        auto repeat_batch = MinValue(batch_size - values_read, static_cast<uint32_t>(repeat_count_));
        std::fill(values + values_read, values + values_read + repeat_batch, static_cast<T>(current_value_));
        repeat_count_ -= repeat_batch;
        values_read += repeat_batch;
      } else if (literal_count_ > 0) {
        uint32_t literal_batch = MinValue(batch_size - values_read, static_cast<uint32_t>(literal_count_));
        uint32_t actual_read = BitUnpack<T>(values + values_read, literal_batch);
        if (literal_batch != actual_read) {
          throw Exception("RLE decode did not find enough values");
        }
        literal_count_ -= literal_batch;
        values_read += literal_batch;
      } else {
        if (!NextCounts()) {
          if (values_read != batch_size) {
            throw Exception("RLE decode did not find enough values");
          }
          return;
        }
      }
    }
    if (values_read != batch_size) {
      throw Exception("RLE decode did not find enough values");
    }
  }

 private:
  ByteBuffer buffer_;

  int bit_width_;
  uint64_t current_value_;
  uint32_t repeat_count_;
  uint32_t literal_count_;
  uint8_t byte_encoded_len_;
  uint32_t max_val_;

  int8_t bitpack_pos_{0};

  static const uint32_t BITPACK_MASKS[];
  static const uint8_t BITPACK_DLEN;

  // Slow but rare: one varint per run header.
  auto VarintDecode() -> uint32_t {
    uint32_t result = 0;
    uint8_t shift = 0;
    while (true) {
      auto byte = buffer_.Read<uint8_t>();
      result |= (byte & 127) << shift;
      if ((byte & 128) == 0) {
        break;
      }
      shift += 7;
      if (shift > 32) {
        throw Exception("Varint-decoding found too large number");
      }
    }
    return result;
  }

  /** @brief Read the next run's header (vlq int; lsb selects literal vs repeated run). */
  auto NextCounts() -> bool {
    if (bitpack_pos_ != 0) {
      buffer_.Inc(1);
      bitpack_pos_ = 0;
    }
    auto indicator_value = VarintDecode();

    bool is_literal = indicator_value & 1;
    if (is_literal) {
      literal_count_ = (indicator_value >> 1) * 8;
    } else {
      repeat_count_ = indicator_value >> 1;
      // (ARROW-4018) this is not big-endian compatible
      current_value_ = 0;
      for (auto i = 0; i < byte_encoded_len_; i++) {
        current_value_ |= (static_cast<uint64_t>(buffer_.Read<uint8_t>()) << (i * 8));
      }
      if (repeat_count_ > 0 && current_value_ > max_val_) {
        throw Exception("Payload value bigger than allowed. Corrupted file?");
      }
    }
    return true;
  }

  template <typename T>
  auto BitUnpack(T *dest, uint32_t count) -> uint32_t {
    auto mask = BITPACK_MASKS[bit_width_];

    uint32_t i = 0;
    // Fast path: while at least 8 bytes remain we can read a 64-bit little-endian window per
    // value (a value spans at most bitpack_pos_(<=7) + bit_width_(<=32) = 39 bits, always
    // inside the window) and advance by whole consumed bytes. This avoids the per-byte
    // boundary loop. Bit order is LSB-first, matching the scalar path below.
    while (i < count && buffer_.len_ >= 8) {
      uint64_t window = Load<uint64_t>(reinterpret_cast<data_ptr_t>(buffer_.ptr_));
      dest[i++] = static_cast<T>((window >> bitpack_pos_) & mask);
      bitpack_pos_ += bit_width_;
      idx_t consumed = bitpack_pos_ >> 3;  // whole bytes now consumed
      buffer_.ptr_ += consumed;
      buffer_.len_ -= consumed;
      bitpack_pos_ &= 7;
    }

    // Tail: byte-by-byte for the final values where fewer than 8 bytes remain.
    for (; i < count; i++) {
      T val = (buffer_.Get<uint8_t>() >> bitpack_pos_) & mask;
      bitpack_pos_ += bit_width_;
      while (bitpack_pos_ > BITPACK_DLEN) {
        buffer_.Inc(1);
        val |= (buffer_.Get<uint8_t>() << (BITPACK_DLEN - (bitpack_pos_ - bit_width_))) & mask;
        bitpack_pos_ -= BITPACK_DLEN;
      }
      dest[i] = val;
    }
    return count;
  }
};

}  // namespace bumblebee
