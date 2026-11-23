/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/core/Attributes.h"

#include "quill/bundled/fmt/base.h"

#ifndef QUILL_MODULE
  #include <cstddef>
  #include <cstdint>
  #include <cstring>
  #include <string_view>
#endif

QUILL_BEGIN_NAMESPACE

namespace detail
{
/**
 * Copies `size` bytes from `src` to `dest`, which must have room for them, and returns the end of
 * the copy. Up to 16 bytes are copied with two overlapping fixed-size moves, which read and write
 * only the bytes of the source, and longer text with one memcpy, since a call to memcpy costs more
 * than copying a few bytes.
 */
QUILL_ATTRIBUTE_HOT QUILL_ALWAYS_INLINE char* copy_to(char* dest, char const* src, size_t size) noexcept
{
  if (size > 16)
  {
    std::memcpy(dest, src, size);
  }
  else if (size >= 8)
  {
    uint64_t head;
    uint64_t tail;
    std::memcpy(&head, src, sizeof(head));
    std::memcpy(&tail, src + size - sizeof(tail), sizeof(tail));
    std::memcpy(dest, &head, sizeof(head));
    std::memcpy(dest + size - sizeof(tail), &tail, sizeof(tail));
  }
  else if (size >= 4)
  {
    uint32_t head;
    uint32_t tail;
    std::memcpy(&head, src, sizeof(head));
    std::memcpy(&tail, src + size - sizeof(tail), sizeof(tail));
    std::memcpy(dest, &head, sizeof(head));
    std::memcpy(dest + size - sizeof(tail), &tail, sizeof(tail));
  }
  else if (size != 0)
  {
    dest[0] = src[0];
    dest[size / 2] = src[size / 2];
    dest[size - 1] = src[size - 1];
  }

  return dest + size;
}

/**
 * Writes `count` copies of `fill` to `dest` and returns the end of the fill. The fill is written in
 * blocks of 8 bytes, so `dest` must have room for up to 7 bytes more than `count`.
 */
QUILL_ATTRIBUTE_HOT inline char* fill_to(char* dest, char fill, size_t count) noexcept
{
  uint64_t const fill_block = static_cast<uint64_t>(static_cast<unsigned char>(fill)) * 0x0101010101010101ull;

  for (size_t written = 0; written < count; written += sizeof(fill_block))
  {
    std::memcpy(dest + written, &fill_block, sizeof(fill_block));
  }

  return dest + count;
}

/** @return true if every byte of `text` is ASCII */
QUILL_NODISCARD QUILL_ATTRIBUTE_HOT inline bool is_ascii(std::string_view text) noexcept
{
  char const* const data = text.data();
  size_t const size = text.size();
  uint64_t bits{0};
  size_t pos{0};

  for (; (pos + sizeof(uint64_t)) <= size; pos += sizeof(uint64_t))
  {
    uint64_t block;
    std::memcpy(&block, data + pos, sizeof(block));
    bits |= block;
  }

  for (; pos < size; ++pos)
  {
    bits |= static_cast<unsigned char>(data[pos]);
  }

  return (bits & 0x8080808080808080ull) == 0;
}

/** Appends text that does not fit the buffer's current capacity */
QUILL_NOINLINE inline void append_to_buffer_and_grow(fmtquill::detail::buffer<char>& buffer, std::string_view text)
{
  // fmt's append() grows the buffer, or flushes a buffer with a fixed capacity, as needed
  buffer.append(text.data(), text.data() + text.size());
}

/**
 * Appends `text` to `buffer`.
 *
 * The backend builds each log statement from many short pieces, such as pattern literals, the
 * log level and the thread id. When the text fits the buffer's capacity it is copied with
 * copy_to(). Text that needs the buffer to grow is appended out of line, so this function stays
 * small enough to inline.
 */
QUILL_ATTRIBUTE_HOT inline void append_to_buffer(fmtquill::detail::buffer<char>& buffer, std::string_view text)
{
  size_t const size = text.size();
  size_t const offset = buffer.size();
  size_t const new_size = offset + size;

  if (QUILL_UNLIKELY(new_size > buffer.capacity()))
  {
    append_to_buffer_and_grow(buffer, text);
    return;
  }

  // The text fits the capacity, so this only sets the size. The check above is the same expression
  // that try_resize() tests before growing, which lets the compiler drop its grow call.
  buffer.try_resize(new_size);

  (void)copy_to(buffer.data() + offset, text.data(), size);
}
} // namespace detail

QUILL_END_NAMESPACE
