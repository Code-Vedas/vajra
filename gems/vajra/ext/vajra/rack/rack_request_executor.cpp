// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_request_executor.hpp"
#include "rack/worker_execution_pool.hpp"

#include "ipc/frame_header.hpp"
#include "request/http_field_utils.hpp"
#include "request/rack_env.hpp"
#include "request/request_head_error.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/runtime_state.hpp"
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
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
  std::atomic<bool> rack_execution_callback_installed_flag{false};
  std::mutex rack_execution_callback_mutex;
  VALUE rack_execution_callback = Qnil;
  std::atomic<bool> rack_execution_app_installed_flag{false};
  std::mutex rack_execution_app_mutex;
  VALUE rack_execution_app = Qnil;
  ID id_exception_message;
  ID id_call;
  ID id_close;
  ID id_each;
  ID id_new;
  ID id_to_s;
  VALUE rb_cStringIO = Qnil;
  VALUE rb_key_content_length = Qnil;
  VALUE rb_key_content_type = Qnil;
  VALUE rb_key_path_info = Qnil;
  VALUE rb_key_query_string = Qnil;
  VALUE rb_key_rack_errors = Qnil;
  VALUE rb_key_rack_input = Qnil;
  VALUE rb_key_rack_multiprocess = Qnil;
  VALUE rb_key_rack_multithread = Qnil;
  VALUE rb_key_rack_run_once = Qnil;
  VALUE rb_key_rack_url_scheme = Qnil;
  VALUE rb_key_rack_version = Qnil;
  VALUE rb_key_remote_addr = Qnil;
  VALUE rb_key_remote_port = Qnil;
  VALUE rb_key_request_method = Qnil;
  VALUE rb_key_script_name = Qnil;
  VALUE rb_key_server_name = Qnil;
  VALUE rb_key_server_port = Qnil;
  VALUE rb_key_server_protocol = Qnil;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    const std::string *request_body;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
    bool use_native_app = false;
  };

  struct HeaderCollectionContext
  {
    std::vector<Vajra::response::Header> headers;
    std::string error_message;
  };

  struct BodyCollectionContext
  {
    std::string body;
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
  constexpr auto kQueueHousekeepingInterval = std::chrono::milliseconds(500);

  struct RuntimeProfilingCounters
  {
    std::atomic<std::uint64_t> scheduler_selection_count{0};
    std::atomic<std::int64_t> scheduler_selection_nanoseconds{0};
    std::atomic<std::uint64_t> ruby_execution_count{0};
    std::atomic<std::int64_t> ruby_execution_nanoseconds{0};
    std::atomic<std::uint64_t> request_ipc_write_count{0};
    std::atomic<std::int64_t> request_ipc_write_nanoseconds{0};
    std::atomic<std::uint64_t> response_ipc_read_count{0};
    std::atomic<std::int64_t> response_ipc_read_nanoseconds{0};
    std::atomic<std::uint64_t> response_completion_count{0};
    std::atomic<std::int64_t> response_completion_nanoseconds{0};
  };

  RuntimeProfilingCounters runtime_profiling_counters;

  std::int64_t steady_clock_nanoseconds_now()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  class ScopedProfilingSample
  {
  public:
    ScopedProfilingSample(std::atomic<std::uint64_t> &count, std::atomic<std::int64_t> &nanoseconds)
        : count_(count),
          nanoseconds_(nanoseconds),
          started_at_(steady_clock_nanoseconds_now())
    {
    }

    ~ScopedProfilingSample()
    {
      count_.fetch_add(1, std::memory_order_acq_rel);
      nanoseconds_.fetch_add(steady_clock_nanoseconds_now() - started_at_, std::memory_order_acq_rel);
    }

  private:
    std::atomic<std::uint64_t> &count_;
    std::atomic<std::int64_t> &nanoseconds_;
    std::int64_t started_at_;
  };

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

  std::string escaped_json_string(const std::string &value)
  {
    std::ostringstream escaped;
    escaped << '"';
    for (unsigned char character : value)
    {
      switch (character)
      {
        case '\\':
          escaped << "\\\\";
          break;
        case '"':
          escaped << "\\\"";
          break;
        case '\n':
          escaped << "\\n";
          break;
        case '\r':
          escaped << "\\r";
          break;
        case '\t':
          escaped << "\\t";
          break;
        default:
          if (character >= 0x20)
          {
            escaped << static_cast<char>(character);
          }
          else
          {
            escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(character)
                    << std::dec << std::setfill(' ');
          }
          break;
      }
    }
    escaped << '"';
    return escaped.str();
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

  std::int64_t rss_bytes_for_pid(pid_t pid)
  {
    if (pid <= 0)
    {
      return -1;
    }

    const std::string command = "ps -o rss= -p " + std::to_string(pid);
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
      return -1;
    }

    char buffer[128];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
      output.append(buffer);
    }
    pclose(pipe);

    std::stringstream parser(output);
    long rss_kilobytes = 0;
    parser >> rss_kilobytes;
    if (parser.fail() || rss_kilobytes < 0)
    {
      return -1;
    }

    return static_cast<std::int64_t>(rss_kilobytes) * 1024;
  }

  std::string request_path_for(const std::string &target)
  {
    const std::size_t delimiter = target.find('?');
    return target.substr(0, delimiter);
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

  VALUE ruby_string_from_header_value(VALUE value)
  {
    return rb_funcall(value, id_to_s, 0);
  }

  VALUE frozen_ruby_key(const char *name)
  {
    return rb_obj_freeze(rb_str_new_cstr(name));
  }

  VALUE ruby_rack_env_key_from(const std::string &key)
  {
    if (key == "REQUEST_METHOD")
    {
      return rb_key_request_method;
    }
    if (key == "SCRIPT_NAME")
    {
      return rb_key_script_name;
    }
    if (key == "PATH_INFO")
    {
      return rb_key_path_info;
    }
    if (key == "QUERY_STRING")
    {
      return rb_key_query_string;
    }
    if (key == "SERVER_PROTOCOL")
    {
      return rb_key_server_protocol;
    }
    if (key == "SERVER_NAME")
    {
      return rb_key_server_name;
    }
    if (key == "SERVER_PORT")
    {
      return rb_key_server_port;
    }
    if (key == "REMOTE_ADDR")
    {
      return rb_key_remote_addr;
    }
    if (key == "REMOTE_PORT")
    {
      return rb_key_remote_port;
    }
    if (key == "rack.url_scheme")
    {
      return rb_key_rack_url_scheme;
    }
    if (key == "CONTENT_TYPE")
    {
      return rb_key_content_type;
    }
    if (key == "CONTENT_LENGTH")
    {
      return rb_key_content_length;
    }

    return ruby_binary_string_from(key);
  }

  VALUE rack_header_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE)
  {
    auto *context = reinterpret_cast<HeaderCollectionContext *>(data);
    VALUE name = Qnil;
    VALUE value = Qnil;
    if (argc >= 2)
    {
      name = argv[0];
      value = argv[1];
    }
    else if (argc == 1 && TYPE(yielded) == T_ARRAY && RARRAY_LEN(yielded) >= 2)
    {
      name = rb_ary_entry(yielded, 0);
      value = rb_ary_entry(yielded, 1);
    }
    else
    {
      context->error_message = "Rack execution returned invalid headers";
      return Qnil;
    }

    context->headers.push_back(Vajra::response::Header{
        ruby_string_value(ruby_string_from_header_value(name)),
        ruby_string_value(ruby_string_from_header_value(value))});
    return Qnil;
  }

  VALUE rack_body_each_callback(VALUE yielded, VALUE data, int, const VALUE *, VALUE)
  {
    auto *context = reinterpret_cast<BodyCollectionContext *>(data);
    context->body.append(ruby_string_value(ruby_string_from_header_value(yielded)));
    return Qnil;
  }

  void close_rack_body(VALUE body)
  {
    if (rb_respond_to(body, id_close) == 0)
    {
      return;
    }
    rb_funcall(body, id_close, 0);
  }

  VALUE ruby_rack_env_from(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries,
      const std::string &request_body)
  {
    VALUE env = rb_hash_new();
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      rb_hash_aset(env, ruby_rack_env_key_from(entry.key), ruby_binary_string_from(entry.value));
    }

    VALUE rack_version = rb_ary_new_capa(2);
    rb_ary_push(rack_version, INT2FIX(1));
    rb_ary_push(rack_version, INT2FIX(6));
    rb_hash_aset(env, rb_key_rack_version, rack_version);
    if (NIL_P(rb_cStringIO))
    {
      rb_cStringIO = rb_path2class("StringIO");
    }
    rb_hash_aset(env, rb_key_rack_input, rb_funcall(rb_cStringIO, id_new, 1, ruby_binary_string_from(request_body)));
    rb_hash_aset(env, rb_key_rack_errors, rb_gv_get("$stderr"));
    rb_hash_aset(env, rb_key_rack_multithread, Qfalse);
    rb_hash_aset(env, rb_key_rack_multiprocess, Qfalse);
    rb_hash_aset(env, rb_key_rack_run_once, Qfalse);
    return env;
  }

  struct DirectRackObservabilityFields
  {
    std::string method;
    std::string target;
    std::string protocol;
    std::string host;
    std::string traceparent;
  };

  DirectRackObservabilityFields direct_rack_observability_fields(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    DirectRackObservabilityFields fields;
    std::string path;
    std::string query;
    std::string server_name;
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      if (entry.key == "REQUEST_METHOD")
      {
        fields.method = entry.value;
      }
      else if (entry.key == "PATH_INFO")
      {
        path = entry.value;
      }
      else if (entry.key == "QUERY_STRING")
      {
        query = entry.value;
      }
      else if (entry.key == "SERVER_PROTOCOL")
      {
        fields.protocol = entry.value;
      }
      else if (entry.key == "HTTP_HOST")
      {
        fields.host = entry.value;
      }
      else if (entry.key == "SERVER_NAME")
      {
        server_name = entry.value;
      }
      else if (entry.key == "HTTP_TRACEPARENT")
      {
        fields.traceparent = entry.value;
      }
    }
    fields.target = std::move(path);
    if (!query.empty())
    {
      fields.target += "?" + query;
    }
    if (fields.host.empty())
    {
      fields.host = std::move(server_name);
    }
    return fields;
  }

  std::string traceparent_part(const std::string &traceparent, int part)
  {
    std::size_t cursor = 0;
    for (int index = 0; index < part; ++index)
    {
      cursor = traceparent.find('-', cursor);
      if (cursor == std::string::npos)
      {
        return "";
      }
      ++cursor;
    }
    const std::size_t end = traceparent.find('-', cursor);
    return traceparent.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
  }

  Vajra::runtime::RequestSpanEvent direct_rack_observability_event(
      const DirectRackObservabilityFields &fields,
      const Vajra::response::Response &response,
      std::chrono::steady_clock::time_point started_at)
  {
    Vajra::runtime::RequestSpanEvent event;
    event.method = fields.method;
    event.target = fields.target;
    event.status_code = response.status.code;
    event.duration_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count();
    event.protocol = fields.protocol;
    event.host = fields.host;
    event.outcome = "completed";
    event.response_sent = true;
    event.connection_outcome = response.connection_behavior == Vajra::response::ConnectionBehavior::close ? "close" : "keepalive";
    event.worker_index = static_cast<int>(Vajra::runtime::current_worker_index());
    event.worker_pid = getpid();
    event.trace_id = traceparent_part(fields.traceparent, 1);
    event.span_id = traceparent_part(fields.traceparent, 2);
    return event;
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

  Vajra::response::Response response_from_rack_result(VALUE value)
  {
    if (TYPE(value) != T_ARRAY || RARRAY_LEN(value) != 3)
    {
      throw std::runtime_error("Rack execution returned an invalid response");
    }

    VALUE status = rb_ary_entry(value, 0);
    VALUE headers = rb_ary_entry(value, 1);
    VALUE body = rb_ary_entry(value, 2);

    HeaderCollectionContext header_context;
    rb_block_call(headers, id_each, 0, nullptr, rack_header_each_callback, reinterpret_cast<VALUE>(&header_context));
    if (!header_context.error_message.empty())
    {
      throw std::runtime_error(header_context.error_message);
    }

    BodyCollectionContext body_context;
    try
    {
      rb_block_call(body, id_each, 0, nullptr, rack_body_each_callback, reinterpret_cast<VALUE>(&body_context));
    }
    catch (...)
    {
      close_rack_body(body);
      throw;
    }
    close_rack_body(body);
    if (!body_context.error_message.empty())
    {
      throw std::runtime_error(body_context.error_message);
    }

    const int status_code = status_code_from_ruby(status);
    return Vajra::response::Response{
        Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
        std::move(header_context.headers),
        std::move(body_context.body),
        Vajra::response::ConnectionBehavior::close};
  }

  VALUE protected_execute_native_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    const auto started_at = std::chrono::steady_clock::now();
    try
    {
      VALUE app = Qnil;
      {
        const std::lock_guard<std::mutex> app_lock(rack_execution_app_mutex);
        app = rack_execution_app;
      }

      if (NIL_P(app))
      {
        return Qnil;
      }

      VALUE env = ruby_rack_env_from(*context->env_entries, *context->request_body);
      VALUE result = rb_funcall(app, id_call, 1, env);
      context->response = response_from_rack_result(result);
      DirectRackObservabilityFields observability_fields;
      if (context->response && Vajra::runtime::runtime_request_span_observability_enabled())
      {
        observability_fields = direct_rack_observability_fields(*context->env_entries);
      }
      if (context->response && Vajra::runtime::runtime_trace_sampled(observability_fields.traceparent))
      {
        Vajra::runtime::emit_runtime_request_span_event(
            direct_rack_observability_event(observability_fields, *context->response, started_at));
      }
      return Qnil;
    }
    catch (const std::exception &error)
    {
      rb_raise(rb_eRuntimeError, "%s", error.what());
    }
    catch (...)
    {
      rb_raise(rb_eRuntimeError, "Rack native request execution failed with an unknown native error");
    }
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
    const auto started_at = std::chrono::steady_clock::now();
    ScopedProfilingSample profiling_sample(
        runtime_profiling_counters.ruby_execution_count,
        runtime_profiling_counters.ruby_execution_nanoseconds);
    Vajra::runtime::note_worker_execution_started();
    const auto profiling_cleanup = []() {
      Vajra::runtime::note_worker_execution_finished();
    };
    auto *context = static_cast<ExecutionCallContext *>(data);

    int state = 0;
    VALUE result = rb_protect(
        context->use_native_app ? protected_execute_native_rack_request : protected_execute_rack_request,
        reinterpret_cast<VALUE>(context),
        &state);
    if (state != 0)
    {
      context->error_message = exception_message(rb_errinfo());
      rb_set_errinfo(Qnil);
      profiling_cleanup();
      Vajra::runtime::note_worker_rack_execution_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started_at)
              .count());
      return nullptr;
    }

    if (!context->use_native_app && !NIL_P(result))
    {
      ResponseNormalizationContext normalization_context{result, std::nullopt, ""};
      state = 0;
      rb_protect(protected_normalize_rack_response, reinterpret_cast<VALUE>(&normalization_context), &state);
      if (state != 0)
      {
        context->error_message = exception_message(rb_errinfo());
        rb_set_errinfo(Qnil);
        profiling_cleanup();
        Vajra::runtime::note_worker_rack_execution_time(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        return nullptr;
      }
      if (!normalization_context.error_message.empty())
      {
        context->error_message = normalization_context.error_message;
        profiling_cleanup();
        Vajra::runtime::note_worker_rack_execution_time(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        return nullptr;
      }

      context->response = std::move(normalization_context.response);
    }

    profiling_cleanup();
    Vajra::runtime::note_worker_rack_execution_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
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
      context.use_native_app =
          rack_execution_app_installed_flag.load(std::memory_order_acquire) &&
          !Vajra::runtime::runtime_tracing_active_context_required();
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
      context.use_native_app =
          rack_execution_app_installed_flag.load(std::memory_order_acquire) &&
          !Vajra::runtime::runtime_tracing_active_context_required();
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
      ScopedProfilingSample profiling_sample(
          runtime_profiling_counters.request_ipc_write_count,
          runtime_profiling_counters.request_ipc_write_nanoseconds);
      write_frame(channel_fd_, Vajra::ipc::FrameFamily::request_execution_input, encode_request_execution_input(env_entries));
    }

    void append_request_body_chunk(const std::string &chunk) override
    {
      ScopedProfilingSample profiling_sample(
          runtime_profiling_counters.request_ipc_write_count,
          runtime_profiling_counters.request_ipc_write_nanoseconds);
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
      ScopedProfilingSample response_completion_sample(
          runtime_profiling_counters.response_completion_count,
          runtime_profiling_counters.response_completion_nanoseconds);
      {
        ScopedProfilingSample request_write_sample(
            runtime_profiling_counters.request_ipc_write_count,
            runtime_profiling_counters.request_ipc_write_nanoseconds);
        write_frame(
            channel_fd_,
            Vajra::ipc::FrameFamily::request_body_continuation,
            encode_request_body_chunk(RequestBodyEvent::complete, ""));
      }
      finished_ = true;

      Vajra::ipc::FrameHeader header{};
      std::string payload;
      ScopedProfilingSample response_read_sample(
          runtime_profiling_counters.response_ipc_read_count,
          runtime_profiling_counters.response_ipc_read_nanoseconds);
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

  const char *worker_lifecycle_state_name(Vajra::runtime::WorkerLifecycleState lifecycle_state)
  {
    switch (lifecycle_state)
    {
      case Vajra::runtime::WorkerLifecycleState::booting:
        return "booting";
      case Vajra::runtime::WorkerLifecycleState::ready:
        return "ready";
      case Vajra::runtime::WorkerLifecycleState::stopping:
        return "stopping";
      case Vajra::runtime::WorkerLifecycleState::exited:
        return "exited";
    }

    return "unknown";
  }

  const char *worker_health_state_name(Vajra::runtime::WorkerHealthState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerHealthState::healthy:
        return "healthy";
      case Vajra::runtime::WorkerHealthState::busy:
        return "busy";
      case Vajra::runtime::WorkerHealthState::overloaded:
        return "overloaded";
      case Vajra::runtime::WorkerHealthState::degraded:
        return "degraded";
      case Vajra::runtime::WorkerHealthState::suspect:
        return "suspect";
      case Vajra::runtime::WorkerHealthState::wedged:
        return "wedged";
    }

    return "unknown";
  }

  const char *worker_recovery_state_name(Vajra::runtime::WorkerRecoveryState state)
  {
    switch (state)
    {
      case Vajra::runtime::WorkerRecoveryState::none:
        return "none";
      case Vajra::runtime::WorkerRecoveryState::draining:
        return "draining";
      case Vajra::runtime::WorkerRecoveryState::terminating:
        return "terminating";
      case Vajra::runtime::WorkerRecoveryState::replacing:
        return "replacing";
      case Vajra::runtime::WorkerRecoveryState::rejoin_pending:
        return "rejoin_pending";
      case Vajra::runtime::WorkerRecoveryState::terminal_failure:
        return "terminal_failure";
    }

    return "unknown";
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
    std::unique_ptr<Vajra::rack::RackExecutionSession> live_session_;
    bool finished_ = false;
    bool canceled_ = false;
  };

  struct PendingRequest
  {
    std::uint64_t request_id = 0;
    int client_fd = -1;
    std::vector<Vajra::request::RackEnvEntry> env_entries;
    std::chrono::steady_clock::time_point enqueued_at;
    std::chrono::steady_clock::time_point deadline;
    std::atomic_bool assigned = false;
    std::atomic_bool released = false;
    std::atomic_bool canceled = false;
    std::atomic_bool timed_out = false;
    std::atomic_bool client_gone = false;
    bool enqueued = false;
    std::size_t worker_index = 0;
    std::size_t channel_index = 0;
    std::uint64_t channel_generation = 0;
  };

  class GlobalQueuedWorkerProcessRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    struct WorkerChannel
    {
      WorkerChannel(int fd, std::size_t worker_timeout_seconds, const std::function<void()> &timeout_handler)
          : worker_timeout_seconds(worker_timeout_seconds),
            transport(std::make_shared<WorkerProcessRackExecutionTransport>(
                fd,
                worker_timeout_seconds,
                timeout_handler))
      {
      }

      std::size_t worker_timeout_seconds;
      std::shared_ptr<WorkerProcessRackExecutionTransport> transport;
      bool busy = false;
    };

    struct WorkerSlot
    {
      WorkerSlot(
          std::vector<WorkerChannel> worker_channels,
          std::shared_ptr<Vajra::runtime::SharedWorkerState> worker_state,
          std::size_t min_channel_count)
          : channels(std::move(worker_channels)),
            state(std::move(worker_state)),
            channel_generation(state->channel_generation.load(std::memory_order_acquire)),
            min_channels(std::min(min_channel_count, channels.size())),
            active_channels(std::min(min_channel_count, channels.size()))
      {
      }

      std::vector<WorkerChannel> channels;
      std::shared_ptr<Vajra::runtime::SharedWorkerState> state;
      std::uint64_t channel_generation;
      std::size_t min_channels;
      std::size_t active_channels;
    };

    GlobalQueuedWorkerProcessRackExecutionTransport(
        const std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> &worker_states,
        std::size_t max_threads,
        std::size_t queue_capacity,
        std::size_t request_timeout_seconds,
        std::size_t worker_timeout_seconds,
        std::function<void(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &)> worker_timeout_handler,
        bool debug_logging)
        : queue_capacity_(queue_capacity),
          request_timeout_(std::chrono::seconds(request_timeout_seconds)),
          worker_timeout_handler_(std::move(worker_timeout_handler)),
          debug_logging_(debug_logging)
    {
      if (worker_states.empty())
      {
        throw std::logic_error("worker request transport requires at least one channel");
      }

      slots_.reserve(worker_states.size());
      for (std::size_t worker_index = 0; worker_index < worker_states.size(); ++worker_index)
      {
        const std::shared_ptr<Vajra::runtime::SharedWorkerState> &worker_state = worker_states[worker_index];
        const std::vector<int> &channel_fds = worker_state->request_channel_fds;
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
            worker_state,
            max_threads));
      }

      housekeeping_thread_ = std::thread([this]() { housekeeping_loop(); });
    }

    ~GlobalQueuedWorkerProcessRackExecutionTransport() override
    {
      {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        housekeeping_stop_requested_ = true;
      }
      scheduler_condition_.notify_all();
      if (housekeeping_thread_.joinable())
      {
        housekeeping_thread_.join();
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

    std::string stats_payload_json() const override
    {
      const pid_t master_pid = getpid();
      std::size_t healthy_workers = 0;
      std::size_t busy_workers = 0;
      std::size_t overloaded_workers = 0;
      std::size_t degraded_workers = 0;
      std::size_t suspect_workers = 0;
      std::size_t wedged_workers = 0;
      std::ostringstream payload;
      payload << '{'
              << "\"scheduler_policy\":\"least_loaded\","
              << "\"master_pid\":" << master_pid << ','
              << "\"master_rss_bytes\":" << rss_bytes_for_pid(master_pid) << ','
              << "\"queue_depth\":" << queued_request_count_.load(std::memory_order_acquire) << ','
              << "\"queue_capacity\":" << queue_capacity_ << ','
              << "\"oldest_queue_age_nanoseconds\":" << oldest_queue_age_nanoseconds_.load(std::memory_order_acquire) << ','
              << "\"tracing\":{\"enabled\":" << (Vajra::runtime::runtime_tracing_enabled() ? "true" : "false")
              << ",\"available\":" << (Vajra::runtime::runtime_tracing_available() ? "true" : "false")
              << ",\"endpoint\":" << escaped_json_string(Vajra::runtime::runtime_tracing_endpoint())
              << ",\"service_name\":" << escaped_json_string(Vajra::runtime::runtime_tracing_service_name()) << "},"
              << "\"profiling\":{\"scheduler_selection_count\":"
              << runtime_profiling_counters.scheduler_selection_count.load(std::memory_order_acquire)
              << ",\"scheduler_selection_nanoseconds\":"
              << runtime_profiling_counters.scheduler_selection_nanoseconds.load(std::memory_order_acquire)
              << ",\"ruby_execution_count\":"
              << runtime_profiling_counters.ruby_execution_count.load(std::memory_order_acquire)
              << ",\"ruby_execution_nanoseconds\":"
              << runtime_profiling_counters.ruby_execution_nanoseconds.load(std::memory_order_acquire)
              << ",\"request_ipc_write_count\":"
              << runtime_profiling_counters.request_ipc_write_count.load(std::memory_order_acquire)
              << ",\"request_ipc_write_nanoseconds\":"
              << runtime_profiling_counters.request_ipc_write_nanoseconds.load(std::memory_order_acquire)
              << ",\"response_ipc_read_count\":"
              << runtime_profiling_counters.response_ipc_read_count.load(std::memory_order_acquire)
              << ",\"response_ipc_read_nanoseconds\":"
              << runtime_profiling_counters.response_ipc_read_nanoseconds.load(std::memory_order_acquire)
              << ",\"response_completion_count\":"
              << runtime_profiling_counters.response_completion_count.load(std::memory_order_acquire)
              << ",\"response_completion_nanoseconds\":"
              << runtime_profiling_counters.response_completion_nanoseconds.load(std::memory_order_acquire)
              << "},"
              << "\"request_admission_rejections\":"
              << request_admission_rejection_count_.load(std::memory_order_acquire) << ','
              << "\"workers\":[";
      for (std::size_t index = 0; index < slots_.size(); ++index)
      {
        if (index > 0)
        {
          payload << ',';
        }
        const std::shared_ptr<WorkerSlot> &slot = slots_[index];
        const auto health_state = slot->state->health_state.load(std::memory_order_acquire);
        switch (health_state)
        {
          case Vajra::runtime::WorkerHealthState::healthy: ++healthy_workers; break;
          case Vajra::runtime::WorkerHealthState::busy: ++busy_workers; break;
          case Vajra::runtime::WorkerHealthState::overloaded: ++overloaded_workers; break;
          case Vajra::runtime::WorkerHealthState::degraded: ++degraded_workers; break;
          case Vajra::runtime::WorkerHealthState::suspect: ++suspect_workers; break;
          case Vajra::runtime::WorkerHealthState::wedged: ++wedged_workers; break;
        }
        payload << '{'
                << "\"worker_index\":" << slot->state->worker_index << ','
                << "\"pid\":" << slot->state->pid.load(std::memory_order_acquire) << ','
                << "\"rss_bytes\":" << rss_bytes_for_pid(slot->state->pid.load(std::memory_order_acquire)) << ','
                << "\"active_channels\":" << slot->state->active_execution_count.load(std::memory_order_acquire) << ','
                << "\"channel_capacity\":" << slot->state->request_channel_count.load(std::memory_order_acquire) << ','
                << "\"active_execution_count\":" << slot->state->active_execution_count.load(std::memory_order_acquire) << ','
                << "\"idle_execution_count\":" << slot->state->idle_execution_count.load(std::memory_order_acquire) << ','
                << "\"local_queue_depth\":" << slot->state->local_queue_depth.load(std::memory_order_acquire) << ','
                << "\"oldest_local_queue_age_nanoseconds\":"
                << slot->state->oldest_local_queue_age_nanoseconds.load(std::memory_order_acquire) << ','
                << "\"available\":" << (slot->state->available.load(std::memory_order_acquire) ? "true" : "false")
                << ",\"lifecycle_state\":" << static_cast<int>(slot->state->lifecycle_state.load(std::memory_order_acquire))
                << ",\"lifecycle_state_name\":\""
                << worker_lifecycle_state_name(slot->state->lifecycle_state.load(std::memory_order_acquire)) << "\""
                << ",\"health_state\":" << static_cast<int>(health_state)
                << ",\"health_state_name\":\"" << worker_health_state_name(health_state) << "\""
                << ",\"recovery_state_name\":\""
                << worker_recovery_state_name(slot->state->recovery_state.load(std::memory_order_acquire)) << "\""
                << ",\"replacement_attempt_count\":" << slot->state->replacement_attempt_count.load(std::memory_order_acquire)
                << ",\"replacement_success_count\":" << slot->state->replacement_success_count.load(std::memory_order_acquire)
                << ",\"replacement_failure_count\":" << slot->state->replacement_failure_count.load(std::memory_order_acquire)
                << ",\"health_transition_count\":" << slot->state->health_transition_count.load(std::memory_order_acquire)
                << ",\"timeout_escalation_count\":" << slot->state->timeout_escalation_count.load(std::memory_order_acquire)
                << ",\"unexpected_exit_count\":" << slot->state->unexpected_exit_count.load(std::memory_order_acquire)
                << ",\"last_unexpected_exit_nanoseconds\":" << slot->state->last_unexpected_exit_nanoseconds.load(std::memory_order_acquire)
                << ",\"last_progress_nanoseconds\":" << slot->state->last_progress_nanoseconds.load(std::memory_order_acquire)
                << ",\"last_lifecycle_transition_nanoseconds\":" << slot->state->last_lifecycle_transition_nanoseconds.load(std::memory_order_acquire)
                << ",\"last_health_transition_nanoseconds\":" << slot->state->last_health_transition_nanoseconds.load(std::memory_order_acquire)
                << ",\"overload_started_nanoseconds\":" << slot->state->overload_started_nanoseconds.load(std::memory_order_acquire)
                << ",\"recovery_deadline_nanoseconds\":" << slot->state->recovery_deadline_nanoseconds.load(std::memory_order_acquire)
                << ",\"terminal_replacement_failure\":"
                << (slot->state->terminal_replacement_failure.load(std::memory_order_acquire) ? "true" : "false")
                << '}';
      }
      payload << "],\"health_counts\":{\"healthy\":" << healthy_workers
              << ",\"busy\":" << busy_workers
              << ",\"overloaded\":" << overloaded_workers
              << ",\"degraded\":" << degraded_workers
              << ",\"suspect\":" << suspect_workers
              << ",\"wedged\":" << wedged_workers
              << "}}";
      return payload.str();
    }

    std::string metrics_payload_text() const override
    {
      std::ostringstream payload;
      payload << "vajra_scheduler_queue_depth " << queued_request_count_.load(std::memory_order_acquire) << '\n';
      payload << "vajra_master_pid " << getpid() << '\n';
      payload << "vajra_master_rss_bytes " << rss_bytes_for_pid(getpid()) << '\n';
      payload << "vajra_scheduler_queue_capacity " << queue_capacity_ << '\n';
      payload << "vajra_scheduler_oldest_queue_age_nanoseconds "
              << oldest_queue_age_nanoseconds_.load(std::memory_order_acquire) << '\n';
      payload << "vajra_request_admission_rejections_total "
              << request_admission_rejection_count_.load(std::memory_order_acquire) << '\n';
      payload << "vajra_tracing_enabled " << (Vajra::runtime::runtime_tracing_enabled() ? 1 : 0) << '\n';
      payload << "vajra_tracing_available " << (Vajra::runtime::runtime_tracing_available() ? 1 : 0) << '\n';
      payload << "vajra_scheduler_selection_total "
              << runtime_profiling_counters.scheduler_selection_count.load(std::memory_order_acquire) << '\n';
      payload << "vajra_scheduler_selection_nanoseconds_total "
              << runtime_profiling_counters.scheduler_selection_nanoseconds.load(std::memory_order_acquire) << '\n';
      payload << "vajra_ruby_execution_total "
              << runtime_profiling_counters.ruby_execution_count.load(std::memory_order_acquire) << '\n';
      payload << "vajra_ruby_execution_nanoseconds_total "
              << runtime_profiling_counters.ruby_execution_nanoseconds.load(std::memory_order_acquire) << '\n';
      payload << "vajra_request_ipc_write_total "
              << runtime_profiling_counters.request_ipc_write_count.load(std::memory_order_acquire) << '\n';
      payload << "vajra_request_ipc_write_nanoseconds_total "
              << runtime_profiling_counters.request_ipc_write_nanoseconds.load(std::memory_order_acquire) << '\n';
      payload << "vajra_response_ipc_read_total "
              << runtime_profiling_counters.response_ipc_read_count.load(std::memory_order_acquire) << '\n';
      payload << "vajra_response_ipc_read_nanoseconds_total "
              << runtime_profiling_counters.response_ipc_read_nanoseconds.load(std::memory_order_acquire) << '\n';
      payload << "vajra_response_completion_total "
              << runtime_profiling_counters.response_completion_count.load(std::memory_order_acquire) << '\n';
      payload << "vajra_response_completion_nanoseconds_total "
              << runtime_profiling_counters.response_completion_nanoseconds.load(std::memory_order_acquire) << '\n';
      for (const auto &slot : slots_)
      {
        const std::size_t worker_index = slot->state->worker_index;
        const auto health_state = slot->state->health_state.load(std::memory_order_acquire);
        payload << "vajra_worker_active_channels{worker=\"" << worker_index << "\"} "
                << slot->state->active_execution_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_pid{worker=\"" << worker_index << "\"} "
                << slot->state->pid.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_rss_bytes{worker=\"" << worker_index << "\"} "
                << rss_bytes_for_pid(slot->state->pid.load(std::memory_order_acquire)) << '\n';
        payload << "vajra_worker_channel_capacity{worker=\"" << worker_index << "\"} "
                << slot->state->request_channel_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_available{worker=\"" << worker_index << "\"} "
                << (slot->state->available.load(std::memory_order_acquire) ? 1 : 0) << '\n';
        payload << "vajra_worker_active_executions{worker=\"" << worker_index << "\"} "
                << slot->state->active_execution_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_idle_executions{worker=\"" << worker_index << "\"} "
                << slot->state->idle_execution_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_local_queue_depth{worker=\"" << worker_index << "\"} "
                << slot->state->local_queue_depth.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_oldest_queue_age_nanoseconds{worker=\"" << worker_index << "\"} "
                << slot->state->oldest_local_queue_age_nanoseconds.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_health_state{worker=\"" << worker_index
                << "\",state=\"" << worker_health_state_name(health_state) << "\"} 1\n";
        payload << "vajra_worker_replacements_started_total{worker=\"" << worker_index << "\"} "
                << slot->state->replacement_attempt_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_replacements_completed_total{worker=\"" << worker_index << "\"} "
                << slot->state->replacement_success_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_replacements_failed_total{worker=\"" << worker_index << "\"} "
                << slot->state->replacement_failure_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_timeout_escalations_total{worker=\"" << worker_index << "\"} "
                << slot->state->timeout_escalation_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_unexpected_exits_total{worker=\"" << worker_index << "\"} "
                << slot->state->unexpected_exit_count.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_last_unexpected_exit_nanoseconds{worker=\"" << worker_index << "\"} "
                << slot->state->last_unexpected_exit_nanoseconds.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_recovery_deadline_nanoseconds{worker=\"" << worker_index << "\"} "
                << slot->state->recovery_deadline_nanoseconds.load(std::memory_order_acquire) << '\n';
        payload << "vajra_worker_terminal_replacement_failure{worker=\"" << worker_index << "\"} "
                << (slot->state->terminal_replacement_failure.load(std::memory_order_acquire) ? 1 : 0) << '\n';
      }
      return payload.str();
    }

    std::shared_ptr<WorkerSlot> slot_for(std::size_t worker_index) const
    {
      return slots_.at(worker_index);
    }

    std::shared_ptr<WorkerProcessRackExecutionTransport> assigned_channel_transport(
        const std::shared_ptr<PendingRequest> &pending_request) const
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex_);
      const std::shared_ptr<WorkerSlot> slot = slot_for(pending_request->worker_index);
      sync_slot_channels_locked(slot);
      if (pending_request->channel_index >= slot->channels.size())
      {
        throw std::runtime_error("assigned worker channel is no longer available");
      }
      pending_request->channel_generation = slot->channel_generation;
      return slot->channels.at(pending_request->channel_index).transport;
    }

    std::pair<std::size_t, std::size_t> await_assignment(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      std::unique_lock<std::mutex> lock(scheduler_mutex_);
      while (!pending_request->assigned.load() &&
             !pending_request->timed_out.load() &&
             !pending_request->client_gone.load() &&
             !pending_request->canceled.load())
      {
        const auto now = std::chrono::steady_clock::now();
        prune_queue_head_locked(now);
        refresh_pending_request_state_locked(pending_request, now, true);
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
            sync_slot_channels_locked(slot);
            slot->channels[assignment->second].busy = true;
            const std::size_t inflight = inflight_count_locked(*slot);
            pending_request->assigned = true;
            pending_request->worker_index = assignment->first;
            pending_request->channel_index = assignment->second;
            pending_request->channel_generation = slot->channel_generation;
            slot->state->active_execution_count.store(inflight, std::memory_order_release);
            slot->state->idle_execution_count.store(slot->channels.size() - inflight, std::memory_order_release);
            slot->state->last_progress_nanoseconds.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                std::memory_order_release);
            log_scheduler_debug_event(
                "event=request_assigned policy=least_loaded request_id=" + std::to_string(pending_request->request_id) +
                    " selected_worker=" + std::to_string(assignment->first) +
                    " channel=" + std::to_string(assignment->second) +
                    " inflight=" + std::to_string(inflight) +
                    " queue_depth=" + std::to_string(queued_request_count_.load(std::memory_order_acquire) - 1),
                debug_logging_);
            pending_request->enqueued = false;
            queued_request_count_.fetch_sub(1, std::memory_order_acq_rel);
            pending_requests_.pop_front();
            update_worker_queue_pressure_locked(now);
            break;
          }
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
        sync_slot_channels_locked(slot);
        if (pending_request->channel_generation == slot->channel_generation &&
            pending_request->channel_index < slot->channels.size())
        {
          slot->channels.at(pending_request->channel_index).busy = false;
        }
        const std::size_t inflight = inflight_count_locked(*slot);
        pending_request->assigned = false;
        slot->state->active_execution_count.store(inflight, std::memory_order_release);
        slot->state->idle_execution_count.store(slot->channels.size() - inflight, std::memory_order_release);
        slot->state->last_progress_nanoseconds.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            std::memory_order_release);
        while (slot->active_channels > slot->min_channels &&
               !slot->channels[slot->active_channels - 1].busy &&
               pending_requests_.empty())
        {
          --slot->active_channels;
        }
        update_worker_queue_pressure_locked(std::chrono::steady_clock::now());
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
          update_worker_queue_pressure_locked(std::chrono::steady_clock::now());
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
    void sync_slot_channels_locked(const std::shared_ptr<WorkerSlot> &slot) const
    {
      const std::uint64_t latest_generation = slot->state->channel_generation.load(std::memory_order_acquire);
      if (slot->channel_generation == latest_generation)
      {
        return;
      }

      std::lock_guard<std::mutex> lock(slot->state->request_channel_mutex);
      const std::vector<int> &channel_fds = slot->state->request_channel_fds;
      std::vector<WorkerChannel> worker_channels;
      worker_channels.reserve(channel_fds.size());
      for (int channel_fd : channel_fds)
      {
        worker_channels.emplace_back(
            channel_fd,
            slot->channels.empty() ? 60 : slot->channels.front().worker_timeout_seconds,
            [this, worker_index = slot->state->worker_index]() { mark_worker_timed_out(worker_index); });
      }

      slot->channels = std::move(worker_channels);
      slot->active_channels = std::min(slot->min_channels, slot->channels.size());
      slot->channel_generation = latest_generation;
      slot->state->active_execution_count.store(0, std::memory_order_release);
      slot->state->idle_execution_count.store(slot->channels.size(), std::memory_order_release);
    }

    static bool worker_schedulable(const WorkerSlot &slot)
    {
      return slot.state->available.load(std::memory_order_acquire) &&
             (slot.state->health_state.load(std::memory_order_acquire) == Vajra::runtime::WorkerHealthState::healthy ||
              slot.state->health_state.load(std::memory_order_acquire) == Vajra::runtime::WorkerHealthState::busy) &&
             slot.state->lifecycle_state.load(std::memory_order_acquire) == Vajra::runtime::WorkerLifecycleState::ready;
    }

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

    std::chrono::steady_clock::duration oldest_queue_age_locked(std::chrono::steady_clock::time_point now) const
    {
      for (const auto &pending_request : pending_requests_)
      {
        if (pending_request->assigned.load() ||
            pending_request->canceled.load() ||
            pending_request->timed_out.load() ||
            pending_request->client_gone.load())
        {
          continue;
        }
        return now - pending_request->enqueued_at;
      }

      return std::chrono::steady_clock::duration::zero();
    }

    void update_worker_queue_pressure_locked(std::chrono::steady_clock::time_point now) const
    {
      const auto oldest_queue_age = oldest_queue_age_locked(now);
      const auto oldest_queue_age_nanoseconds =
          std::chrono::duration_cast<std::chrono::nanoseconds>(oldest_queue_age).count();
      oldest_queue_age_nanoseconds_.store(oldest_queue_age_nanoseconds, std::memory_order_release);

      for (const auto &slot : slots_)
      {
        sync_slot_channels_locked(slot);
        const std::size_t inflight = inflight_count_locked(*slot);
        const std::size_t channel_capacity = slot->channels.size();
        const bool saturated = channel_capacity > 0 && inflight >= channel_capacity;
        const std::size_t local_queue_depth = saturated ? queued_request_count_.load(std::memory_order_acquire) : 0;

        slot->state->active_execution_count.store(inflight, std::memory_order_release);
        slot->state->idle_execution_count.store(channel_capacity - std::min(inflight, channel_capacity), std::memory_order_release);
        slot->state->local_queue_depth.store(local_queue_depth, std::memory_order_release);
        slot->state->oldest_local_queue_age_nanoseconds.store(
            local_queue_depth > 0 ? oldest_queue_age_nanoseconds : 0,
            std::memory_order_release);
        if (local_queue_depth == 0)
        {
          slot->state->overload_started_nanoseconds.store(0, std::memory_order_release);
        }
      }
    }

    std::shared_ptr<PendingRequest> admit_request(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        int client_fd) const
    {
      std::lock_guard<std::mutex> lock(scheduler_mutex_);
      const auto now = std::chrono::steady_clock::now();
      prune_queue_locked(now);
      prune_queue_head_locked(now);
      update_worker_queue_pressure_locked(now);
      if (queued_request_count_.load(std::memory_order_acquire) >= queue_capacity_)
      {
        request_admission_rejection_count_.fetch_add(1, std::memory_order_acq_rel);
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
      pending_request->enqueued_at = now;
      pending_request->deadline = std::chrono::steady_clock::now() + request_timeout_;
      pending_request->enqueued = true;
      pending_requests_.push_back(pending_request);
      queued_request_count_.fetch_add(1, std::memory_order_acq_rel);
      update_worker_queue_pressure_locked(now);
      scheduler_condition_.notify_all();
      return pending_request;
    }

    void mark_worker_timed_out(std::size_t worker_index) const
    {
      std::shared_ptr<Vajra::runtime::SharedWorkerState> worker_state;
      pid_t worker_pid = -1;
      {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        const std::shared_ptr<WorkerSlot> slot = slot_for(worker_index);
        const Vajra::runtime::WorkerLifecycleState current_state =
            slot->state->lifecycle_state.load(std::memory_order_acquire);
        if (current_state == Vajra::runtime::WorkerLifecycleState::stopping ||
            current_state == Vajra::runtime::WorkerLifecycleState::exited)
        {
          return;
        }

        slot->state->available.store(false, std::memory_order_release);
        worker_state = slot->state;
        if (worker_state->timeout_handling_started.load(std::memory_order_acquire))
        {
          return;
        }
        worker_pid = worker_state->pid.load(std::memory_order_acquire);
        log_scheduler_debug_event(
            "event=worker_timeout worker_index=" + std::to_string(worker_index) +
                " worker_pid=" + std::to_string(worker_pid),
            debug_logging_);
        scheduler_condition_.notify_all();
      }

      if (!worker_timeout_handler_)
      {
        return;
      }

      try
      {
        worker_timeout_handler_(worker_state);
      }
      catch (const std::exception &error)
      {
        std::cerr << "[Vajra][error] " << Vajra::runtime::utc_timestamp()
                  << " failed to signal timed out worker pid=" << worker_pid
                  << ": " << error.what()
                  << std::endl;
      }
      catch (...)
      {
        std::cerr << "[Vajra][error] " << Vajra::runtime::utc_timestamp()
                  << " failed to signal timed out worker pid=" << worker_pid
                  << ": unknown native error"
                  << std::endl;
      }
    }

    std::optional<std::pair<std::size_t, std::size_t>> least_busy_channel_locked() const
    {
      ScopedProfilingSample profiling_sample(
          runtime_profiling_counters.scheduler_selection_count,
          runtime_profiling_counters.scheduler_selection_nanoseconds);
      std::optional<std::pair<std::size_t, std::size_t>> best_assignment;
      std::size_t best_worker_load = std::numeric_limits<std::size_t>::max();
      const std::size_t worker_count = slots_.size();

      for (std::size_t offset = 0; offset < worker_count; ++offset)
      {
        const std::size_t worker_index = (next_preferred_worker_ + offset) % worker_count;
        const std::shared_ptr<WorkerSlot> slot = slots_[worker_index];
        sync_slot_channels_locked(slot);
        if (!worker_schedulable(*slot))
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

    void refresh_pending_request_state_locked(
        const std::shared_ptr<PendingRequest> &pending_request,
        std::chrono::steady_clock::time_point now,
        bool check_client_disconnect) const
    {
      if (pending_request->assigned.load() ||
          pending_request->canceled.load() ||
          pending_request->timed_out.load() ||
          pending_request->client_gone.load())
      {
        return;
      }

      if (pending_request->deadline <= now)
      {
        pending_request->timed_out = true;
        log_scheduler_debug_event(
            "event=request_wait_timeout request_id=" + std::to_string(pending_request->request_id),
            debug_logging_);
        return;
      }

      if (check_client_disconnect && client_disconnected(pending_request->client_fd))
      {
        pending_request->client_gone = true;
        log_scheduler_debug_event(
            "event=request_client_disconnected request_id=" + std::to_string(pending_request->request_id),
            debug_logging_);
      }
    }

    void prune_queue_head_locked(std::chrono::steady_clock::time_point now) const
    {
      while (!pending_requests_.empty())
      {
        const std::shared_ptr<PendingRequest> &pending_request = pending_requests_.front();
        refresh_pending_request_state_locked(pending_request, now, true);
        if (!pending_request->canceled.load() &&
            !pending_request->timed_out.load() &&
            !pending_request->client_gone.load())
        {
          return;
        }

        pending_request->enqueued = false;
        queued_request_count_.fetch_sub(1, std::memory_order_acq_rel);
        pending_requests_.pop_front();
        update_worker_queue_pressure_locked(now);
      }
    }

    void prune_queue_locked(std::chrono::steady_clock::time_point now) const
    {
      for (const auto &pending_request : pending_requests_)
      {
        refresh_pending_request_state_locked(pending_request, now, true);
      }

      pending_requests_.erase(
          std::remove_if(
              pending_requests_.begin(),
              pending_requests_.end(),
              [this](const std::shared_ptr<PendingRequest> &pending_request) {
                if (!(pending_request->canceled.load() ||
                      pending_request->timed_out.load() ||
                      pending_request->client_gone.load()))
                {
                  return false;
                }

                if (pending_request->enqueued)
                {
                  pending_request->enqueued = false;
                  queued_request_count_.fetch_sub(1, std::memory_order_acq_rel);
                }
                return true;
              }),
          pending_requests_.end());
      update_worker_queue_pressure_locked(now);
    }

    void erase_pending_request_locked(const std::shared_ptr<PendingRequest> &pending_request) const
    {
      const auto iterator = std::find(pending_requests_.begin(), pending_requests_.end(), pending_request);
      if (iterator == pending_requests_.end())
      {
        return;
      }

      if ((*iterator)->enqueued)
      {
        (*iterator)->enqueued = false;
        queued_request_count_.fetch_sub(1, std::memory_order_acq_rel);
      }
      pending_requests_.erase(iterator);
      update_worker_queue_pressure_locked(std::chrono::steady_clock::now());
    }

    std::size_t queue_depth() const
    {
      return queued_request_count_.load(std::memory_order_acquire);
    }

    void housekeeping_loop() const
    {
      for (;;)
      {
        std::unique_lock<std::mutex> lock(scheduler_mutex_);
        if (housekeeping_stop_requested_)
        {
          return;
        }

        if (queued_request_count_.load(std::memory_order_acquire) == 0)
        {
          scheduler_condition_.wait(lock, [this]() {
            return housekeeping_stop_requested_ || queued_request_count_.load(std::memory_order_acquire) > 0;
          });
        }
        else
        {
          prune_queue_locked(std::chrono::steady_clock::now());
          prune_queue_head_locked(std::chrono::steady_clock::now());
          update_worker_queue_pressure_locked(std::chrono::steady_clock::now());
          scheduler_condition_.notify_all();
          scheduler_condition_.wait_for(lock, kQueueHousekeepingInterval, [this]() {
            return housekeeping_stop_requested_ || queued_request_count_.load(std::memory_order_acquire) == 0;
          });
        }
        if (housekeeping_stop_requested_)
        {
          return;
        }
      }
    }

    std::vector<std::shared_ptr<WorkerSlot>> slots_;
    std::size_t queue_capacity_;
    std::chrono::steady_clock::duration request_timeout_;
    std::function<void(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &)> worker_timeout_handler_;
    bool debug_logging_;
    mutable std::mutex scheduler_mutex_;
    mutable std::condition_variable scheduler_condition_;
    mutable std::deque<std::shared_ptr<PendingRequest>> pending_requests_;
    mutable std::atomic<std::size_t> queued_request_count_{0};
    mutable std::atomic<std::int64_t> oldest_queue_age_nanoseconds_{0};
    mutable std::atomic<std::uint64_t> request_admission_rejection_count_{0};
    mutable std::uint64_t next_request_id_ = 0;
    mutable std::size_t next_preferred_worker_ = 0;
    mutable bool housekeeping_stop_requested_ = false;
    mutable std::thread housekeeping_thread_;

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
    ensure_live_session_started();
    live_session_->append_request_body_chunk(chunk);
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

    (void)transport_.await_assignment(pending_request_);

    try
    {
      const std::shared_ptr<WorkerProcessRackExecutionTransport> channel_transport =
          transport_.assigned_channel_transport(pending_request_);
      live_session_ =
          channel_transport->start(
              pending_request_->env_entries,
              pending_request_->client_fd);
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

  struct WorkerRequestExecutionTask final : public Vajra::rack::WorkerExecutionTask
  {
    explicit WorkerRequestExecutionTask(
        std::vector<Vajra::request::RackEnvEntry> request_env_entries,
        std::string request_payload)
        : env_entries(std::move(request_env_entries)),
          request_body(std::move(request_payload))
    {
    }

    void execute() override
    {
      CurrentThreadRackExecutionTransport transport;
      try
      {
        response = transport.execute(env_entries, request_body);
        metadata_kind = response ? ResponseMetadataKind::response : ResponseMetadataKind::no_response;
      }
      catch (const Vajra::request::HeadError &error)
      {
        metadata_kind = ResponseMetadataKind::head_error;
        error_message = error.what();
      }
      catch (const std::exception &error)
      {
        metadata_kind = ResponseMetadataKind::execution_error;
        error_message = error.what();
      }

      mark_complete();
    }

    void cancel_due_to_pool_abort() override
    {
      metadata_kind = ResponseMetadataKind::execution_error;
      error_message = "worker local execution pool aborted before execution started";
      mark_complete();
    }

    void wait_for_completion()
    {
      std::unique_lock<std::mutex> lock(result_mutex);
      result_condition.wait(lock, [this]() { return completed; });
    }

    std::vector<Vajra::request::RackEnvEntry> env_entries;
    std::string request_body;
    ResponseMetadataKind metadata_kind = ResponseMetadataKind::no_response;
    std::optional<Vajra::response::Response> response;
    std::string error_message;

  private:
    void mark_complete()
    {
      {
        const std::lock_guard<std::mutex> lock(result_mutex);
        completed = true;
      }
      result_condition.notify_all();
    }

    std::mutex result_mutex;
    std::condition_variable result_condition;
    bool completed = false;
  };

  struct WorkerRequestExecutionPoolWaitContext
  {
    Vajra::rack::WorkerExecutionPool *pool = nullptr;
    std::shared_ptr<Vajra::rack::WorkerExecutionTask> task;
  };

  struct WorkerRequestExecutionTaskWaitContext
  {
    WorkerRequestExecutionTask *task = nullptr;
  };

  struct WorkerRequestLoopSharedState
  {
    WorkerRequestLoopSharedState(std::vector<int> owned_channel_fds, std::size_t max_threads)
        : channel_fds(std::move(owned_channel_fds)),
          execution_pool(max_threads),
          remaining_readers(channel_fds.size())
    {
    }

    void abort(const std::string &message)
    {
      bool expected = false;
      if (!abort_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      {
        return;
      }

      {
        const std::lock_guard<std::mutex> lock(error_mutex);
        error_message = message;
      }

      execution_pool.abort();
      for (int channel_fd : channel_fds)
      {
        if (channel_fd < 0)
        {
          continue;
        }
        if (shutdown(channel_fd, SHUT_RDWR) != 0 && errno != ENOTCONN && errno != EINVAL && errno != EBADF)
        {
          continue;
        }
      }
    }

    void mark_reader_finished()
    {
      if (remaining_readers.fetch_sub(1, std::memory_order_acq_rel) == 1)
      {
        execution_pool.close_input();
      }
    }

    std::optional<std::string> fatal_error() const
    {
      const std::lock_guard<std::mutex> lock(error_mutex);
      if (error_message.empty())
      {
        return std::nullopt;
      }
      return error_message;
    }

    const std::vector<int> channel_fds;
    Vajra::rack::WorkerExecutionPool execution_pool;
    std::atomic<std::size_t> remaining_readers;
    std::atomic_bool abort_requested{false};
    mutable std::mutex error_mutex;
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

  void *wait_for_worker_execution_task_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerRequestExecutionPoolWaitContext *>(data);
    context->task = context->pool->wait_for_task();
    return nullptr;
  }

  void *wait_for_worker_request_task_completion_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerRequestExecutionTaskWaitContext *>(data);
    context->task->wait_for_completion();
    return nullptr;
  }

  struct WorkerLoopThreadContext
  {
    int completion_signal_fd = -1;
    std::atomic_bool completed{false};
    std::atomic_bool failed{false};
    mutable std::mutex error_mutex;
    std::string error_message;
  };

  struct WorkerRequestReaderThreadContext final : public WorkerLoopThreadContext
  {
    int channel_fd = -1;
    std::shared_ptr<WorkerRequestLoopSharedState> runtime_state;
  };

  struct WorkerRequestExecutorThreadContext final : public WorkerLoopThreadContext
  {
    std::shared_ptr<WorkerRequestLoopSharedState> runtime_state;
  };

  struct WorkerChannelSignalReadContext
  {
    int fd = -1;
    bool eof = false;
    int error_number = 0;
  };

  std::optional<std::string> take_worker_loop_thread_error(const WorkerLoopThreadContext &context)
  {
    if (!context.failed.load(std::memory_order_acquire))
    {
      return std::nullopt;
    }

    const std::lock_guard<std::mutex> lock(context.error_mutex);
    return context.error_message;
  }

  void notify_worker_loop_thread_completion(
      WorkerLoopThreadContext *context,
      const std::optional<std::string> &error_message = std::nullopt)
  {
    if (error_message)
    {
      {
        const std::lock_guard<std::mutex> lock(context->error_mutex);
        context->error_message = *error_message;
      }
      context->failed.store(true, std::memory_order_release);
    }

    context->completed.store(true, std::memory_order_release);

    const std::uint8_t signal_byte = 1;
    for (;;)
    {
      const ssize_t result = write(context->completion_signal_fd, &signal_byte, sizeof(signal_byte));
      if (result >= 0)
      {
        return;
      }
      if (errno == EINTR)
      {
        continue;
      }
      return;
    }
  }

  void *wait_for_worker_channel_signal_without_gvl(void *data)
  {
    auto *context = static_cast<WorkerChannelSignalReadContext *>(data);
    std::uint8_t signal_byte = 0;

    for (;;)
    {
      const ssize_t result = read(context->fd, &signal_byte, sizeof(signal_byte));
      if (result > 0)
      {
        return nullptr;
      }
      if (result == 0)
      {
        context->eof = true;
        return nullptr;
      }
      if (errno == EINTR)
      {
        continue;
      }

      context->error_number = errno;
      return nullptr;
    }
  }

  void write_worker_request_task_result(
      int channel_fd,
      const WorkerRequestExecutionTask &task)
  {
    switch (task.metadata_kind)
    {
    case ResponseMetadataKind::no_response:
      write_frame(
          channel_fd,
          Vajra::ipc::FrameFamily::response_metadata_result,
          encode_response_metadata(std::nullopt));
      return;
    case ResponseMetadataKind::response:
    {
      write_frame(
          channel_fd,
          Vajra::ipc::FrameFamily::response_metadata_result,
          encode_response_metadata(task.response));
      if (!task.response)
      {
        return;
      }

      std::size_t cursor = 0;
      while (cursor < task.response->body.size())
      {
        const std::size_t length = std::min(kInlineBodyChunkBytes, task.response->body.size() - cursor);
        write_frame(
            channel_fd,
            Vajra::ipc::FrameFamily::response_body_continuation,
            encode_response_body_event(ResponseBodyEvent::chunk, task.response->body.substr(cursor, length)));
        cursor += length;
      }
      write_frame(
          channel_fd,
          Vajra::ipc::FrameFamily::response_body_continuation,
          encode_response_body_event(ResponseBodyEvent::complete, ""));
      return;
    }
    case ResponseMetadataKind::head_error:
    case ResponseMetadataKind::execution_error:
      write_frame(
          channel_fd,
          Vajra::ipc::FrameFamily::response_metadata_result,
          encode_response_error(task.metadata_kind, task.error_message));
      return;
    }
  }

  VALUE run_worker_request_reader_thread(void *data)
  {
    auto *context = static_cast<WorkerRequestReaderThreadContext *>(data);
    bool reader_finished = false;

    for (;;)
    {
      try
      {
        ChannelRequestReadResult read_result{context->channel_fd, false, false, {}, "", ""};
        rb_thread_call_without_gvl(
            read_worker_request_from_channel_without_gvl,
            &read_result,
            RUBY_UBF_IO,
            nullptr);

        if (!read_result.error_message.empty())
        {
          throw std::runtime_error(read_result.error_message);
        }
        if (read_result.eof)
        {
          break;
        }
        if (read_result.request_canceled)
        {
          continue;
        }

        auto task = std::make_shared<WorkerRequestExecutionTask>(
            std::move(read_result.env_entries),
            std::move(read_result.request_body));
        (void)context->runtime_state->execution_pool.enqueue(task);

        WorkerRequestExecutionTaskWaitContext wait_context{task.get()};
        rb_thread_call_without_gvl(
            wait_for_worker_request_task_completion_without_gvl,
            &wait_context,
            RUBY_UBF_IO,
            nullptr);
        write_worker_request_task_result(context->channel_fd, *task);
      }
      catch (const std::exception &error)
      {
        context->runtime_state->abort(error.what());
        if (!reader_finished)
        {
          context->runtime_state->mark_reader_finished();
          reader_finished = true;
        }
        notify_worker_loop_thread_completion(context, error.what());
        return Qnil;
      }
    }

    if (!reader_finished)
    {
      context->runtime_state->mark_reader_finished();
    }
    notify_worker_loop_thread_completion(context);
    return Qnil;
  }

  VALUE run_worker_request_executor_thread(void *data)
  {
    auto *context = static_cast<WorkerRequestExecutorThreadContext *>(data);

    try
    {
      for (;;)
      {
        WorkerRequestExecutionPoolWaitContext wait_context{&context->runtime_state->execution_pool, nullptr};
        rb_thread_call_without_gvl(
            wait_for_worker_execution_task_without_gvl,
            &wait_context,
            RUBY_UBF_IO,
            nullptr);
        if (!wait_context.task)
        {
          break;
        }

        wait_context.task->execute();
        context->runtime_state->execution_pool.finish_task();
      }
    }
    catch (const std::exception &error)
    {
      context->runtime_state->abort(error.what());
      notify_worker_loop_thread_completion(context, error.what());
      return Qnil;
    }

    notify_worker_loop_thread_completion(context);
    return Qnil;
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
  rb_global_variable(&rack_execution_app);
  rb_global_variable(&rb_cStringIO);
  rb_global_variable(&rb_key_content_length);
  rb_global_variable(&rb_key_content_type);
  rb_global_variable(&rb_key_path_info);
  rb_global_variable(&rb_key_query_string);
  rb_global_variable(&rb_key_rack_errors);
  rb_global_variable(&rb_key_rack_input);
  rb_global_variable(&rb_key_rack_multiprocess);
  rb_global_variable(&rb_key_rack_multithread);
  rb_global_variable(&rb_key_rack_run_once);
  rb_global_variable(&rb_key_rack_url_scheme);
  rb_global_variable(&rb_key_rack_version);
  rb_global_variable(&rb_key_remote_addr);
  rb_global_variable(&rb_key_remote_port);
  rb_global_variable(&rb_key_request_method);
  rb_global_variable(&rb_key_script_name);
  rb_global_variable(&rb_key_server_name);
  rb_global_variable(&rb_key_server_port);
  rb_global_variable(&rb_key_server_protocol);
  id_exception_message = rb_intern("message");
  id_call = rb_intern("call");
  id_close = rb_intern("close");
  id_each = rb_intern("each");
  id_new = rb_intern("new");
  id_to_s = rb_intern("to_s");
  rb_key_content_length = frozen_ruby_key("CONTENT_LENGTH");
  rb_key_content_type = frozen_ruby_key("CONTENT_TYPE");
  rb_key_path_info = frozen_ruby_key("PATH_INFO");
  rb_key_query_string = frozen_ruby_key("QUERY_STRING");
  rb_key_rack_errors = frozen_ruby_key("rack.errors");
  rb_key_rack_input = frozen_ruby_key("rack.input");
  rb_key_rack_multiprocess = frozen_ruby_key("rack.multiprocess");
  rb_key_rack_multithread = frozen_ruby_key("rack.multithread");
  rb_key_rack_run_once = frozen_ruby_key("rack.run_once");
  rb_key_rack_url_scheme = frozen_ruby_key("rack.url_scheme");
  rb_key_rack_version = frozen_ruby_key("rack.version");
  rb_key_remote_addr = frozen_ruby_key("REMOTE_ADDR");
  rb_key_remote_port = frozen_ruby_key("REMOTE_PORT");
  rb_key_request_method = frozen_ruby_key("REQUEST_METHOD");
  rb_key_script_name = frozen_ruby_key("SCRIPT_NAME");
  rb_key_server_name = frozen_ruby_key("SERVER_NAME");
  rb_key_server_port = frozen_ruby_key("SERVER_PORT");
  rb_key_server_protocol = frozen_ruby_key("SERVER_PROTOCOL");
}

void Vajra::rack::set_rack_execution_callback(VALUE callback)
{
  {
    const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
    rack_execution_callback = callback;
  }
  rack_execution_callback_installed_flag.store(!NIL_P(callback), std::memory_order_release);
}

void Vajra::rack::set_rack_execution_app(VALUE app)
{
  {
    const std::lock_guard<std::mutex> app_lock(rack_execution_app_mutex);
    rack_execution_app = app;
  }
  rack_execution_app_installed_flag.store(!NIL_P(app), std::memory_order_release);
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::request_channel_transport(int channel_fd)
{
  return std::make_shared<WorkerProcessRackExecutionTransport>(
      channel_fd,
      60,
      []() {});
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::request_channel_transport(
    const std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> &worker_states,
    std::size_t max_threads,
    std::size_t queue_capacity,
    std::size_t request_timeout_seconds,
    std::size_t worker_timeout_seconds,
    std::function<void(const std::shared_ptr<Vajra::runtime::SharedWorkerState> &)> worker_timeout_handler,
    bool debug_logging)
{
  return std::make_shared<GlobalQueuedWorkerProcessRackExecutionTransport>(
      worker_states,
      max_threads,
      queue_capacity,
      request_timeout_seconds,
      worker_timeout_seconds,
      std::move(worker_timeout_handler),
      debug_logging);
}

std::unique_ptr<Vajra::rack::RackExecutionSession> Vajra::rack::RackExecutionTransport::start(
    const std::vector<request::RackEnvEntry> &env_entries,
    int) const
{
  return std::make_unique<BufferedRackExecutionSession>(*this, env_entries);
}

std::string Vajra::rack::RackExecutionTransport::stats_payload_json() const
{
  return Vajra::runtime::runtime_stats_payload_json();
}

std::string Vajra::rack::RackExecutionTransport::metrics_payload_text() const
{
  return Vajra::runtime::runtime_metrics_payload_text();
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor()
    : transport_(std::make_shared<SameProcessRackExecutionTransport>())
{
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor(
    std::shared_ptr<const RackExecutionTransport> transport,
    ControlPlaneConfig control_plane_config)
    : transport_(transport ? std::move(transport) : std::make_shared<SameProcessRackExecutionTransport>()),
      control_plane_config_(std::move(control_plane_config))
{
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::control_response(
    const request::RequestContext &request_context) const
{
  const std::string path = request_path_for(request_context.request.request_line.target);
  response::Response response;
  response.status = {200, "OK"};
  response.headers.push_back({"Cache-Control", "no-store"});

  if (!control_plane_config_.stats_path.empty() &&
      path == control_plane_config_.stats_path &&
      request_context.request.request_line.method == "GET")
  {
    response.headers.push_back({"Content-Type", "application/json"});
    response.body = transport_->stats_payload_json();
    return response;
  }

  if (!control_plane_config_.metrics_endpoint.empty() &&
      path == control_plane_config_.metrics_endpoint &&
      request_context.request.request_line.method == "GET")
  {
    response.headers.push_back({"Content-Type", "text/plain; version=0.0.4"});
    response.body = transport_->metrics_payload_text();
    return response;
  }

  return std::nullopt;
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
  if (channel_fds.size() < max_threads)
  {
    throw std::runtime_error("worker request channel count must be at least max_threads");
  }

  int completion_signal_pipe[2] = {-1, -1};
  if (pipe(completion_signal_pipe) != 0)
  {
    throw std::runtime_error("worker request completion signal pipe failed");
  }

  auto runtime_state = std::make_shared<WorkerRequestLoopSharedState>(channel_fds, max_threads);
  const std::size_t total_thread_count = channel_fds.size() + max_threads;
  std::vector<WorkerLoopThreadContext *> contexts;
  contexts.reserve(total_thread_count);
  std::vector<VALUE> threads;
  threads.reserve(total_thread_count);
  std::vector<std::unique_ptr<WorkerRequestReaderThreadContext>> reader_contexts;
  reader_contexts.reserve(channel_fds.size());
  std::vector<std::unique_ptr<WorkerRequestExecutorThreadContext>> executor_contexts;
  executor_contexts.reserve(max_threads);

  for (int channel_fd : channel_fds)
  {
    auto context = std::make_unique<WorkerRequestReaderThreadContext>();
    context->channel_fd = channel_fd;
    context->completion_signal_fd = completion_signal_pipe[1];
    context->runtime_state = runtime_state;
    contexts.push_back(context.get());
    threads.push_back(rb_thread_create(run_worker_request_reader_thread, context.get()));
    reader_contexts.push_back(std::move(context));
  }

  for (std::size_t thread_index = 0; thread_index < max_threads; ++thread_index)
  {
    auto context = std::make_unique<WorkerRequestExecutorThreadContext>();
    context->completion_signal_fd = completion_signal_pipe[1];
    context->runtime_state = runtime_state;
    contexts.push_back(context.get());
    threads.push_back(rb_thread_create(run_worker_request_executor_thread, context.get()));
    executor_contexts.push_back(std::move(context));
  }

  ID id_join = rb_intern("join");
  std::vector<bool> joined(threads.size(), false);
  std::size_t joined_count = 0;

  while (joined_count < threads.size())
  {
    WorkerChannelSignalReadContext signal_read_context{completion_signal_pipe[0], false, 0};
    rb_thread_call_without_gvl(
        wait_for_worker_channel_signal_without_gvl,
        &signal_read_context,
        RUBY_UBF_IO,
        nullptr);
    if (signal_read_context.error_number != 0)
    {
      close(completion_signal_pipe[0]);
      close(completion_signal_pipe[1]);
      throw std::runtime_error("worker request completion signal wait failed");
    }
    if (signal_read_context.eof)
    {
      break;
    }

    for (std::size_t index = 0; index < threads.size(); ++index)
    {
      if (joined[index])
      {
        continue;
      }
      if (!contexts[index]->completed.load(std::memory_order_acquire))
      {
        continue;
      }

      rb_funcall(threads[index], id_join, 0);
      joined[index] = true;
      ++joined_count;
    }
  }

  close(completion_signal_pipe[0]);
  close(completion_signal_pipe[1]);

  for (WorkerLoopThreadContext *context : contexts)
  {
    if (const std::optional<std::string> error_message = take_worker_loop_thread_error(*context))
    {
      throw std::runtime_error(*error_message);
    }
  }

  if (const std::optional<std::string> error_message = runtime_state->fatal_error())
  {
    throw std::runtime_error(*error_message);
  }
}
