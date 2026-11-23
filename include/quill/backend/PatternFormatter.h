/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/backend/TimestampFormatter.h"
#include "quill/bundled/fmt/base.h"
#include "quill/bundled/fmt/format.h"
#include "quill/core/Attributes.h"
#include "quill/core/BufferUtilities.h"
#include "quill/core/Common.h"
#include "quill/core/MacroMetadata.h"
#include "quill/core/PatternFormatterOptions.h"
#include "quill/core/QuillError.h"
#include "quill/sinks/LogRecord.h"

#ifndef QUILL_MODULE
  #include <array>
  #include <bitset>
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <string>
  #include <string_view>
  #include <utility>
  #include <vector>
#endif

QUILL_BEGIN_NAMESPACE

QUILL_BEGIN_EXPORT

class PatternFormatter
{
  /** Public classes **/
public:
  enum Attribute : uint8_t
  {
    Time = 0,
    FileName,
    CallerFunction,
    LogLevel,
    LogLevelShortCode,
    LineNumber,
    Logger,
    FullPath,
    ThreadId,
    ThreadName,
    ProcessId,
    SourceLocation,
    ShortSourceLocation,
    Message,
    Mdc,
    Tags,
    NamedArgs,
    ATTR_NR_ITEMS
  };

  /** Main PatternFormatter class **/
public:
  /**
   * Constructor for a PatternFormatter with custom formatting options.
   *
   * @param options The PatternFormatterOptions object containing the formatting configuration.
   *                @see PatternFormatterOptions for detailed information on available options.
   *
   * @throws QuillError if the format string in options is invalid
   */
  explicit PatternFormatter(PatternFormatterOptions options)
    : _options(std::move(options)),
      _timestamp_formatter(_options.timestamp_pattern, _options.timestamp_timezone)
  {
    _set_pattern();
  }

  /***/
  PatternFormatter(PatternFormatter const& other) = delete;
  PatternFormatter& operator=(PatternFormatter const& other) = delete;

  /***/
  PatternFormatter& operator=(PatternFormatter&& other) noexcept = default;
  PatternFormatter(PatternFormatter&& other) noexcept = default;

  /***/
  ~PatternFormatter() = default;

  /**
   * Formats a log record with this pattern. The record's log_statement is not used.
   * @return the formatted statement, valid until the next call
   */
  QUILL_NODISCARD QUILL_ATTRIBUTE_HOT std::string_view format(LogRecord const& record)
  {
    if (_options.format_pattern.empty())
    {
      // No formatting is needed when the format pattern is empty.
      // For example, in JsonFileSink, we can retrieve the MacroMetadata and the named arguments as
      // key-value pairs, but we do not need to format the log statement.
      return std::string_view{};
    }

    // clear out the existing buffer
    _formatted_log_message_buffer.clear();

    std::string_view const log_msg = record.log_message;

    if (QUILL_UNLIKELY(log_msg.empty()))
    {
      // Process an empty message
      return _format(record, log_msg);
    }

    std::string_view formatted_log_msg;

    // Check if we need to handle multi-line formatting
    if (_options.add_metadata_to_multi_line_logs && (!record.named_args || record.named_args->empty()))
    {
      // multi line metadata only supported when named_args are not used
      size_t start = 0;

      while (start < log_msg.size())
      {
        size_t const end = log_msg.find_first_of('\n', start);

        if (end == std::string_view::npos)
        {
          // Handle the last line or a single line without a newline
          formatted_log_msg =
            _format(record, std::string_view(log_msg.data() + start, log_msg.size() - start));
          break;
        }

        // Write the current line
        size_t line_length = end - start;
        if (_options.pattern_suffix != '\n')
        {
          // When suffix is not '\n', include the newline character in the message
          line_length++;
        }

        formatted_log_msg = _format(record, std::string_view(log_msg.data() + start, line_length));
        start = end + 1;
      }
    }
    else
    {
      // Use the regular format method for single-line messages
      QUILL_ASSERT(
        !log_msg.empty(),
        "log_msg should not be empty, already checked earlier in PatternFormatter::format()");
      size_t log_message_size = log_msg.size();

      if (_options.pattern_suffix == '\n' && log_msg[log_msg.size() - 1] == '\n')
      {
        // if the log_message ends with \n we exclude it (only when using newline suffix)
        log_message_size = log_msg.size() - 1;
      }

      formatted_log_msg = _format(record, std::string_view{log_msg.data(), log_message_size});
    }

    return formatted_log_msg;
  }

  /**
   * Formats a log statement from its individual fields, see format(LogRecord const&).
   * @return the formatted statement, valid until the next call
   */
  QUILL_NODISCARD std::string_view format(uint64_t timestamp, std::string_view thread_id,
                                          std::string_view thread_name, std::string_view process_id,
                                          std::string_view logger, std::string_view log_level_description,
                                          std::string_view log_level_short_code,
                                          MacroMetadata const& log_statement_metadata,
                                          std::vector<std::pair<std::string, std::string>> const* named_args,
                                          std::string_view log_msg, std::string_view mdc)
  {
    LogRecord record;
    record.log_metadata = &log_statement_metadata;
    record.log_timestamp = timestamp;
    record.thread_id = thread_id;
    record.thread_name = thread_name;
    record.process_id = process_id;
    record.logger_name = logger;
    record.log_level = log_statement_metadata.log_level();
    record.log_level_description = log_level_description;
    record.log_level_short_code = log_level_short_code;
    record.named_args = named_args;
    record.log_message = log_msg;
    record.mdc = mdc;
    return format(record);
  }

  /***/
  QUILL_NODISCARD PatternFormatterOptions const& get_options() const noexcept { return _options; }

  /**
   * @return empty, or a warning that the timestamp pattern is formatted with strftime() on every
   * call because it contains a time-of-day specifier that cannot be cached, e.g. `%-H`
   */
  QUILL_NODISCARD std::string const& timestamp_pattern_warning() const noexcept
  {
    return _timestamp_formatter.warning();
  }

protected:
  /***/
  QUILL_NODISCARD static std::string_view _process_source_location_path(std::string_view source_location,
                                                                        std::string const& strip_prefix,
                                                                        bool remove_relative_paths)
  {
    std::string_view result = source_location;

    // First, handle removal of relative paths if requested
    if (remove_relative_paths)
    {
      // Remove any relative paths (e.g., relative paths can appear when using a mounted volume under docker)

      size_t const forward_slash_pos = result.rfind("../");
      size_t const backslash_pos = result.rfind("..\\");
      size_t relative_path_pos{forward_slash_pos};

      bool const use_backslash_pos = (backslash_pos != std::string_view::npos) &&
        ((relative_path_pos == std::string_view::npos) || (backslash_pos > relative_path_pos));
      if (use_backslash_pos)
      {
        relative_path_pos = backslash_pos;
      }

      if (relative_path_pos != std::string_view::npos)
      {
        result = result.substr(relative_path_pos + 3u);
      }
    }

    // Then handle prefix stripping
    if (!strip_prefix.empty())
    {
      // Find the last occurrence of the prefix in the path
      size_t prefix_pos = result.rfind(strip_prefix);

      if (prefix_pos != std::string_view::npos)
      {
        size_t after_prefix_pos = prefix_pos + strip_prefix.size();

        // If the prefix doesn't end with a separator and there is a character after the prefix
        // and that character is a separator, skip it as well
        if (after_prefix_pos < result.length() &&
            ((result[after_prefix_pos] == '/') || (result[after_prefix_pos] == '\\')))
        {
          after_prefix_pos++;
        }

        return result.substr(after_prefix_pos);
      }
      // Prefix not found, use the full path
    }

    // No prefix set or prefix not found, use the full path
    return result;
  }

private:
  /** A pre-parsed attribute and the literal text immediately preceding it. */
  struct FormatPart
  {
    size_t literal_pos{0};
    size_t literal_size{0};
    Attribute attribute{Attribute::ATTR_NR_ITEMS};
    fmtquill::format_specs specs;
    bool has_format_specs{false};

    /** The specs only pad the value to a width, which _format() can do itself for ASCII text */
    bool is_padding_only{false};
  };

  /** Attribute names, at the same index as the Attribute enum **/
  static constexpr std::array<std::string_view, Attribute::ATTR_NR_ITEMS> _attribute_names{
    "time",
    "file_name",
    "caller_function",
    "log_level",
    "log_level_short_code",
    "line_number",
    "logger",
    "full_path",
    "thread_id",
    "thread_name",
    "process_id",
    "source_location",
    "short_source_location",
    "message",
    "mdc",
    "tags",
    "named_args"};

  /**
   * Splits the pattern into literal text and %(attribute[:format_spec]) parts.
   * e.g. "%(time) [%(thread_id)] %(log_level:<12) - " becomes
   *   {"", Time}, {" [", ThreadId}, {"] ", LogLevel, "<12"} and the trailing literal " - "
   */
  void _set_pattern()
  {
    std::string pattern = _options.format_pattern;

    if (_options.pattern_suffix != PatternFormatterOptions::NO_SUFFIX)
    {
      pattern += _options.pattern_suffix;
    }

    size_t literal_begin = 0;
    size_t pos = 0;

    while (pos < pattern.size())
    {
      char const c = pattern[pos];
      bool const has_next = (pos + 1u) < pattern.size();

      if ((c == '{') || (c == '}'))
      {
        // Literal braces can be written directly; the doubled fmt spelling yields one brace
        _literal_buffer += c;
        pos += (has_next && (pattern[pos + 1u] == c)) ? 2u : 1u;
        continue;
      }

      if ((c != '%') || !has_next || (pattern[pos + 1u] != '('))
      {
        _literal_buffer += c;
        ++pos;
        continue;
      }

      size_t const closed_paren_pos = pattern.find(')', pos + 2u);

      if (closed_paren_pos == std::string::npos)
      {
        QUILL_THROW(QuillError{"Invalid format pattern"});
      }

      std::string_view const attr{pattern.data() + pos + 2u, closed_paren_pos - (pos + 2u)};
      size_t const colon_pos = attr.find(':');
      std::string_view const attr_name = attr.substr(0, colon_pos);

      size_t attr_index = 0;
      while ((attr_index < _attribute_names.size()) && (_attribute_names[attr_index] != attr_name))
      {
        ++attr_index;
      }

      if (attr_index == _attribute_names.size())
      {
        QUILL_THROW(QuillError{"Invalid format pattern, attribute with name \"" +
                               std::string{attr_name} + "\" is invalid"});
      }

      if (_is_set_in_pattern.test(attr_index))
      {
        QUILL_THROW(QuillError{"Invalid format pattern, attribute with name \"" +
                               std::string{attr_name} + "\" is used more than once"});
      }

      _is_set_in_pattern.set(attr_index);

      FormatPart part{
        literal_begin, _literal_buffer.size() - literal_begin, static_cast<Attribute>(attr_index), {}, false};

      if (colon_pos != std::string_view::npos)
      {
        part.specs = _parse_format_specs(attr.substr(colon_pos + 1u));
        part.has_format_specs = true;
        part.is_padding_only = _is_padding_only(part.specs);
        _has_format_specs_other_than_padding |= !part.is_padding_only;
        _has_padding |= part.is_padding_only;
      }

      _format_parts.push_back(part);
      literal_begin = _literal_buffer.size();
      pos = closed_paren_pos + 1u;
    }

    _trailing_literal_pos = literal_begin;
  }

  /***/
  QUILL_NODISCARD static fmtquill::format_specs _parse_format_specs(std::string_view spec)
  {
    fmtquill::detail::dynamic_format_specs<char> specs;

    auto const parse = [&specs, spec]()
    {
      fmtquill::parse_context<char> parse_context{fmtquill::string_view{spec.data(), spec.size()}};
      char const* const end = spec.data() + spec.size();
      char const* const parsed_end = fmtquill::detail::parse_format_specs(
        spec.data(), end, specs, parse_context, fmtquill::detail::type::string_type);

      if (parsed_end != end)
      {
        fmtquill::report_error("invalid format specifier");
      }

      if (specs.dynamic())
      {
        fmtquill::report_error("width/precision is not integer");
      }
    };

#if defined(QUILL_NO_EXCEPTIONS)
    parse();
#else
    try
    {
      parse();
    }
    catch (fmtquill::format_error const& error)
    {
      QUILL_THROW(QuillError{"Invalid format pattern: " + std::string{error.what()}});
    }
#endif

    return specs;
  }

  /** @return the fill needed to pad `value`, measured as ASCII, to the width of `part` */
  QUILL_NODISCARD static size_t _padding(FormatPart const& part, std::string_view value) noexcept
  {
    auto const width = static_cast<size_t>(part.specs.width);
    return (width > value.size()) ? (width - value.size()) : 0;
  }

  /**
   * @return true if `specs` only set a width, with an optional single-byte fill and an alignment.
   * fmt pads a string by its display width, which it measures by decoding every code point. The
   * display width of ASCII text is its length, so _format() pads ASCII values itself.
   */
  QUILL_NODISCARD static bool _is_padding_only(fmtquill::format_specs const& specs) noexcept
  {
    fmtquill::align const align = specs.align();

    return (specs.width > 0) && (specs.precision < 0) &&
      (specs.dynamic_width() == fmtquill::arg_id_kind::none) &&
      (specs.dynamic_precision() == fmtquill::arg_id_kind::none) &&
      ((specs.type() == fmtquill::presentation_type::none) ||
       (specs.type() == fmtquill::presentation_type::string)) &&
      (specs.sign() == fmtquill::sign::none) && !specs.alt() && !specs.localized() &&
      (specs.fill_size() == 1) && (align != fmtquill::align::numeric);
  }

  /** @return the end of the source location, which the metadata measured when it was created */
  QUILL_NODISCARD static char const* _source_location_end(MacroMetadata const& metadata) noexcept
  {
    return metadata.source_location() + metadata.source_location_size();
  }

  /***/
  template <size_t I, typename T>
  void _set_arg_val(T const& arg)
  {
    _attribute_values[I] = arg;
  }

  /**
   * Formats the named arguments as "name: value, name: value". Out of line so that its appends do
   * not stop the compiler from inlining the ones in _format()
   */
  QUILL_NOINLINE std::string_view _format_named_args(std::vector<std::pair<std::string, std::string>> const* named_args)
  {
    _formatted_named_args_buffer.clear();

    if (named_args)
    {
      for (size_t i = 0; i < named_args->size(); ++i)
      {
        detail::append_to_buffer(_formatted_named_args_buffer, (*named_args)[i].first);
        detail::append_to_buffer(_formatted_named_args_buffer, std::string_view{": "});
        detail::append_to_buffer(_formatted_named_args_buffer, (*named_args)[i].second);

        if (i != named_args->size() - 1)
        {
          detail::append_to_buffer(_formatted_named_args_buffer, std::string_view{", "});
        }
      }
    }

    return std::string_view{_formatted_named_args_buffer.data(), _formatted_named_args_buffer.size()};
  }

  /**
   * Appends the statement when every piece is a plain string or a padded string. The statement is
   * reserved once and the pieces are copied through a local pointer instead of checking and
   * updating the buffer's size for each one.
   * @return false, with nothing appended, if a padded value is not ASCII
   */
  template <bool HasPadding>
  QUILL_ATTRIBUTE_HOT QUILL_ALWAYS_INLINE bool _format_pieces()
  {
    size_t total_size = _literal_buffer.size();

    for (FormatPart const& part : _format_parts)
    {
      std::string_view const value = _attribute_values[part.attribute];
      total_size += value.size();

      if constexpr (HasPadding)
      {
        if (part.is_padding_only)
        {
          if (QUILL_UNLIKELY(!detail::is_ascii(value)))
          {
            return false;
          }

          total_size += _padding(part, value);
        }
      }
    }

    size_t const offset = _formatted_log_message_buffer.size();

    if constexpr (HasPadding)
    {
      // The fill is written in blocks of 8 bytes, which may run past the statement
      _formatted_log_message_buffer.try_resize(offset + total_size + sizeof(uint64_t));
    }

    _formatted_log_message_buffer.try_resize(offset + total_size);

    char* write_pos = _formatted_log_message_buffer.data() + offset;
    char const* const literals = _literal_buffer.data();

    for (FormatPart const& part : _format_parts)
    {
      write_pos = detail::copy_to(write_pos, literals + part.literal_pos, part.literal_size);

      std::string_view const value = _attribute_values[part.attribute];

      if constexpr (HasPadding)
      {
        if (part.is_padding_only)
        {
          size_t const padding = _padding(part, value);

          // The same split as fmt's write_padded(): strings align left by default
          size_t const left_padding = (part.specs.align() == fmtquill::align::right) ? padding
            : (part.specs.align() == fmtquill::align::center)                        ? padding / 2
                                                                                     : 0;
          char const fill = part.specs.fill_unit<char>();

          write_pos = detail::fill_to(write_pos, fill, left_padding);
          write_pos = detail::copy_to(write_pos, value.data(), value.size());
          write_pos = detail::fill_to(write_pos, fill, padding - left_padding);
          continue;
        }
      }

      write_pos = detail::copy_to(write_pos, value.data(), value.size());
    }

    (void)detail::copy_to(write_pos, literals + _trailing_literal_pos, _literal_buffer.size() - _trailing_literal_pos);

    return true;
  }

  /** _format_pieces() for patterns with padding, out of line so that _format() stays small */
  QUILL_NOINLINE bool _format_padded_pieces() { return _format_pieces<true>(); }

  /***/
  QUILL_ATTRIBUTE_HOT std::string_view _format(LogRecord const& record, std::string_view log_msg)
  {
    MacroMetadata const& log_statement_metadata = *record.log_metadata;
    std::vector<std::pair<std::string, std::string>> const* named_args = record.named_args;

    if (_is_set_in_pattern[Attribute::Time])
    {
      _set_arg_val<Attribute::Time>(
        _timestamp_formatter.format_timestamp(std::chrono::nanoseconds{record.log_timestamp}));
    }

    if (_is_set_in_pattern[Attribute::FileName])
    {
      _set_arg_val<Attribute::FileName>(log_statement_metadata.file_name());
    }

    if (_is_set_in_pattern[Attribute::CallerFunction])
    {
      std::string_view const function_name = _options.process_function_name
        ? _options.process_function_name(log_statement_metadata.caller_function())
        : std::string_view{log_statement_metadata.caller_function()};

      _set_arg_val<Attribute::CallerFunction>(function_name);
    }

    if (_is_set_in_pattern[Attribute::LogLevel])
    {
      _set_arg_val<Attribute::LogLevel>(record.log_level_description);
    }

    if (_is_set_in_pattern[Attribute::LogLevelShortCode])
    {
      _set_arg_val<Attribute::LogLevelShortCode>(record.log_level_short_code);
    }

    if (_is_set_in_pattern[Attribute::LineNumber])
    {
      char const* const line = log_statement_metadata.line();
      _set_arg_val<Attribute::LineNumber>(std::string_view{
        line, static_cast<size_t>(_source_location_end(log_statement_metadata) - line)});
    }

    if (_is_set_in_pattern[Attribute::Logger])
    {
      _set_arg_val<Attribute::Logger>(record.logger_name);
    }

    if (_is_set_in_pattern[Attribute::FullPath])
    {
      _set_arg_val<Attribute::FullPath>(log_statement_metadata.full_path());
    }

    if (_is_set_in_pattern[Attribute::ThreadId])
    {
      _set_arg_val<Attribute::ThreadId>(record.thread_id);
    }

    if (_is_set_in_pattern[Attribute::ThreadName])
    {
      _set_arg_val<Attribute::ThreadName>(record.thread_name);
    }

    if (_is_set_in_pattern[Attribute::ProcessId])
    {
      _set_arg_val<Attribute::ProcessId>(record.process_id);
    }

    if (_is_set_in_pattern[Attribute::SourceLocation])
    {
      _set_arg_val<Attribute::SourceLocation>(_process_source_location_path(
        log_statement_metadata.source_location(), _options.source_location_path_strip_prefix,
        _options.source_location_remove_relative_paths));
    }

    if (_is_set_in_pattern[Attribute::ShortSourceLocation])
    {
      char const* const short_source_location = log_statement_metadata.short_source_location();
      _set_arg_val<Attribute::ShortSourceLocation>(std::string_view{
        short_source_location,
        static_cast<size_t>(_source_location_end(log_statement_metadata) - short_source_location)});
    }

    if (_is_set_in_pattern[Attribute::NamedArgs])
    {
      _set_arg_val<Attribute::NamedArgs>(_format_named_args(named_args));
    }

    if (_is_set_in_pattern[Attribute::Mdc])
    {
      _set_arg_val<Attribute::Mdc>(record.mdc);
    }

    if (_is_set_in_pattern[Attribute::Tags])
    {
      if (log_statement_metadata.tags())
      {
        if (_options.process_tags)
        {
          _processed_tags = _options.process_tags(log_statement_metadata.tags());
          _set_arg_val<Attribute::Tags>(_processed_tags);
        }
        else
        {
          _set_arg_val<Attribute::Tags>(std::string_view{log_statement_metadata.tags()});
        }
      }
      else
      {
        _set_arg_val<Attribute::Tags>(std::string_view{});
      }
    }

    _set_arg_val<Attribute::Message>(log_msg);

    if (!_has_format_specs_other_than_padding)
    {
      // Every piece is a plain string or an ASCII string padded to a width. Patterns without
      // padding get a copy loop without the padding checks
      bool const formatted = _has_padding ? _format_padded_pieces() : _format_pieces<false>();

      if (QUILL_LIKELY(formatted))
      {
        return std::string_view{_formatted_log_message_buffer.data(), _formatted_log_message_buffer.size()};
      }
    }

    fmtquill::appender out{_formatted_log_message_buffer};

    for (FormatPart const& part : _format_parts)
    {
      detail::append_to_buffer(_formatted_log_message_buffer,
                               std::string_view{_literal_buffer.data() + part.literal_pos, part.literal_size});

      std::string_view const value = _attribute_values[part.attribute];
      if (!part.has_format_specs)
      {
        // All pattern attributes are string views, so the common case needs no fmt dispatch.
        detail::append_to_buffer(_formatted_log_message_buffer, value);
      }
      else
      {
        (void)fmtquill::detail::write<char>(out, fmtquill::string_view{value.data(), value.size()},
                                            part.specs);
      }
    }

    detail::append_to_buffer(_formatted_log_message_buffer,
                             std::string_view{_literal_buffer.data() + _trailing_literal_pos,
                                              _literal_buffer.size() - _trailing_literal_pos});

    return std::string_view{_formatted_log_message_buffer.data(), _formatted_log_message_buffer.size()};
  }

private:
  PatternFormatterOptions _options;
  std::string _literal_buffer;
  std::vector<FormatPart> _format_parts;
  size_t _trailing_literal_pos{0};
  bool _has_format_specs_other_than_padding{false};
  bool _has_padding{false};

  std::array<std::string_view, Attribute::ATTR_NR_ITEMS> _attribute_values{};
  std::bitset<Attribute::ATTR_NR_ITEMS> _is_set_in_pattern;

  /** class responsible for formatting the timestamp */
  detail::TimestampFormatter _timestamp_formatter;

  /** The buffer where we store each formatted string, also stored as class member to avoid
   * re-allocations **/
  fmtquill::basic_memory_buffer<char, 512> _formatted_log_message_buffer;
  fmtquill::basic_memory_buffer<char, 512> _formatted_named_args_buffer;
  std::string _processed_tags;
};

QUILL_END_EXPORT

QUILL_END_NAMESPACE
