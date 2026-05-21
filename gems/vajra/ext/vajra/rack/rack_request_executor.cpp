// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_request_executor.hpp"

#include "ipc/frame_header.hpp"
#include "request/http_field_utils.hpp"
#include "request/rack_env.hpp"
#include "request/request_head_error.hpp"
#include "ruby.h"
#include "ruby/encoding.h"
#include "ruby/thread.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
  std::atomic<bool> rack_execution_callback_installed_flag{false};
  std::mutex rack_execution_callback_mutex;
  VALUE rack_execution_callback = Qnil;
  ID id_exception_message;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    const std::string *request_body;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  struct ResponseNormalizationContext
  {
    VALUE result = Qnil;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  enum class RequestBodyEvent : std::uint8_t
  {
    chunk = 1,
    complete = 2,
    cancel = 3,
  };

  enum class ResponseMetadataKind : std::uint8_t
  {
    no_response = 0,
    response = 1,
    head_error = 2,
    execution_error = 3,
  };

  enum class ResponseBodyEvent : std::uint8_t
  {
    chunk = 1,
    complete = 2,
  };

  constexpr std::size_t kInlineBodyChunkBytes =
      static_cast<std::size_t>(Vajra::ipc::kMaxFramePayloadLength) - 1;
  constexpr std::size_t kMillisecondsPerSecond = 1000;
  struct DecodedResponseMetadata
  {
    ResponseMetadataKind kind = ResponseMetadataKind::no_response;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  int clamp_poll_timeout_milliseconds(std::size_t timeout_seconds)
  {
    constexpr std::size_t kMaxPollTimeoutMilliseconds = static_cast<std::size_t>(std::numeric_limits<int>::max());
    const std::size_t timeout_milliseconds = timeout_seconds > (kMaxPollTimeoutMilliseconds / kMillisecondsPerSecond)
                                                 ? kMaxPollTimeoutMilliseconds
                                                 : timeout_seconds * kMillisecondsPerSecond;
    return static_cast<int>(timeout_milliseconds);
  }

  void write_all_or_throw(int fd, const void *buffer, std::size_t length)
  {
    const auto *bytes = static_cast<const std::uint8_t *>(buffer);
    std::size_t written = 0;
    while (written < length)
    {
      const ssize_t result = write(fd, bytes + written, length - written);
      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        throw std::runtime_error("request channel write failed");
      }

      written += static_cast<std::size_t>(result);
    }
  }

  bool read_exact_or_eof(int fd, void *buffer, std::size_t length)
  {
    auto *bytes = static_cast<std::uint8_t *>(buffer);
    std::size_t read_bytes = 0;
    while (read_bytes < length)
    {
      const ssize_t result = read(fd, bytes + read_bytes, length - read_bytes);
      if (result == 0)
      {
        if (read_bytes == 0)
        {
          return false;
        }

        throw std::runtime_error("request channel closed unexpectedly");
      }

      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        throw std::runtime_error("request channel read failed");
      }

      read_bytes += static_cast<std::size_t>(result);
    }

    return true;
  }

  bool wait_for_fd_readable(int fd, int timeout_milliseconds)
  {
    short events = POLLIN | POLLERR | POLLHUP;
#ifdef POLLRDHUP
    events = static_cast<short>(events | POLLRDHUP);
#endif
    pollfd descriptor{fd, events, 0};
    for (;;)
    {
      const int result = poll(&descriptor, 1, timeout_milliseconds);
      if (result > 0)
      {
        return true;
      }
      if (result == 0)
      {
        return false;
      }
      if (errno == EINTR)
      {
        continue;
      }

      throw std::runtime_error("request channel readiness poll failed");
    }
  }

  bool read_exact_or_eof_with_timeout(int fd, void *buffer, std::size_t length, int timeout_milliseconds)
  {
    auto *bytes = static_cast<std::uint8_t *>(buffer);
    std::size_t read_bytes = 0;
    while (read_bytes < length)
    {
      if (!wait_for_fd_readable(fd, timeout_milliseconds))
      {
        return false;
      }

      const ssize_t result = read(fd, bytes + read_bytes, length - read_bytes);
      if (result == 0)
      {
        if (read_bytes == 0)
        {
          return false;
        }

        throw std::runtime_error("request channel closed unexpectedly");
      }

      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }

        throw std::runtime_error("request channel read failed");
      }

      read_bytes += static_cast<std::size_t>(result);
    }

    return true;
  }

  void append_u32(std::string &buffer, std::uint32_t value)
  {
    buffer.push_back(static_cast<char>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<char>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<char>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<char>(value & 0xFF));
  }

  std::uint32_t read_u32(const std::string &buffer, std::size_t &cursor, const char *error_message)
  {
    if (buffer.size() - cursor < 4)
    {
      throw std::runtime_error(error_message);
    }

    const std::uint32_t value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[cursor])) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[cursor + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[cursor + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[cursor + 3]));
    cursor += 4;
    return value;
  }

  void append_string(std::string &buffer, const std::string &value)
  {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
      throw std::runtime_error("request channel payload string exceeds maximum size");
    }

    append_u32(buffer, static_cast<std::uint32_t>(value.size()));
    buffer.append(value);
  }

  std::string read_string(const std::string &buffer, std::size_t &cursor, const char *error_message)
  {
    const std::uint32_t length = read_u32(buffer, cursor, error_message);
    if (buffer.size() - cursor < length)
    {
      throw std::runtime_error(error_message);
    }

    std::string value = buffer.substr(cursor, length);
    cursor += length;
    return value;
  }

  void write_frame(int fd, Vajra::ipc::FrameFamily family, const std::string &payload)
  {
    Vajra::ipc::FrameHeader header{
        Vajra::ipc::ChannelKind::request,
        family,
        Vajra::ipc::kProtocolVersion1_0,
        static_cast<std::uint32_t>(payload.size())};
    const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
        Vajra::ipc::encode_frame_header(header);
    write_all_or_throw(fd, encoded_header.data(), encoded_header.size());
    if (!payload.empty())
    {
      write_all_or_throw(fd, payload.data(), payload.size());
    }
  }

  bool read_frame(int fd, Vajra::ipc::FrameHeader &header, std::string &payload)
  {
    std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header{};
    if (!read_exact_or_eof(fd, encoded_header.data(), encoded_header.size()))
    {
      return false;
    }

    Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::none;
    Vajra::ipc::HeaderDecodeWarning warning = Vajra::ipc::HeaderDecodeWarning::none;
    const std::optional<Vajra::ipc::FrameHeader> decoded_header =
        Vajra::ipc::decode_frame_header(encoded_header, error, warning);
    if (!decoded_header.has_value())
    {
      throw std::runtime_error("request channel received an invalid frame header");
    }

    if (warning != Vajra::ipc::HeaderDecodeWarning::none)
    {
      throw std::runtime_error("request channel received an unsupported frame header");
    }

    payload.assign(decoded_header->payload_length, '\0');
    if (decoded_header->payload_length > 0 &&
        !read_exact_or_eof(fd, payload.data(), decoded_header->payload_length))
    {
      throw std::runtime_error("request channel closed before payload body");
    }

    header = *decoded_header;
    return true;
  }

  enum class TimedReadResult
  {
    ready,
    eof,
    timeout,
  };

  TimedReadResult read_frame_with_timeout(int fd, Vajra::ipc::FrameHeader &header, std::string &payload, int timeout_milliseconds)
  {
    std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header{};
    if (!read_exact_or_eof_with_timeout(fd, encoded_header.data(), encoded_header.size(), timeout_milliseconds))
    {
      std::uint8_t probe = 0;
      const ssize_t probe_result = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      if (probe_result == 0)
      {
        return TimedReadResult::eof;
      }
      if (probe_result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        return TimedReadResult::timeout;
      }
      if (probe_result < 0 && errno == EINTR)
      {
        return TimedReadResult::timeout;
      }
      return TimedReadResult::eof;
    }

    Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::none;
    Vajra::ipc::HeaderDecodeWarning warning = Vajra::ipc::HeaderDecodeWarning::none;
    const std::optional<Vajra::ipc::FrameHeader> decoded_header =
        Vajra::ipc::decode_frame_header(encoded_header, error, warning);
    if (!decoded_header.has_value())
    {
      throw std::runtime_error("request channel received an invalid frame header");
    }

    if (warning != Vajra::ipc::HeaderDecodeWarning::none)
    {
      throw std::runtime_error("request channel received an unsupported frame header");
    }

    payload.assign(decoded_header->payload_length, '\0');
    if (decoded_header->payload_length > 0 &&
        !read_exact_or_eof_with_timeout(fd, payload.data(), decoded_header->payload_length, timeout_milliseconds))
    {
      throw std::runtime_error("request channel timed out or closed before payload body");
    }

    header = *decoded_header;
    return TimedReadResult::ready;
  }

  bool client_disconnected(int client_fd)
  {
    if (client_fd < 0)
    {
      return false;
    }

    char byte = 0;
    const ssize_t result = recv(client_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (result == 0)
    {
      return true;
    }
    if (result < 0)
    {
      if (errno == EINTR)
      {
        return false;
      }

      return errno != EAGAIN && errno != EWOULDBLOCK;
    }

    return false;
  }

  std::string exception_message(VALUE exception);

  VALUE protected_exception_message(VALUE data)
  {
    auto *exception = reinterpret_cast<VALUE *>(data);
    return rb_funcall(*exception, id_exception_message, 0);
  }

  std::string ruby_string_value(VALUE value)
  {
    if (RB_TYPE_P(value, T_STRING) == 0)
    {
      throw std::runtime_error("Rack execution returned a non-string normalized value");
    }

    return std::string(RSTRING_PTR(value), static_cast<std::size_t>(RSTRING_LEN(value)));
  }

  long ruby_string_length_for(const std::string &value)
  {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()))
    {
      throw std::runtime_error("native request payload exceeds Ruby string length limit");
    }

    return static_cast<long>(value.size());
  }

  VALUE ruby_binary_string_from(const std::string &value)
  {
    VALUE ruby_string = rb_str_new(value.empty() ? "" : value.data(), ruby_string_length_for(value));
    rb_enc_associate_index(ruby_string, rb_ascii8bit_encindex());
    return ruby_string;
  }

  VALUE ruby_env_entries_from(const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    VALUE ruby_entries = rb_ary_new_capa(static_cast<long>(env_entries.size()));
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      VALUE pair = rb_ary_new_capa(2);
      VALUE key = ruby_binary_string_from(entry.key);
      VALUE value = ruby_binary_string_from(entry.value);
      rb_ary_push(pair, key);
      rb_ary_push(pair, value);
      rb_ary_push(ruby_entries, pair);
    }

    return ruby_entries;
  }

  VALUE protected_execute_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    try
    {
      VALUE callback = Qnil;
      {
        const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
        callback = rack_execution_callback;
      }

      if (NIL_P(callback))
      {
        return Qnil;
      }

      VALUE env_entries = ruby_env_entries_from(*context->env_entries);
      VALUE request_body = ruby_binary_string_from(*context->request_body);
      VALUE arguments[] = {env_entries, request_body};
      return rb_proc_call(callback, rb_ary_new_from_values(2, arguments));
    }
    catch (const std::exception &error)
    {
      rb_raise(rb_eRuntimeError, "%s", error.what());
    }
    catch (...)
    {
      rb_raise(rb_eRuntimeError, "Rack request execution failed with an unknown native error");
    }
  }

  std::string exception_message(VALUE exception)
  {
    const std::string class_name = rb_obj_classname(exception);
    int state = 0;
    VALUE message = rb_protect(protected_exception_message, reinterpret_cast<VALUE>(&exception), &state);
    if (state != 0)
    {
      rb_set_errinfo(Qnil);
      return class_name;
    }

    if (RB_TYPE_P(message, T_STRING) == 0)
    {
      return class_name;
    }

    return class_name + ": " + ruby_string_value(message);
  }

  Vajra::response::Header response_header_from_ruby(VALUE pair)
  {
    if (TYPE(pair) != T_ARRAY || RARRAY_LEN(pair) != 2)
    {
      throw std::runtime_error("Rack execution returned an invalid header entry");
    }

    VALUE name = rb_ary_entry(pair, 0);
    VALUE value = rb_ary_entry(pair, 1);
    return Vajra::response::Header{ruby_string_value(name), ruby_string_value(value)};
  }

  std::string reason_phrase_for_status(int status_code)
  {
    switch (status_code)
    {
      case 100:
        return "Continue";
      case 101:
        return "Switching Protocols";
      case 102:
        return "Processing";
      case 103:
        return "Early Hints";
      case 200:
        return "OK";
      case 201:
        return "Created";
      case 202:
        return "Accepted";
      case 203:
        return "Non-Authoritative Information";
      case 204:
        return "No Content";
      case 205:
        return "Reset Content";
      case 206:
        return "Partial Content";
      case 207:
        return "Multi-Status";
      case 208:
        return "Already Reported";
      case 226:
        return "IM Used";
      case 300:
        return "Multiple Choices";
      case 301:
        return "Moved Permanently";
      case 302:
        return "Found";
      case 303:
        return "See Other";
      case 304:
        return "Not Modified";
      case 305:
        return "Use Proxy";
      case 307:
        return "Temporary Redirect";
      case 308:
        return "Permanent Redirect";
      case 400:
        return "Bad Request";
      case 401:
        return "Unauthorized";
      case 402:
        return "Payment Required";
      case 403:
        return "Forbidden";
      case 404:
        return "Not Found";
      case 405:
        return "Method Not Allowed";
      case 406:
        return "Not Acceptable";
      case 407:
        return "Proxy Authentication Required";
      case 408:
        return "Request Timeout";
      case 409:
        return "Conflict";
      case 410:
        return "Gone";
      case 411:
        return "Length Required";
      case 412:
        return "Precondition Failed";
      case 413:
        return "Content Too Large";
      case 414:
        return "URI Too Long";
      case 415:
        return "Unsupported Media Type";
      case 416:
        return "Range Not Satisfiable";
      case 417:
        return "Expectation Failed";
      case 418:
        return "I'm a teapot";
      case 421:
        return "Misdirected Request";
      case 422:
        return "Unprocessable Entity";
      case 423:
        return "Locked";
      case 424:
        return "Failed Dependency";
      case 425:
        return "Too Early";
      case 426:
        return "Upgrade Required";
      case 428:
        return "Precondition Required";
      case 429:
        return "Too Many Requests";
      case 431:
        return "Request Header Fields Too Large";
      case 451:
        return "Unavailable For Legal Reasons";
      case 500:
        return "Internal Server Error";
      case 501:
        return "Not Implemented";
      case 502:
        return "Bad Gateway";
      case 503:
        return "Service Unavailable";
      case 504:
        return "Gateway Timeout";
      case 505:
        return "HTTP Version Not Supported";
      case 506:
        return "Variant Also Negotiates";
      case 507:
        return "Insufficient Storage";
      case 508:
        return "Loop Detected";
      case 510:
        return "Not Extended";
      case 511:
        return "Network Authentication Required";
      default:
        return "Status";
    }
  }

  int status_code_from_ruby(VALUE status)
  {
    if (RB_INTEGER_TYPE_P(status) == 0)
    {
      throw std::runtime_error("Rack execution returned a non-integer HTTP status code");
    }

    if (RB_FIXNUM_P(status) == 0)
    {
      throw std::runtime_error("Rack execution returned an unrepresentable HTTP status code");
    }

    const long status_code = FIX2LONG(status);
    if (status_code < 100 || status_code > 599)
    {
      throw std::runtime_error("Rack execution returned an out-of-range HTTP status code");
    }

    return static_cast<int>(status_code);
  }

  Vajra::response::Response response_from_ruby(VALUE value)
  {
    if (TYPE(value) != T_ARRAY || RARRAY_LEN(value) != 3)
    {
      throw std::runtime_error("Rack execution returned an invalid normalized response");
    }

    VALUE status = rb_ary_entry(value, 0);
    VALUE headers = rb_ary_entry(value, 1);
    VALUE body = rb_ary_entry(value, 2);

    if (TYPE(headers) != T_ARRAY)
    {
      throw std::runtime_error("Rack execution returned invalid normalized headers");
    }

    std::vector<Vajra::response::Header> response_headers;
    response_headers.reserve(static_cast<std::size_t>(RARRAY_LEN(headers)));
    for (long index = 0; index < RARRAY_LEN(headers); ++index)
    {
      response_headers.push_back(response_header_from_ruby(rb_ary_entry(headers, index)));
    }

    const int status_code = status_code_from_ruby(status);

    return Vajra::response::Response{
        Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
        std::move(response_headers),
        ruby_string_value(body),
        Vajra::response::ConnectionBehavior::close};
  }

  VALUE protected_normalize_rack_response(VALUE data)
  {
    auto *context = reinterpret_cast<ResponseNormalizationContext *>(data);
    try
    {
      context->response = response_from_ruby(context->result);
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    return Qnil;
  }

  void *execute_rack_request_with_gvl(void *data)
  {
    auto *context = static_cast<ExecutionCallContext *>(data);

    int state = 0;
    VALUE result = rb_protect(protected_execute_rack_request, reinterpret_cast<VALUE>(context), &state);
    if (state != 0)
    {
      context->error_message = exception_message(rb_errinfo());
      rb_set_errinfo(Qnil);
      return nullptr;
    }

    if (!NIL_P(result))
    {
      ResponseNormalizationContext normalization_context{result, std::nullopt, ""};
      state = 0;
      rb_protect(protected_normalize_rack_response, reinterpret_cast<VALUE>(&normalization_context), &state);
      if (state != 0)
      {
        context->error_message = exception_message(rb_errinfo());
        rb_set_errinfo(Qnil);
        return nullptr;
      }
      if (!normalization_context.error_message.empty())
      {
        context->error_message = normalization_context.error_message;
        return nullptr;
      }

      context->response = std::move(normalization_context.response);
    }

    return nullptr;
  }

  std::string encode_request_execution_input(const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    std::string payload;
    append_u32(payload, static_cast<std::uint32_t>(env_entries.size()));
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      append_string(payload, entry.key);
      append_string(payload, entry.value);
    }

    return payload;
  }

  std::vector<Vajra::request::RackEnvEntry> decode_request_execution_input(const std::string &payload)
  {
    std::size_t cursor = 0;
    const std::uint32_t entry_count = read_u32(payload, cursor, "request execution input payload is truncated");
    std::vector<Vajra::request::RackEnvEntry> env_entries;
    env_entries.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index)
    {
      env_entries.push_back(Vajra::request::RackEnvEntry{
          read_string(payload, cursor, "request execution input key is truncated"),
          read_string(payload, cursor, "request execution input value is truncated")});
    }

    if (cursor != payload.size())
    {
      throw std::runtime_error("request execution input payload contains trailing bytes");
    }

    return env_entries;
  }

  std::string encode_request_body_chunk(RequestBodyEvent event, const std::string &chunk)
  {
    std::string payload;
    payload.push_back(static_cast<char>(event));
    if (event == RequestBodyEvent::chunk)
    {
      payload.append(chunk);
    }
    return payload;
  }

  RequestBodyEvent decode_request_body_event(const std::string &payload, std::string &chunk)
  {
    if (payload.empty())
    {
      throw std::runtime_error("request body continuation payload is empty");
    }

    const auto event = static_cast<RequestBodyEvent>(static_cast<unsigned char>(payload[0]));
    switch (event)
    {
      case RequestBodyEvent::chunk:
        chunk.assign(payload.data() + 1, payload.size() - 1);
        return event;
      case RequestBodyEvent::complete:
        chunk.clear();
        return event;
      case RequestBodyEvent::cancel:
        chunk.clear();
        return event;
    }

    throw std::runtime_error("request body continuation payload has an unknown event");
  }

  std::string encode_response_metadata(const std::optional<Vajra::response::Response> &response)
  {
    std::string payload;
    if (!response)
    {
      payload.push_back(static_cast<char>(ResponseMetadataKind::no_response));
      return payload;
    }

    payload.push_back(static_cast<char>(ResponseMetadataKind::response));
    append_u32(payload, static_cast<std::uint32_t>(response->status.code));
    append_string(payload, response->status.reason_phrase);
    append_u32(payload, static_cast<std::uint32_t>(response->headers.size()));
    for (const Vajra::response::Header &header : response->headers)
    {
      append_string(payload, header.name);
      append_string(payload, header.value);
    }
    return payload;
  }

  std::string encode_response_error(ResponseMetadataKind kind, const std::string &message)
  {
    std::string payload;
    payload.push_back(static_cast<char>(kind));
    append_string(payload, message);
    return payload;
  }

  DecodedResponseMetadata decode_response_metadata(const std::string &payload)
  {
    if (payload.empty())
    {
      throw std::runtime_error("response metadata payload is empty");
    }

    std::size_t cursor = 1;
    const auto kind = static_cast<ResponseMetadataKind>(static_cast<unsigned char>(payload[0]));
    switch (kind)
    {
      case ResponseMetadataKind::no_response:
        if (cursor != payload.size())
        {
          throw std::runtime_error("no-response metadata payload contains trailing bytes");
        }
        return DecodedResponseMetadata{kind, std::nullopt, ""};
      case ResponseMetadataKind::response:
      {
        const int status_code = static_cast<int>(read_u32(payload, cursor, "response metadata status is truncated"));
        const std::string reason_phrase = read_string(payload, cursor, "response metadata reason phrase is truncated");
        const std::uint32_t header_count = read_u32(payload, cursor, "response metadata header count is truncated");
        std::vector<Vajra::response::Header> headers;
        headers.reserve(header_count);
        for (std::uint32_t index = 0; index < header_count; ++index)
        {
          headers.push_back(Vajra::response::Header{
              read_string(payload, cursor, "response header name is truncated"),
              read_string(payload, cursor, "response header value is truncated")});
        }

        if (cursor != payload.size())
        {
          throw std::runtime_error("response metadata payload contains trailing bytes");
        }

        return DecodedResponseMetadata{
            kind,
            Vajra::response::Response{
                Vajra::response::Status{status_code, reason_phrase},
                std::move(headers),
                "",
                Vajra::response::ConnectionBehavior::close},
            ""};
      }
      case ResponseMetadataKind::head_error:
      case ResponseMetadataKind::execution_error:
      {
        const std::string error_message = read_string(payload, cursor, "response error payload is truncated");
        if (cursor != payload.size())
        {
          throw std::runtime_error("response error payload contains trailing bytes");
        }

        return DecodedResponseMetadata{kind, std::nullopt, error_message};
      }
    }

    throw std::runtime_error("response metadata payload has an unknown kind");
  }

  std::string encode_response_body_event(ResponseBodyEvent event, const std::string &chunk)
  {
    std::string payload;
    payload.push_back(static_cast<char>(event));
    if (event == ResponseBodyEvent::chunk)
    {
      payload.append(chunk);
    }
    return payload;
  }

  ResponseBodyEvent decode_response_body_event(const std::string &payload, std::string &chunk)
  {
    if (payload.empty())
    {
      throw std::runtime_error("response body continuation payload is empty");
    }

    const auto event = static_cast<ResponseBodyEvent>(static_cast<unsigned char>(payload[0]));
    switch (event)
    {
      case ResponseBodyEvent::chunk:
        chunk.assign(payload.data() + 1, payload.size() - 1);
        return event;
      case ResponseBodyEvent::complete:
        chunk.clear();
        return event;
    }

    throw std::runtime_error("response body continuation payload has an unknown event");
  }

  class BufferedRackExecutionSession final : public Vajra::rack::RackExecutionSession
  {
  public:
    BufferedRackExecutionSession(
        const Vajra::rack::RackExecutionTransport &transport,
        std::vector<Vajra::request::RackEnvEntry> env_entries)
        : transport_(transport),
          env_entries_(std::move(env_entries))
    {
    }

    void append_request_body_chunk(const std::string &chunk) override
    {
      request_body_.append(chunk);
    }

    std::optional<Vajra::response::Response> finish() override
    {
      return transport_.execute(env_entries_, request_body_);
    }

  private:
    const Vajra::rack::RackExecutionTransport &transport_;
    std::vector<Vajra::request::RackEnvEntry> env_entries_;
    std::string request_body_;
  };

  class SameProcessRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        const std::string &request_body) const override
    {
      if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }

      ExecutionCallContext context{&env_entries, &request_body, std::nullopt, ""};
      rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);

      if (!context.error_message.empty())
      {
        throw std::runtime_error("Rack request execution failed: " + context.error_message);
      }

      return context.response;
    }
  };

  class CurrentThreadRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        const std::string &request_body) const override
    {
      if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }

      ExecutionCallContext context{&env_entries, &request_body, std::nullopt, ""};
      execute_rack_request_with_gvl(&context);

      if (!context.error_message.empty())
      {
        throw std::runtime_error("Rack request execution failed: " + context.error_message);
      }

      return context.response;
    }
  };

  class WorkerProcessRackExecutionSession final : public Vajra::rack::RackExecutionSession
  {
  public:
    WorkerProcessRackExecutionSession(
        int channel_fd,
        std::mutex &channel_mutex,
        std::size_t worker_timeout_seconds,
        std::function<void()> timeout_handler)
        : channel_fd_(channel_fd),
          channel_lock_(channel_mutex),
          worker_timeout_milliseconds_(clamp_poll_timeout_milliseconds(worker_timeout_seconds)),
          timeout_handler_(std::move(timeout_handler))
    {
    }

    ~WorkerProcessRackExecutionSession() override
    {
      if (!finished_)
      {
        try
        {
          write_frame(
              channel_fd_,
              Vajra::ipc::FrameFamily::request_body_continuation,
              encode_request_body_chunk(RequestBodyEvent::cancel, ""));
        }
        catch (...)
        {
        }
      }
    }

    void send_request_start(const std::vector<Vajra::request::RackEnvEntry> &env_entries)
    {
      write_frame(channel_fd_, Vajra::ipc::FrameFamily::request_execution_input, encode_request_execution_input(env_entries));
    }

    void append_request_body_chunk(const std::string &chunk) override
    {
      std::size_t cursor = 0;
      while (cursor < chunk.size())
      {
        const std::size_t length = std::min(kInlineBodyChunkBytes, chunk.size() - cursor);
        write_frame(
            channel_fd_,
            Vajra::ipc::FrameFamily::request_body_continuation,
            encode_request_body_chunk(RequestBodyEvent::chunk, chunk.substr(cursor, length)));
        cursor += length;
      }
    }

    std::optional<Vajra::response::Response> finish() override
    {
      write_frame(
          channel_fd_,
          Vajra::ipc::FrameFamily::request_body_continuation,
          encode_request_body_chunk(RequestBodyEvent::complete, ""));
      finished_ = true;

      Vajra::ipc::FrameHeader header{};
      std::string payload;
      const TimedReadResult metadata_result =
          read_frame_with_timeout(channel_fd_, header, payload, worker_timeout_milliseconds_);
      if (metadata_result != TimedReadResult::ready)
      {
        timeout_handler_();
        throw std::runtime_error(
            metadata_result == TimedReadResult::timeout
                ? "worker request channel timed out before response metadata"
                : "worker request channel closed before response metadata");
      }
      if (header.family != Vajra::ipc::FrameFamily::response_metadata_result)
      {
        throw std::runtime_error("worker request channel returned an unexpected response frame");
      }

      DecodedResponseMetadata metadata = decode_response_metadata(payload);
      switch (metadata.kind)
      {
        case ResponseMetadataKind::no_response:
          return std::nullopt;
        case ResponseMetadataKind::head_error:
          throw Vajra::request::bad_request_error(metadata.error_message);
        case ResponseMetadataKind::execution_error:
          throw std::runtime_error(metadata.error_message);
        case ResponseMetadataKind::response:
          break;
      }

      std::optional<Vajra::response::Response> response = std::move(metadata.response);
      for (;;)
      {
        const TimedReadResult body_result =
            read_frame_with_timeout(channel_fd_, header, payload, worker_timeout_milliseconds_);
        if (body_result != TimedReadResult::ready)
        {
          timeout_handler_();
          throw std::runtime_error(
              body_result == TimedReadResult::timeout
                  ? "worker request channel timed out before response body completion"
                  : "worker request channel closed before response body completion");
        }
        if (header.family != Vajra::ipc::FrameFamily::response_body_continuation)
        {
          throw std::runtime_error("worker request channel returned an unexpected response body frame");
        }

        std::string chunk;
        const ResponseBodyEvent event = decode_response_body_event(payload, chunk);
        if (event == ResponseBodyEvent::complete)
        {
          return response;
        }

        response->body.append(chunk);
      }
    }

  private:
    int channel_fd_;
    std::unique_lock<std::mutex> channel_lock_;
    int worker_timeout_milliseconds_;
    std::function<void()> timeout_handler_;
    bool finished_ = false;
  };

  class WorkerProcessRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    WorkerProcessRackExecutionTransport(
        int channel_fd,
        std::size_t worker_timeout_seconds,
        std::function<void()> timeout_handler)
        : channel_fd_(channel_fd),
          worker_timeout_seconds_(worker_timeout_seconds),
          timeout_handler_(std::move(timeout_handler))
    {
    }

    std::unique_ptr<Vajra::rack::RackExecutionSession> start(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        int) const override
    {
      auto session = std::make_unique<WorkerProcessRackExecutionSession>(
          channel_fd_,
          channel_mutex_,
          worker_timeout_seconds_,
          timeout_handler_);
      session->send_request_start(env_entries);
      return session;
    }

    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &,
        const std::string &) const override
    {
      throw std::logic_error("worker request transport must use streaming start()");
    }

  private:
    int channel_fd_;
    std::size_t worker_timeout_seconds_;
    std::function<void()> timeout_handler_;
    mutable std::mutex channel_mutex_;
  };

  void log_scheduler_debug_event(const std::string &message, bool debug_logging)
  {
    if (!debug_logging)
    {
      return;
    }

    std::cout << "[Vajra][scheduler] " << message << std::endl;
  }

  struct PendingRequest;
  class GlobalQueuedWorkerProcessRackExecutionTransport;

  class QueuedWorkerProcessRackExecutionSession final : public Vajra::rack::RackExecutionSession
  {
  public:
    QueuedWorkerProcessRackExecutionSession(
        const GlobalQueuedWorkerProcessRackExecutionTransport &transport,
        std::shared_ptr<PendingRequest> pending_request);
    ~QueuedWorkerProcessRackExecutionSession() override;

    void append_request_body_chunk(const std::string &chunk) override;
    std::optional<Vajra::response::Response> finish() override;

  private:
    void cancel() noexcept;
    void ensure_live_session_started();
    void ensure_request_still_live() const;

    const GlobalQueuedWorkerProcessRackExecutionTransport &transport_;
    std::shared_ptr<PendingRequest> pending_request_;
    std::string buffered_request_body_;
    std::unique_ptr<Vajra::rack::RackExecutionSession> live_session_;
    bool finished_ = false;
    bool canceled_ = false;
  };

  struct PendingRequest
  {
    std::uint64_t request_id = 0;
    int client_fd = -1;
    std::vector<Vajra::request::RackEnvEntry> env_entries;
    std::chrono::steady_clock::time_point deadline;
    std::atomic_bool assigned = false;
    std::atomic_bool released = false;
    std::atomic_bool canceled = false;
    std::atomic_bool timed_out = false;
    std::atomic_bool client_gone = false;
    std::size_t worker_index = 0;
    std::size_t channel_index = 0;
  };

  class GlobalQueuedWorkerProcessRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    struct WorkerChannel
    {
      WorkerChannel(int fd, std::size_t worker_timeout_seconds, const std::function<void()> &timeout_handler)
          : transport(std::make_shared<WorkerProcessRackExecutionTransport>(
                fd,
                worker_timeout_seconds,
                timeout_handler))
      {
      }

      std::shared_ptr<WorkerProcessRackExecutionTransport> transport;
      bool busy = false;
    };

    struct WorkerSlot
    {
      WorkerSlot(std::vector<WorkerChannel> worker_channels, int worker_pid, std::size_t min_channel_count)
          : channels(std::move(worker_channels)),
            pid(worker_pid),
            min_channels(std::min(min_channel_count, channels.size())),
            active_channels(std::min(min_channel_count, channels.size()))
      {
      }

      std::vector<WorkerChannel> channels;
      int pid;
      std::size_t min_channels;
      std::size_t active_channels;
      bool alive = true;
    };

    GlobalQueuedWorkerProcessRackExecutionTransport(
        const std::vector<std::vector<int>> &worker_channel_fds,
        const std::vector<int> &worker_pids,
        std::size_t min_threads,
        std::size_t queue_capacity,
        std::size_t request_timeout_seconds,
        std::size_t worker_timeout_seconds,
        bool debug_logging)
        : queue_capacity_(queue_capacity),
          request_timeout_(std::chrono::seconds(request_timeout_seconds)),
          debug_logging_(debug_logging)
    {
      if (worker_channel_fds.empty())
      {
        throw std::logic_error("worker request transport requires at least one channel");
      }
      if (worker_channel_fds.size() != worker_pids.size())
      {
        throw std::logic_error("worker request transport requires one pid per worker");
      }

      slots_.reserve(worker_channel_fds.size());
      for (std::size_t worker_index = 0; worker_index < worker_channel_fds.size(); ++worker_index)
      {
        const std::vector<int> &channel_fds = worker_channel_fds[worker_index];
        if (channel_fds.empty())
        {
          throw std::logic_error("worker request transport requires at least one channel per worker");
        }

        std::vector<WorkerChannel> worker_channels;
        worker_channels.reserve(channel_fds.size());
        for (int channel_fd : channel_fds)
        {
          worker_channels.emplace_back(
              channel_fd,
              worker_timeout_seconds,
              [this, worker_index]() { mark_worker_timed_out(worker_index); });
        }

        slots_.push_back(std::make_shared<WorkerSlot>(
            std::move(worker_channels),
            worker_pids[worker_index],
            min_threads));
      }
    }

    std::unique_ptr<Vajra::rack::RackExecutionSession> start(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        int client_fd) const override
    {
      const std::shared_ptr<PendingRequest> pending_request = admit_request(env_entries, client_fd);
      log_scheduler_debug_event(
          "event=request_admitted policy=least_loaded request_id=" + std::to_string(pending_request->request_id) +
              " queue_depth=" + std::to_string(queue_depth()) +
              " queue_capacity=" + std::to_string(queue_capacity_),
          debug_logging_);

      return std::make_unique<QueuedWorkerProcessRackExecutionSession>(*this, pending_request);
    }

    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        const std::string &request_body) const override
    {
      std::unique_ptr<Vajra::rack::RackExecutionSession> session = start(env_entries, -1);
      session->append_request_body_chunk(request_body);
      return session->finish();
    }

    std::shared_ptr<WorkerSlot> slot_for(std::size_t worker_index) const
    {
      return slots_.at(worker_index);
    }

    std::pair<std::size_t, std::size_t> await_assignment(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      std::unique_lock<std::mutex> lock(scheduler_mutex_);
      while (!pending_request->assigned.load() &&
             !pending_request->timed_out.load() &&
             !pending_request->client_gone.load() &&
             !pending_request->canceled.load())
      {
        prune_queue_locked();
        if (pending_request->assigned.load() ||
            pending_request->timed_out.load() ||
            pending_request->client_gone.load() ||
            pending_request->canceled.load())
        {
          break;
        }

        if (!pending_requests_.empty() && pending_requests_.front() == pending_request)
        {
          const std::optional<std::pair<std::size_t, std::size_t>> assignment = least_busy_channel_locked();
          if (assignment.has_value())
          {
            const std::shared_ptr<WorkerSlot> slot = slot_for(assignment->first);
            slot->channels[assignment->second].busy = true;
            pending_request->assigned = true;
            pending_request->worker_index = assignment->first;
            pending_request->channel_index = assignment->second;
            log_scheduler_debug_event(
                "event=request_assigned policy=least_loaded request_id=" + std::to_string(pending_request->request_id) +
                    " selected_worker=" + std::to_string(assignment->first) +
                    " channel=" + std::to_string(assignment->second) +
                    " inflight=" + std::to_string(inflight_count_locked(*slot)) +
                    " queue_depth=" + std::to_string(pending_requests_.size() - 1),
                debug_logging_);
            pending_requests_.pop_front();
            break;
          }
        }

        if (pending_request->deadline <= std::chrono::steady_clock::now())
        {
          pending_request->timed_out = true;
          log_scheduler_debug_event(
              "event=request_wait_timeout request_id=" + std::to_string(pending_request->request_id),
              debug_logging_);
          erase_pending_request_locked(pending_request);
          break;
        }

        scheduler_condition_.wait_until(lock, pending_request->deadline);
      }

      if (pending_request->timed_out.load())
      {
        throw Vajra::request::RequestTimeoutError("request timed out while waiting in the global queue");
      }
      if (pending_request->client_gone.load() || pending_request->canceled.load())
      {
        throw Vajra::request::RequestTimeoutError("request left the global queue before execution started");
      }

      return {pending_request->worker_index, pending_request->channel_index};
    }

    void release_channel(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      bool expected = false;
      if (!pending_request->released.compare_exchange_strong(expected, true))
      {
        return;
      }

      {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        const std::shared_ptr<WorkerSlot> slot = slot_for(pending_request->worker_index);
        slot->channels.at(pending_request->channel_index).busy = false;
        pending_request->assigned = false;
        while (slot->active_channels > slot->min_channels &&
               !slot->channels[slot->active_channels - 1].busy &&
               pending_requests_.empty())
        {
          --slot->active_channels;
        }
      }
      scheduler_condition_.notify_all();
    }

    void cancel_request(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      bool release_assigned_channel = false;
      {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        if (pending_request->assigned.load())
        {
          release_assigned_channel = true;
        }
        else
        {
          pending_request->canceled = true;
          erase_pending_request_locked(pending_request);
        }
      }
      if (release_assigned_channel)
      {
        release_channel(pending_request);
        return;
      }
      scheduler_condition_.notify_all();
    }

  private:
    static std::optional<std::size_t> first_available_channel_index_locked(const WorkerSlot &slot)
    {
      for (std::size_t index = 0; index < slot.active_channels; ++index)
      {
        if (!slot.channels[index].busy)
        {
          return index;
        }
      }

      return std::nullopt;
    }

    static std::size_t inflight_count_locked(const WorkerSlot &slot)
    {
      return static_cast<std::size_t>(std::count_if(
          slot.channels.begin(),
          slot.channels.end(),
          [](const WorkerChannel &channel) { return channel.busy; }));
    }

    std::shared_ptr<PendingRequest> admit_request(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        int client_fd) const
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex_);
      prune_queue_locked();
      if (pending_requests_.size() >= queue_capacity_)
      {
        log_scheduler_debug_event(
            "event=queue_capacity_reached policy=least_loaded queue_capacity=" + std::to_string(queue_capacity_),
            debug_logging_);
        throw Vajra::request::QueueCapacityError(
            "request admission rejected because the global queue reached its hard capacity");
      }

      auto pending_request = std::make_shared<PendingRequest>();
      pending_request->request_id = next_request_id_++;
      pending_request->client_fd = client_fd;
      pending_request->env_entries = env_entries;
      pending_request->deadline = std::chrono::steady_clock::now() + request_timeout_;
      pending_requests_.push_back(pending_request);
      scheduler_condition_.notify_all();
      return pending_request;
    }

    void mark_worker_timed_out(std::size_t worker_index) const
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex_);
      const std::shared_ptr<WorkerSlot> slot = slot_for(worker_index);
      if (!slot->alive)
      {
        return;
      }

      slot->alive = false;
      log_scheduler_debug_event(
          "event=worker_timeout worker_index=" + std::to_string(worker_index) +
              " worker_pid=" + std::to_string(slot->pid),
          debug_logging_);
      kill(slot->pid, SIGKILL);
      scheduler_condition_.notify_all();
    }

    std::optional<std::pair<std::size_t, std::size_t>> least_busy_channel_locked() const
    {
      std::optional<std::pair<std::size_t, std::size_t>> best_assignment;
      std::size_t best_worker_load = std::numeric_limits<std::size_t>::max();
      const std::size_t worker_count = slots_.size();

      for (std::size_t offset = 0; offset < worker_count; ++offset)
      {
        const std::size_t worker_index = (next_preferred_worker_ + offset) % worker_count;
        const std::shared_ptr<WorkerSlot> slot = slots_[worker_index];
        if (!slot->alive)
        {
          continue;
        }

        std::optional<std::size_t> channel_index = first_available_channel_index_locked(*slot);
        if (!channel_index.has_value() && slot->active_channels < slot->channels.size())
        {
          ++slot->active_channels;
          channel_index = first_available_channel_index_locked(*slot);
        }
        if (!channel_index.has_value())
        {
          continue;
        }

        const std::size_t worker_load = inflight_count_locked(*slot);
        if (!best_assignment.has_value() || worker_load < best_worker_load)
        {
          best_assignment = std::make_pair(worker_index, *channel_index);
          best_worker_load = worker_load;
        }
      }

      if (best_assignment.has_value())
      {
        next_preferred_worker_ = (best_assignment->first + 1) % worker_count;
      }

      return best_assignment;
    }

    void prune_queue_locked() const
    {
      const auto now = std::chrono::steady_clock::now();
      for (const auto &pending_request : pending_requests_)
      {
        if (pending_request->assigned.load() ||
            pending_request->canceled.load() ||
            pending_request->timed_out.load() ||
            pending_request->client_gone.load())
        {
          continue;
        }
        if (pending_request->deadline <= now)
        {
          pending_request->timed_out = true;
          log_scheduler_debug_event(
              "event=request_wait_timeout request_id=" + std::to_string(pending_request->request_id),
              debug_logging_);
          continue;
        }
        if (client_disconnected(pending_request->client_fd))
        {
          pending_request->client_gone = true;
          log_scheduler_debug_event(
              "event=request_client_disconnected request_id=" + std::to_string(pending_request->request_id),
              debug_logging_);
        }
      }

      pending_requests_.erase(
          std::remove_if(
              pending_requests_.begin(),
              pending_requests_.end(),
              [](const std::shared_ptr<PendingRequest> &pending_request) {
                return pending_request->canceled.load() ||
                       pending_request->timed_out.load() ||
                       pending_request->client_gone.load();
              }),
          pending_requests_.end());
    }

    void erase_pending_request_locked(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      pending_requests_.erase(
          std::remove(pending_requests_.begin(), pending_requests_.end(), pending_request),
          pending_requests_.end());
    }

    std::size_t queue_depth() const
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex_);
      return pending_requests_.size();
    }

    std::vector<std::shared_ptr<WorkerSlot>> slots_;
    std::size_t queue_capacity_;
    std::chrono::steady_clock::duration request_timeout_;
    bool debug_logging_;
    mutable std::mutex scheduler_mutex_;
    mutable std::condition_variable scheduler_condition_;
    mutable std::deque<std::shared_ptr<PendingRequest>> pending_requests_;
    mutable std::uint64_t next_request_id_ = 0;
    mutable std::size_t next_preferred_worker_ = 0;

    friend class QueuedWorkerProcessRackExecutionSession;
  };

  QueuedWorkerProcessRackExecutionSession::QueuedWorkerProcessRackExecutionSession(
      const GlobalQueuedWorkerProcessRackExecutionTransport &transport,
      std::shared_ptr<PendingRequest> pending_request)
      : transport_(transport),
        pending_request_(std::move(pending_request))
  {
  }

  QueuedWorkerProcessRackExecutionSession::~QueuedWorkerProcessRackExecutionSession()
  {
    cancel();
  }

  void QueuedWorkerProcessRackExecutionSession::append_request_body_chunk(const std::string &chunk)
  {
    ensure_request_still_live();
    if (live_session_)
    {
      live_session_->append_request_body_chunk(chunk);
      return;
    }

    buffered_request_body_.append(chunk);
  }

  std::optional<Vajra::response::Response> QueuedWorkerProcessRackExecutionSession::finish()
  {
    ensure_live_session_started();
    finished_ = true;

    try
    {
      std::optional<Vajra::response::Response> response = live_session_->finish();
      live_session_.reset();
      transport_.release_channel(pending_request_);
      return response;
    }
    catch (...)
    {
      live_session_.reset();
      transport_.release_channel(pending_request_);
      throw;
    }
  }

  void QueuedWorkerProcessRackExecutionSession::cancel() noexcept
  {
    if (finished_ || canceled_)
    {
      return;
    }

    canceled_ = true;
    if (live_session_)
    {
      live_session_.reset();
      transport_.release_channel(pending_request_);
      return;
    }

    transport_.cancel_request(pending_request_);
  }

  void QueuedWorkerProcessRackExecutionSession::ensure_live_session_started()
  {
    if (live_session_)
    {
      return;
    }

    const auto [worker_index, channel_index] = transport_.await_assignment(pending_request_);

    try
    {
      live_session_ =
          transport_.slot_for(worker_index)->channels.at(channel_index).transport->start(
              pending_request_->env_entries,
              pending_request_->client_fd);
      if (!buffered_request_body_.empty())
      {
        live_session_->append_request_body_chunk(buffered_request_body_);
        buffered_request_body_.clear();
      }
    }
    catch (...)
    {
      if (!live_session_)
      {
        transport_.release_channel(pending_request_);
      }
      throw;
    }
  }

  void QueuedWorkerProcessRackExecutionSession::ensure_request_still_live() const
  {
    if (pending_request_->timed_out.load())
    {
      throw Vajra::request::RequestTimeoutError("request timed out while waiting in the global queue");
    }
    if (pending_request_->client_gone.load())
    {
      throw Vajra::request::RequestTimeoutError("client disconnected before request execution started");
    }
  }

  struct ChannelRequestReadResult
  {
    int channel_fd;
    bool eof = false;
    bool request_canceled = false;
    std::vector<Vajra::request::RackEnvEntry> env_entries;
    std::string request_body;
    std::string error_message;
  };

  void *read_worker_request_from_channel_without_gvl(void *data)
  {
    auto *result = static_cast<ChannelRequestReadResult *>(data);
    try
    {
      Vajra::ipc::FrameHeader header{};
      std::string payload;
      if (!read_frame(result->channel_fd, header, payload))
      {
        result->eof = true;
        return nullptr;
      }

      if (header.family != Vajra::ipc::FrameFamily::request_execution_input)
      {
        throw std::runtime_error("worker request loop expected request execution input");
      }

      result->env_entries = decode_request_execution_input(payload);
      result->request_body.clear();
      result->request_canceled = false;

      for (;;)
      {
        if (!read_frame(result->channel_fd, header, payload))
        {
          throw std::runtime_error("worker request loop closed before request body completion");
        }

        if (header.family != Vajra::ipc::FrameFamily::request_body_continuation)
        {
          throw std::runtime_error("worker request loop expected request body continuation");
        }

        std::string chunk;
        const RequestBodyEvent event = decode_request_body_event(payload, chunk);
        if (event == RequestBodyEvent::cancel)
        {
          result->request_canceled = true;
          return nullptr;
        }
        if (event == RequestBodyEvent::complete)
        {
          return nullptr;
        }

        result->request_body.append(chunk);
      }
    }
    catch (const std::exception &error)
    {
      result->error_message = error.what();
    }

    return nullptr;
  }

  struct WorkerChannelThreadContext
  {
    int channel_fd = -1;
    std::atomic_bool failed{false};
    mutable std::mutex error_mutex;
    std::string error_message;
  };

  std::optional<std::string> take_worker_channel_error(const WorkerChannelThreadContext &context)
  {
    if (!context.failed.load(std::memory_order_acquire))
    {
      return std::nullopt;
    }

    const std::lock_guard<std::mutex> lock(context.error_mutex);
    return context.error_message;
  }

  VALUE run_worker_request_channel_thread(void *data)
  {
    auto *context = static_cast<WorkerChannelThreadContext *>(data);
    CurrentThreadRackExecutionTransport transport;

    for (;;)
    {
      ChannelRequestReadResult read_result{context->channel_fd, false, false, {}, "", ""};
      rb_thread_call_without_gvl(
          read_worker_request_from_channel_without_gvl,
          &read_result,
          RUBY_UBF_IO,
          nullptr);

      if (!read_result.error_message.empty())
      {
        {
          const std::lock_guard<std::mutex> lock(context->error_mutex);
          context->error_message = read_result.error_message;
        }
        context->failed.store(true, std::memory_order_release);
        return Qnil;
      }
      if (read_result.eof)
      {
        return Qnil;
      }
      if (read_result.request_canceled)
      {
        continue;
      }

      try
      {
        const std::optional<Vajra::response::Response> response =
            transport.execute(read_result.env_entries, read_result.request_body);
        write_frame(
            context->channel_fd,
            Vajra::ipc::FrameFamily::response_metadata_result,
            encode_response_metadata(response));
        if (!response)
        {
          continue;
        }

        std::size_t cursor = 0;
        while (cursor < response->body.size())
        {
          const std::size_t length = std::min(kInlineBodyChunkBytes, response->body.size() - cursor);
          write_frame(
              context->channel_fd,
              Vajra::ipc::FrameFamily::response_body_continuation,
              encode_response_body_event(ResponseBodyEvent::chunk, response->body.substr(cursor, length)));
          cursor += length;
        }
        write_frame(
            context->channel_fd,
            Vajra::ipc::FrameFamily::response_body_continuation,
            encode_response_body_event(ResponseBodyEvent::complete, ""));
      }
      catch (const Vajra::request::HeadError &error)
      {
        write_frame(
            context->channel_fd,
            Vajra::ipc::FrameFamily::response_metadata_result,
            encode_response_error(ResponseMetadataKind::head_error, error.what()));
      }
      catch (const std::exception &error)
      {
        write_frame(
            context->channel_fd,
            Vajra::ipc::FrameFamily::response_metadata_result,
            encode_response_error(ResponseMetadataKind::execution_error, error.what()));
      }
    }
  }

  class RequestExecutionBridgeSession final : public Vajra::request::RequestExecutionSession
  {
  public:
    explicit RequestExecutionBridgeSession(std::unique_ptr<Vajra::rack::RackExecutionSession> session)
        : session_(std::move(session))
    {
    }

    void append_request_body_chunk(const std::string &chunk) override
    {
      session_->append_request_body_chunk(chunk);
    }

    std::optional<Vajra::response::Response> finish() override
    {
      return session_->finish();
    }

  private:
    std::unique_ptr<Vajra::rack::RackExecutionSession> session_;
  };
}

void Vajra::rack::initialize_rack_execution_bridge()
{
  rb_global_variable(&rack_execution_callback);
  id_exception_message = rb_intern("message");
}

void Vajra::rack::set_rack_execution_callback(VALUE callback)
{
  {
    const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
    rack_execution_callback = callback;
  }
  rack_execution_callback_installed_flag.store(!NIL_P(callback), std::memory_order_release);
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::request_channel_transport(int channel_fd)
{
  return std::make_shared<WorkerProcessRackExecutionTransport>(
      channel_fd,
      60,
      []() {});
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::request_channel_transport(
    const std::vector<std::vector<int>> &worker_channel_fds,
    const std::vector<int> &worker_pids,
    std::size_t min_threads,
    std::size_t queue_capacity,
    std::size_t request_timeout_seconds,
    std::size_t worker_timeout_seconds,
    bool debug_logging)
{
  return std::make_shared<GlobalQueuedWorkerProcessRackExecutionTransport>(
      worker_channel_fds,
      worker_pids,
      min_threads,
      queue_capacity,
      request_timeout_seconds,
      worker_timeout_seconds,
      debug_logging);
}

std::unique_ptr<Vajra::rack::RackExecutionSession> Vajra::rack::RackExecutionTransport::start(
    const std::vector<request::RackEnvEntry> &env_entries,
    int) const
{
  return std::make_unique<BufferedRackExecutionSession>(*this, env_entries);
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor()
    : transport_(std::make_shared<SameProcessRackExecutionTransport>())
{
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor(
    std::shared_ptr<const RackExecutionTransport> transport)
    : transport_(std::move(transport))
{
  if (!transport_)
  {
    throw std::invalid_argument("rack execution transport must not be null");
  }
}

std::unique_ptr<Vajra::request::RequestExecutionSession> Vajra::rack::RackRequestExecutor::start(
    const request::RequestContext &request_context) const
{
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return std::make_unique<RequestExecutionBridgeSession>(transport_->start(env_entries, request_context.client_fd));
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    const request::RequestContext &request_context) const
{
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return transport_->execute(env_entries, request_context.request_body);
}

void Vajra::rack::run_worker_request_execution_loop(
    const std::vector<int> &channel_fds,
    std::size_t max_threads)
{
  if (channel_fds.size() != max_threads)
  {
    throw std::runtime_error("worker request channel count must match max_threads");
  }

  std::vector<std::unique_ptr<WorkerChannelThreadContext>> contexts;
  contexts.reserve(channel_fds.size());
  std::vector<VALUE> threads;
  threads.reserve(channel_fds.size());

  for (int channel_fd : channel_fds)
  {
    auto context = std::make_unique<WorkerChannelThreadContext>();
    context->channel_fd = channel_fd;
    contexts.push_back(std::move(context));
    threads.push_back(rb_thread_create(run_worker_request_channel_thread, contexts.back().get()));
  }

  ID id_join = rb_intern("join");
  VALUE join_timeout = rb_float_new(0.01);
  std::vector<bool> joined(threads.size(), false);
  std::size_t joined_count = 0;

  while (joined_count < threads.size())
  {
    for (std::size_t index = 0; index < threads.size(); ++index)
    {
      if (joined[index])
      {
        continue;
      }
      if (const std::optional<std::string> error_message = take_worker_channel_error(*contexts[index]))
      {
        throw std::runtime_error(*error_message);
      }

      const VALUE join_result = rb_funcall(threads[index], id_join, 1, join_timeout);
      if (NIL_P(join_result))
      {
        continue;
      }

      joined[index] = true;
      ++joined_count;
      if (const std::optional<std::string> error_message = take_worker_channel_error(*contexts[index]))
      {
        throw std::runtime_error(*error_message);
      }
    }
  }

  for (const std::unique_ptr<WorkerChannelThreadContext> &context : contexts)
  {
    if (const std::optional<std::string> error_message = take_worker_channel_error(*context))
    {
      throw std::runtime_error(*error_message);
    }
  }
}
