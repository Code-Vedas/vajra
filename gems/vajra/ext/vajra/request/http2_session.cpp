// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "http2_session.hpp"

#include "http_field_utils.hpp"
#include "rack/http2_stream.hpp"
#include "rack/native_input.hpp"
#include "response/http_header_utils.hpp"
#include "response/response_serializer.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/runtime_state.hpp"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>

namespace
{
  constexpr std::size_t kHttp2ReadBufferSize = 1024 * 1024;
  constexpr std::size_t kHttp2FrameHeaderBytes = 9;
  constexpr std::size_t kHttp2MaxHeadersFrameBytes = 16 * 1024;
  constexpr std::size_t kHttp2ClosedStreamRetention = 1024;
  constexpr std::size_t kHttp2DefaultHeaderLimit = 100;
  constexpr std::size_t kHttp2DirectBodyBytes = 64 * 1024;
  constexpr std::size_t kHttp2MaxDefaultConnectionWindowBytes = 16 * 1024 * 1024;
  constexpr std::size_t kHttp2WriteBufferBytes = 32 * 1024;
  constexpr int kHttp2IdlePollSeconds = 1;
  constexpr int kHttp2ActiveBodySleepMilliseconds = 1;
  constexpr int kHttp2PendingWorkWaitMilliseconds = 1;
  constexpr int kHttp2PriorityCoalesceWaitMilliseconds = 100;
  constexpr std::string_view kConnectionHeader = "connection";
  constexpr std::string_view kKeepAliveHeader = "keep-alive";
  constexpr std::string_view kProxyConnectionHeader = "proxy-connection";
  constexpr std::string_view kTransferEncodingHeader = "transfer-encoding";
  constexpr std::string_view kUpgradeHeader = "upgrade";
  constexpr std::uint32_t kHttp2CancelErrorCode = 0x8;

  std::string lower_ascii(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                   { return static_cast<char>(std::tolower(character)); });
    return value;
  }

  bool connection_specific_header(std::string_view name)
  {
    return name == kConnectionHeader ||
           name == kKeepAliveHeader ||
           name == kProxyConnectionHeader ||
           name == kTransferEncodingHeader ||
           name == kUpgradeHeader;
  }

  std::string bytes_to_string(const std::uint8_t *bytes, std::size_t length)
  {
    return std::string(reinterpret_cast<const char *>(bytes), length);
  }

  bool http2_header_name_is_lowercase(const std::uint8_t *bytes, std::size_t length)
  {
    for (std::size_t index = 0; index < length; ++index)
    {
      if (bytes[index] >= 'A' && bytes[index] <= 'Z')
      {
        return false;
      }
    }
    return true;
  }

  std::optional<std::size_t> parse_content_length_value(const std::string &value)
  {
    const std::string normalized = Vajra::request::strip_http_whitespace(value);
    if (normalized.empty())
    {
      return std::nullopt;
    }

    std::size_t content_length = 0;
    for (const char character : normalized)
    {
      if (character < '0' || character > '9')
      {
        return std::nullopt;
      }

      const std::size_t digit = static_cast<std::size_t>(character - '0');
      if (content_length > (std::numeric_limits<std::size_t>::max() - digit) / 10)
      {
        return std::nullopt;
      }

      content_length = content_length * 10 + digit;
    }

    return content_length;
  }

  nghttp2_nv header_nv(const std::string &name, const std::string &value)
  {
    return nghttp2_nv{
        reinterpret_cast<std::uint8_t *>(const_cast<char *>(name.data())),
        reinterpret_cast<std::uint8_t *>(const_cast<char *>(value.data())),
        name.size(),
        value.size(),
        NGHTTP2_NV_FLAG_NONE};
  }

  struct OwnedHttp2Header
  {
    std::string name;
    std::string value;
  };

  std::int64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point started_at)
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - started_at)
        .count();
  }

  std::array<std::uint8_t, 13> rst_stream_frame(std::int32_t stream_id, std::uint32_t error_code)
  {
    std::array<std::uint8_t, 13> frame{};
    frame[2] = 4;
    frame[3] = NGHTTP2_RST_STREAM;
    frame[5] = static_cast<std::uint8_t>((stream_id >> 24) & 0x7f);
    frame[6] = static_cast<std::uint8_t>((stream_id >> 16) & 0xff);
    frame[7] = static_cast<std::uint8_t>((stream_id >> 8) & 0xff);
    frame[8] = static_cast<std::uint8_t>(stream_id & 0xff);
    frame[9] = static_cast<std::uint8_t>((error_code >> 24) & 0xff);
    frame[10] = static_cast<std::uint8_t>((error_code >> 16) & 0xff);
    frame[11] = static_cast<std::uint8_t>((error_code >> 8) & 0xff);
    frame[12] = static_cast<std::uint8_t>(error_code & 0xff);
    return frame;
  }

  std::array<std::uint8_t, 17> goaway_frame(std::int32_t last_stream_id, std::uint32_t error_code)
  {
    std::array<std::uint8_t, 17> frame{};
    frame[2] = 8;
    frame[3] = NGHTTP2_GOAWAY;
    frame[9] = static_cast<std::uint8_t>((last_stream_id >> 24) & 0x7f);
    frame[10] = static_cast<std::uint8_t>((last_stream_id >> 16) & 0xff);
    frame[11] = static_cast<std::uint8_t>((last_stream_id >> 8) & 0xff);
    frame[12] = static_cast<std::uint8_t>(last_stream_id & 0xff);
    frame[13] = static_cast<std::uint8_t>((error_code >> 24) & 0xff);
    frame[14] = static_cast<std::uint8_t>((error_code >> 16) & 0xff);
    frame[15] = static_cast<std::uint8_t>((error_code >> 8) & 0xff);
    frame[16] = static_cast<std::uint8_t>(error_code & 0xff);
    return frame;
  }

  Vajra::response::Response fallback_response(int status, std::string body)
  {
    return Vajra::response::Response{
        Vajra::response::Status{status, status == 503 ? "Service Unavailable" : status == 504 ? "Gateway Timeout"
                                                                                              : "Internal Server Error"},
        {Vajra::response::Header{"Content-Type", "text/plain"}},
        std::move(body),
        Vajra::response::ConnectionBehavior::close};
  }

  class Http2ExecutionPoolImpl
  {
  public:
    explicit Http2ExecutionPoolImpl(std::size_t thread_count)
    {
      threads_.reserve(thread_count);
      for (std::size_t index = 0; index < thread_count; ++index)
      {
        threads_.emplace_back([this]()
                              { run(); });
      }
    }

    ~Http2ExecutionPoolImpl()
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
      }
      condition_.notify_all();
      for (std::thread &thread : threads_)
      {
        if (thread.joinable())
        {
          thread.join();
        }
      }
    }

    Http2ExecutionPoolImpl(const Http2ExecutionPoolImpl &) = delete;
    Http2ExecutionPoolImpl &operator=(const Http2ExecutionPoolImpl &) = delete;

    void enqueue(std::function<void()> work)
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        work_.push_back(std::move(work));
      }
      condition_.notify_one();
    }

  private:
    void run()
    {
      for (;;)
      {
        std::function<void()> work;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          condition_.wait(lock, [this]()
                          { return stopping_ || !work_.empty(); });
          if (work_.empty())
          {
            if (stopping_)
            {
              return;
            }
            continue;
          }
          work = std::move(work_.front());
          work_.pop_front();
        }
        work();
      }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> work_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
  };
}

class Vajra::request::Http2ExecutionPool::Impl final : public Http2ExecutionPoolImpl
{
public:
  explicit Impl(std::size_t thread_count)
      : Http2ExecutionPoolImpl(thread_count)
  {
  }
};

Vajra::request::Http2ExecutionPool::Http2ExecutionPool(std::size_t thread_count)
    : impl_(std::make_unique<Impl>(thread_count))
{
}

Vajra::request::Http2ExecutionPool::~Http2ExecutionPool() = default;

void Vajra::request::Http2ExecutionPool::enqueue(std::function<void()> work)
{
  impl_->enqueue(std::move(work));
}

class Vajra::request::Http2Session::Impl final
{
public:
  enum class EntryMode
  {
    client_preface,
    prior_knowledge,
    upgrade
  };

  Impl(
      Vajra::transport::Connection &connection,
      SocketContext socket_context,
      Http2Config config,
      std::shared_ptr<const RequestExecutor> request_executor,
      std::shared_ptr<Http2ExecutionPool> execution_pool,
      EntryMode entry_mode = EntryMode::client_preface,
      std::string initial_bytes = "",
      std::optional<Http2UpgradeRequest> upgrade_request = std::nullopt)
      : connection_(connection),
        socket_context_(std::move(socket_context)),
        config_(config),
        request_executor_(std::move(request_executor)),
        execution_pool_(execution_pool ? std::move(execution_pool) : std::make_shared<Http2ExecutionPool>(1)),
        entry_mode_(entry_mode),
        initial_bytes_(std::move(initial_bytes))
  {
    nghttp2_session_callbacks *raw_callbacks = nullptr;
    check(nghttp2_session_callbacks_new(&raw_callbacks), "nghttp2_session_callbacks_new");
    callbacks_.reset(raw_callbacks);

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_.get(), begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks_.get(), header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_.get(), data_chunk_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_.get(), frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_.get(), stream_close_callback);
    nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks_.get(), begin_frame_callback);

    nghttp2_option *raw_options = nullptr;
    check(nghttp2_option_new(&raw_options), "nghttp2_option_new");
    std::unique_ptr<nghttp2_option, decltype(&nghttp2_option_del)> options(raw_options, nghttp2_option_del);
    nghttp2_option_set_no_auto_window_update(options.get(), 1);
    if (entry_mode_ == EntryMode::upgrade)
    {
      nghttp2_option_set_no_recv_client_magic(options.get(), 1);
      client_preface_seen_ = true;
    }

    nghttp2_session *raw_session = nullptr;
    check(nghttp2_session_server_new2(&raw_session, callbacks_.get(), this, options.get()), "nghttp2_session_server_new2");
    session_.reset(raw_session);
    if (upgrade_request)
    {
      initialize_upgrade_request(std::move(*upgrade_request));
    }
  }

  ~Impl()
  {
    wait_for_execution_completion();
  }

  void run()
  {
    submit_settings();
    send_pending();
    submit_completed_streams();
    drain_finished_executions();
    consume_drained_request_body_bytes();
    flush_http2_stream_events();
    if (deliver_paused_request_body_chunks() &&
        !pending_receive_bytes_.empty() &&
        !receive_pending_bytes())
    {
      return;
    }
    send_pending();
    if (!initial_bytes_.empty())
    {
      const std::string buffered = std::move(initial_bytes_);
      if (!receive_bytes(
              reinterpret_cast<const std::uint8_t *>(buffered.data()),
              buffered.size()))
      {
        return;
      }
      submit_completed_streams();
      drain_finished_executions();
      consume_drained_request_body_bytes();
      flush_http2_stream_events();
      if (deliver_paused_request_body_chunks() &&
          !pending_receive_bytes_.empty() &&
          !receive_pending_bytes())
      {
        return;
      }
      send_pending();
    }

    std::vector<std::uint8_t> buffer(kHttp2ReadBufferSize);
    for (;;)
    {
      if (nghttp2_session_want_write(session_.get()) != 0)
      {
        send_pending();
      }

      submit_completed_streams();
      drain_finished_executions();
      consume_drained_request_body_bytes();
      flush_http2_stream_events();
      if (deliver_paused_request_body_chunks() &&
          !pending_receive_bytes_.empty() &&
          !receive_pending_bytes())
      {
        return;
      }
      if (nghttp2_session_want_write(session_.get()) != 0)
      {
        send_pending();
      }

      const bool wants_read = nghttp2_session_want_read(session_.get()) != 0;
      const bool wants_write = nghttp2_session_want_write(session_.get()) != 0;
      const bool has_work = has_pending_executions() || has_finished_executions();
      const bool has_active_body_flow_control = has_active_request_body_flow_control();

      if (!wants_read)
      {
        if (peer_goaway_received_)
        {
          if (connection_.fd() >= 0 && connection_.wait_readable(0))
          {
            if (!receive_once(buffer))
            {
              return;
            }
            while (connection_.wait_readable(0))
            {
              if (!receive_once(buffer))
              {
                return;
              }
            }
            continue;
          }
          return;
        }
        if (wants_write || has_work)
        {
          if (has_work)
          {
            wait_for_finished_execution();
          }
          else
          {
            std::this_thread::yield();
          }
          continue;
        }
        return;
      }

      const int poll_timeout_seconds = (has_work || has_active_body_flow_control) ? 0 : kHttp2IdlePollSeconds;
      if (!connection_.wait_readable(poll_timeout_seconds))
      {
        if (has_work)
        {
          wait_for_finished_execution();
          continue;
        }
        if (has_active_body_flow_control)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(kHttp2ActiveBodySleepMilliseconds));
          continue;
        }
        if (connection_.fd() < 0)
        {
          return;
        }
        continue;
      }

      if (!receive_once(buffer))
      {
        return;
      }
      while (connection_.wait_readable(0))
      {
        if (!receive_once(buffer))
        {
          return;
        }
      }
      submit_completed_streams();
      drain_finished_executions();
      consume_drained_request_body_bytes();
      flush_http2_stream_events();
      send_pending();
    }
  }

private:
  struct CallbackDeleter
  {
    void operator()(nghttp2_session_callbacks *callbacks) const
    {
      nghttp2_session_callbacks_del(callbacks);
    }
  };

  struct SessionDeleter
  {
    void operator()(nghttp2_session *session) const
    {
      nghttp2_session_del(session);
    }
  };

  struct StreamState
  {
    bool pseudo_complete = false;
    bool invalid = false;
    bool executed = false;
    bool execution_start_failed = false;
    bool queue_capacity_rejected = false;
    bool method_seen = false;
    bool path_seen = false;
    bool scheme_seen = false;
    bool authority_seen = false;
    bool protocol_seen = false;
    bool priority_seen = false;
    bool priority_exclusive = false;
    std::string method;
    std::string path;
    std::string scheme;
    std::string authority;
    std::string protocol;
    std::int32_t priority_dependency = 0;
    std::int32_t priority_weight = 16;
    std::vector<ParsedHeader> headers;
    std::size_t request_body_bytes = 0;
    std::string request_body;
    std::deque<std::string> paused_request_body_chunks;
    std::size_t buffered_request_body_bytes_to_consume = 0;
    std::unique_ptr<RequestExecutionSession> execution_session;
    std::shared_ptr<Vajra::rack::NativeInputState> input_state_owner;
    std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream_state;
    std::size_t header_count = 0;
    std::size_t header_bytes = 0;
    Vajra::response::Response response{
        Vajra::response::Status{200, "OK"},
        {},
        "",
        Vajra::response::ConnectionBehavior::close};
    std::vector<std::string> response_body_chunks;
    std::shared_ptr<Vajra::response::ResponseBodyFile> response_body_file;
    std::size_t response_body_size = 0;
    std::size_t response_chunk_index = 0;
    std::size_t response_chunk_offset = 0;
    std::size_t response_body_file_offset = 0;
    bool tunnel_accepted = false;
    bool tunnel_data_provider_active = false;
    bool tunnel_end_stream_queued = false;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  };

  struct ContentLengthState
  {
    bool present = false;
    bool valid = true;
    std::size_t value = 0;
  };

  struct ExecutionTask
  {
    explicit ExecutionTask(
        std::int32_t id,
        RequestContext context,
        std::unique_ptr<RequestExecutionSession> session,
        std::chrono::steady_clock::time_point started)
        : stream_id(id),
          request_context(std::move(context)),
          execution_session(std::move(session)),
          started_at(started),
          enqueued_at(std::chrono::steady_clock::now())
    {
    }

    std::int32_t stream_id;
    RequestContext request_context;
    std::unique_ptr<RequestExecutionSession> execution_session;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point enqueued_at;
    std::int64_t queue_wait_nanoseconds = 0;
    bool queue_capacity_rejected = false;
    Vajra::response::Response response{
        Vajra::response::Status{200, "OK"},
        {},
        "",
        Vajra::response::ConnectionBehavior::close};
  };

  struct PriorityNode
  {
    std::int32_t parent = 0;
    std::vector<std::int32_t> children;
    std::int32_t weight = 16;
    std::uint64_t sent_bytes = 0;
    bool exclusive = false;
    bool seen = false;
    bool closed = false;
  };

  struct PriorityPathElement
  {
    std::int32_t stream_id = 0;
    std::int32_t weight = 16;
    std::uint64_t sent_bytes = 0;
  };

  static Impl *from_session(nghttp2_session *session)
  {
    return static_cast<Impl *>(nghttp2_session_get_stream_user_data(session, 0));
  }

  static Impl *from_user_data(void *user_data)
  {
    return static_cast<Impl *>(user_data);
  }

  static int begin_headers_callback(
      nghttp2_session *,
      const nghttp2_frame *frame,
      void *user_data)
  {
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
      return 0;
    }

    Impl *impl = from_user_data(user_data);
    if (!impl->accept_request_stream(frame->hd.stream_id))
    {
      return 0;
    }

    StreamState stream;
    impl->streams_[frame->hd.stream_id] = std::move(stream);
    return 0;
  }

  static int header_callback(
      nghttp2_session *,
      const nghttp2_frame *frame,
      const std::uint8_t *name,
      std::size_t name_length,
      const std::uint8_t *value,
      std::size_t value_length,
      std::uint8_t,
      void *user_data)
  {
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
      return 0;
    }

    Impl *impl = from_user_data(user_data);
    if (impl->rejected_stream_ids_.find(frame->hd.stream_id) != impl->rejected_stream_ids_.end())
    {
      return 0;
    }

    StreamState &stream = impl->stream_for(frame->hd.stream_id);
    ++stream.header_count;
    stream.header_bytes += name_length + value_length;
    if (stream.header_count > kHttp2DefaultHeaderLimit ||
        stream.header_bytes > impl->config_.max_request_head_bytes)
    {
      stream.invalid = true;
      impl->reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }

    if (!http2_header_name_is_lowercase(name, name_length))
    {
      stream.invalid = true;
      impl->reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }

    const std::string header_name = bytes_to_string(name, name_length);
    const std::string header_value = bytes_to_string(value, value_length);
    impl->record_header(stream, header_name, header_value);
    return 0;
  }

  static int data_chunk_callback(
      nghttp2_session *,
      std::uint8_t,
      std::int32_t stream_id,
      const std::uint8_t *data,
      std::size_t length,
      void *user_data)
  {
    Impl *impl = from_user_data(user_data);
    if (impl->closed_stream_ids_.find(stream_id) != impl->closed_stream_ids_.end())
    {
      impl->write_rst_stream(stream_id, NGHTTP2_STREAM_CLOSED);
      return 0;
    }

    StreamState &stream = impl->stream_for(stream_id);
    if (stream.http2_stream_state)
    {
      if (!Vajra::rack::http2_stream_try_append_inbound(
              stream.http2_stream_state.get(),
              reinterpret_cast<const char *>(data),
              length))
      {
        return NGHTTP2_ERR_PAUSE;
      }
      return 0;
    }

    if (!stream.executed && !impl->can_defer_http2_body_execution(stream))
    {
      impl->start_streaming_execution_session(stream_id, stream);
    }
    if (stream.request_body_bytes > impl->config_.max_request_body_bytes ||
        length > impl->config_.max_request_body_bytes - stream.request_body_bytes)
    {
      stream.invalid = true;
      if (!stream.executed)
      {
        impl->start_streaming_execution_session(stream_id, stream);
      }
      if (stream.execution_session)
      {
        stream.execution_session->fail_request_body("request body exceeded configured limit");
        stream.execution_session.reset();
      }
      impl->reset_stream(stream_id, NGHTTP2_REFUSED_STREAM);
      return 0;
    }
    stream.request_body_bytes += length;
    const ContentLengthState content_length = impl->declared_content_length_state(stream);
    if (!content_length.valid ||
        (content_length.present && stream.request_body_bytes > content_length.value))
    {
      stream.invalid = true;
      if (stream.execution_session)
      {
        stream.execution_session->fail_request_body("HTTP/2 request body length mismatch");
        stream.execution_session.reset();
      }
      impl->reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }
    if (stream.execution_session)
    {
      try
      {
        if (stream.input_state_owner &&
            Vajra::rack::native_input_closed(stream.input_state_owner.get()))
        {
          stream.buffered_request_body_bytes_to_consume += length;
          return 0;
        }
        if (!stream.execution_session->try_append_request_body_bytes(
                reinterpret_cast<const char *>(data),
                length))
        {
          if (stream.input_state_owner &&
              Vajra::rack::native_input_closed(stream.input_state_owner.get()))
          {
            stream.buffered_request_body_bytes_to_consume += length;
            return 0;
          }
          stream.paused_request_body_chunks.emplace_back(
              reinterpret_cast<const char *>(data),
              length);
          return NGHTTP2_ERR_PAUSE;
        }
      }
      catch (...)
      {
        stream.invalid = true;
        stream.execution_session.reset();
        impl->reset_stream(stream_id, NGHTTP2_REFUSED_STREAM);
      }
    }
    else
    {
      stream.request_body.append(reinterpret_cast<const char *>(data), length);
      if (impl->direct_body_flow_control_can_be_released(stream))
      {
        stream.buffered_request_body_bytes_to_consume += length;
      }
      else if (stream.request_body.size() > kHttp2DirectBodyBytes)
      {
        try
        {
          impl->start_streaming_execution_session(stream_id, stream);
        }
        catch (...)
        {
          stream.invalid = true;
          stream.execution_session.reset();
          impl->reset_stream(stream_id, NGHTTP2_REFUSED_STREAM);
        }
      }
    }
    return 0;
  }

  static int frame_recv_callback(
      nghttp2_session *,
      const nghttp2_frame *frame,
      void *user_data)
  {
    Impl *impl = from_user_data(user_data);
    if (frame->hd.type == NGHTTP2_WINDOW_UPDATE || frame->hd.type == NGHTTP2_SETTINGS)
    {
      impl->resume_deferred_data_pending_ = true;
    }

    if (frame->hd.type == NGHTTP2_GOAWAY && frame->hd.stream_id == 0)
    {
      impl->peer_goaway_received_ = true;
      return 0;
    }

    if (frame->hd.stream_id == 0)
    {
      return 0;
    }

    if (frame->hd.type == NGHTTP2_PRIORITY)
    {
      if (frame->priority.pri_spec.stream_id == frame->hd.stream_id)
      {
        impl->reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
      }
      else
      {
        impl->record_priority(frame->hd.stream_id, frame->priority.pri_spec);
      }
      return 0;
    }

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
    {
      if (impl->rejected_stream_ids_.find(frame->hd.stream_id) != impl->rejected_stream_ids_.end())
      {
        return 0;
      }

      StreamState &stream = impl->stream_for(frame->hd.stream_id);
      if (stream.invalid)
      {
        impl->reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
      }
      else
      {
        if ((frame->hd.flags & NGHTTP2_FLAG_PRIORITY) != 0)
        {
          impl->record_priority(frame->hd.stream_id, frame->headers.pri_spec);
        }
        if (impl->extended_connect_request(stream))
        {
          impl->start_http2_stream_execution(frame->hd.stream_id, stream);
          return 0;
        }
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0)
        {
          impl->complete_stream(frame->hd.stream_id);
        }
        else if (!impl->can_defer_http2_body_execution(stream))
        {
          impl->start_streaming_execution_session(frame->hd.stream_id, stream);
        }
        else
        {
          impl->reserve_deferred_body_capacity(stream);
        }
      }
    }
    else if (frame->hd.type == NGHTTP2_HEADERS && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0)
    {
      impl->complete_stream(frame->hd.stream_id);
    }
    else if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0)
    {
      StreamState &stream = impl->stream_for(frame->hd.stream_id);
      if (stream.invalid)
      {
        impl->reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
      }
      else if (stream.http2_stream_state)
      {
        Vajra::rack::http2_stream_finish_inbound(stream.http2_stream_state.get());
      }
      else
      {
        impl->complete_stream(frame->hd.stream_id);
      }
    }
    return 0;
  }

  static int begin_frame_callback(
      nghttp2_session *,
      const nghttp2_frame_hd *frame_header,
      void *user_data)
  {
    Impl *impl = from_user_data(user_data);
    if ((frame_header->type == NGHTTP2_DATA || frame_header->type == NGHTTP2_HEADERS) &&
        impl->closed_stream_ids_.find(frame_header->stream_id) != impl->closed_stream_ids_.end())
    {
      impl->write_rst_stream(frame_header->stream_id, NGHTTP2_STREAM_CLOSED);
    }
    return 0;
  }

  static int stream_close_callback(
      nghttp2_session *,
      std::int32_t stream_id,
      std::uint32_t error_code,
      void *user_data)
  {
    Impl *impl = from_user_data(user_data);
    auto stream = impl->streams_.find(stream_id);
    if (stream != impl->streams_.end() && stream->second.http2_stream_state)
    {
      if (error_code == NGHTTP2_NO_ERROR)
      {
        Vajra::rack::http2_stream_finish_inbound(stream->second.http2_stream_state.get());
      }
      else
      {
        Vajra::rack::http2_stream_reset(stream->second.http2_stream_state.get(), error_code);
      }
    }
    impl->remember_closed_stream(stream_id);
    impl->streams_.erase(stream_id);
    impl->rejected_stream_ids_.erase(stream_id);
    impl->completed_stream_ids_.erase(
        std::remove(impl->completed_stream_ids_.begin(), impl->completed_stream_ids_.end(), stream_id),
        impl->completed_stream_ids_.end());
    return 0;
  }

  static ssize_t data_read_callback(
      nghttp2_session *,
      std::int32_t stream_id,
      std::uint8_t *buffer,
      std::size_t length,
      std::uint32_t *data_flags,
      nghttp2_data_source *,
      void *user_data)
  {
    Impl *impl = from_user_data(user_data);
    StreamState &stream = impl->stream_for(stream_id);
    if (impl->should_defer_for_higher_priority(stream_id))
    {
      return NGHTTP2_ERR_DEFERRED;
    }
    if (stream.http2_stream_state && stream.tunnel_accepted)
    {
      length = impl->outbound_window_capacity(stream_id, length);
      if (length == 0)
      {
        stream.tunnel_data_provider_active = true;
        return NGHTTP2_ERR_DEFERRED;
      }

      const std::size_t copied = Vajra::rack::http2_stream_drain_outbound(
          stream.http2_stream_state.get(),
          buffer,
          length);
      const bool has_more = Vajra::rack::http2_stream_has_outbound(stream.http2_stream_state.get());
      const bool app_closed = stream.tunnel_end_stream_queued ||
                              Vajra::rack::http2_stream_app_closed(stream.http2_stream_state.get());
      if (copied == 0)
      {
        if (app_closed)
        {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          stream.tunnel_data_provider_active = false;
          impl->resume_priority_deferred_streams();
          return 0;
        }
        stream.tunnel_data_provider_active = true;
        return NGHTTP2_ERR_DEFERRED;
      }
      impl->note_priority_bytes_sent(stream_id, copied);
      if (!has_more && app_closed)
      {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        stream.tunnel_data_provider_active = false;
        impl->resume_priority_deferred_streams();
      }
      else
      {
        stream.tunnel_data_provider_active = true;
      }
      return static_cast<ssize_t>(copied);
    }

    const bool has_remaining = stream_has_remaining_response_body(stream);
    if (has_remaining)
    {
      length = impl->outbound_window_capacity(stream_id, length);
    }
    if (has_remaining && length == 0)
    {
      return NGHTTP2_ERR_DEFERRED;
    }

    std::size_t copied = 0;
    while (copied < length && stream.response_chunk_index < stream.response_body_chunks.size())
    {
      const std::string &chunk = stream.response_body_chunks[stream.response_chunk_index];
      const std::size_t available = chunk.size() - stream.response_chunk_offset;
      const std::size_t chunk_copied = std::min(length - copied, available);
      if (chunk_copied > 0)
      {
        std::memcpy(buffer + copied, chunk.data() + stream.response_chunk_offset, chunk_copied);
        stream.response_chunk_offset += chunk_copied;
        copied += chunk_copied;
      }
      if (stream.response_chunk_offset >= chunk.size())
      {
        ++stream.response_chunk_index;
        stream.response_chunk_offset = 0;
      }
    }
    if (copied < length && stream.response_chunk_index >= stream.response_body_chunks.size() &&
        stream.response_body_file && stream.response_body_file_offset < stream.response_body_size)
    {
      const std::size_t remaining_file_bytes = stream.response_body_size - stream.response_body_file_offset;
      const std::size_t file_read_size = std::min(length - copied, remaining_file_bytes);
      if (std::fseek(stream.response_body_file->file, static_cast<long>(stream.response_body_file_offset), SEEK_SET) != 0)
      {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      const std::size_t read = std::fread(buffer + copied, 1, file_read_size, stream.response_body_file->file);
      if (read == 0 && file_read_size > 0)
      {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      stream.response_body_file_offset += read;
      copied += read;
      if (read < file_read_size && std::ferror(stream.response_body_file->file) != 0)
      {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
    }
    const bool body_complete =
        stream.response_chunk_index >= stream.response_body_chunks.size() &&
        (!stream.response_body_file || stream.response_body_file_offset >= stream.response_body_size);
    if (body_complete)
    {
      if (stream.http2_stream_state && stream.tunnel_accepted && !stream.tunnel_end_stream_queued)
      {
        stream.tunnel_data_provider_active = true;
        return NGHTTP2_ERR_DEFERRED;
      }
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      if (stream.http2_stream_state)
      {
        stream.tunnel_data_provider_active = false;
      }
      impl->resume_priority_deferred_streams();
    }
    if (copied > 0)
    {
      impl->note_priority_bytes_sent(stream_id, copied);
    }
    return static_cast<ssize_t>(copied);
  }

  void submit_settings()
  {
    std::array<nghttp2_settings_entry, 5> settings{{
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, static_cast<std::uint32_t>(config_.max_concurrent_streams)},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, static_cast<std::uint32_t>(config_.initial_window_size)},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, static_cast<std::uint32_t>(config_.max_frame_size)},
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, static_cast<std::uint32_t>(config_.header_table_size)},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1},
    }};
    check(
        nghttp2_submit_settings(session_.get(), NGHTTP2_FLAG_NONE, settings.data(), settings.size()),
        "nghttp2_submit_settings");
    const std::size_t connection_window_size = receive_connection_window_size();
    if (connection_window_size > NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE)
    {
      const std::size_t increment = connection_window_size - NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE;
      check(
          nghttp2_submit_window_update(
              session_.get(),
              NGHTTP2_FLAG_NONE,
              0,
              static_cast<std::int32_t>(increment)),
          "nghttp2_submit_window_update");
    }
  }

  std::size_t receive_connection_window_size() const
  {
    if (config_.initial_window_size <= NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE)
    {
      return config_.initial_window_size;
    }
    const std::size_t body_limited_window = std::min(
        config_.max_request_body_bytes,
        kHttp2MaxDefaultConnectionWindowBytes);
    return std::max(config_.initial_window_size, body_limited_window);
  }

  void send_pending()
  {
    const auto started_at = std::chrono::steady_clock::now();
    const auto write_started_at = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> write_buffer;
    write_buffer.reserve(kHttp2WriteBufferBytes);
    bool wrote_bytes = false;

    auto flush_buffer = [&]()
    {
      if (!write_buffer.empty())
      {
        write_serialized_bytes(write_buffer.data(), write_buffer.size());
        wrote_bytes = true;
        write_buffer.clear();
      }
    };

    for (;;)
    {
      const std::uint8_t *data = nullptr;
      const nghttp2_ssize length = nghttp2_session_mem_send2(session_.get(), &data);
      if (length < 0)
      {
        flush_buffer();
        check(static_cast<int>(length), "nghttp2_session_mem_send2");
      }
      if (length == 0)
      {
        break;
      }

      const std::size_t byte_count = static_cast<std::size_t>(length);
      if (byte_count >= kHttp2WriteBufferBytes)
      {
        flush_buffer();
        write_serialized_bytes(data, byte_count);
        wrote_bytes = true;
        continue;
      }
      if (write_buffer.size() + byte_count > kHttp2WriteBufferBytes)
      {
        flush_buffer();
      }
      write_buffer.insert(write_buffer.end(), data, data + byte_count);
    }

    flush_buffer();
    if (wrote_bytes)
    {
      Vajra::runtime::note_worker_response_write_time(elapsed_nanoseconds(write_started_at));
    }
    Vajra::runtime::note_worker_http2_session_send_time(elapsed_nanoseconds(started_at));
  }

  void write_serialized_bytes(const std::uint8_t *data, std::size_t length)
  {
    std::size_t written = 0;
    while (written < length)
    {
      const ssize_t result = connection_.write(
          reinterpret_cast<const char *>(data + written),
          length - written);
      if (result <= 0)
      {
        throw std::runtime_error("HTTP/2 serialized write failed");
      }
      written += static_cast<std::size_t>(result);
    }
  }

  bool receive_once(std::vector<std::uint8_t> &buffer)
  {
    const auto receive_started_at = std::chrono::steady_clock::now();
    const ssize_t read_bytes = connection_.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    Vajra::runtime::note_worker_http2_receive_time(elapsed_nanoseconds(receive_started_at));
    if (read_bytes == 0)
    {
      return false;
    }
    if (read_bytes < 0)
    {
      if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)
      {
        return false;
      }
      throw std::runtime_error("HTTP/2 connection read failed");
    }

    return receive_bytes(buffer.data(), static_cast<std::size_t>(read_bytes));
  }

  bool receive_bytes(const std::uint8_t *data, std::size_t length)
  {
    const auto precheck_started_at = std::chrono::steady_clock::now();
    const bool precheck_failed = precheck_frames(data, length);
    Vajra::runtime::note_worker_http2_frame_precheck_time(elapsed_nanoseconds(precheck_started_at));
    if (precheck_failed)
    {
      return false;
    }

    const auto nghttp2_recv_started_at = std::chrono::steady_clock::now();
    const ssize_t consumed = nghttp2_session_mem_recv(
        session_.get(),
        data,
        length);
    Vajra::runtime::note_worker_http2_nghttp2_recv_time(elapsed_nanoseconds(nghttp2_recv_started_at));
    if (consumed < 0)
    {
      send_pending();
      throw std::runtime_error("nghttp2 receive failed: " + std::string(nghttp2_strerror(static_cast<int>(consumed))));
    }
    if (static_cast<std::size_t>(consumed) != length)
    {
      pending_receive_bytes_.assign(
          data + static_cast<std::size_t>(consumed),
          data + length);
      return true;
    }
    if (resume_deferred_data_pending_)
    {
      resume_deferred_data_pending_ = false;
      resume_deferred_data();
    }
    return true;
  }

  bool receive_pending_bytes()
  {
    while (!pending_receive_bytes_.empty())
    {
      const auto nghttp2_recv_started_at = std::chrono::steady_clock::now();
      const ssize_t consumed = nghttp2_session_mem_recv(
          session_.get(),
          pending_receive_bytes_.data(),
          pending_receive_bytes_.size());
      Vajra::runtime::note_worker_http2_nghttp2_recv_time(elapsed_nanoseconds(nghttp2_recv_started_at));
      if (consumed < 0)
      {
        send_pending();
        throw std::runtime_error("nghttp2 receive failed: " + std::string(nghttp2_strerror(static_cast<int>(consumed))));
      }
      if (static_cast<std::size_t>(consumed) < pending_receive_bytes_.size())
      {
        pending_receive_bytes_.erase(
            pending_receive_bytes_.begin(),
            pending_receive_bytes_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return true;
      }
      pending_receive_bytes_.clear();
    }
    return true;
  }

  bool deliver_paused_request_body_chunks()
  {
    for (auto &entry : streams_)
    {
      StreamState &stream = entry.second;
      while (!stream.paused_request_body_chunks.empty())
      {
        if (!stream.execution_session)
        {
          stream.paused_request_body_chunks.clear();
          break;
        }

        const std::string &chunk = stream.paused_request_body_chunks.front();
        try
        {
          if (stream.input_state_owner &&
              Vajra::rack::native_input_closed(stream.input_state_owner.get()))
          {
            stream.buffered_request_body_bytes_to_consume += chunk.size();
            stream.paused_request_body_chunks.pop_front();
            continue;
          }

          if (!stream.execution_session->try_append_request_body_bytes(chunk.data(), chunk.size()))
          {
            return false;
          }
          stream.buffered_request_body_bytes_to_consume += chunk.size();
          stream.paused_request_body_chunks.pop_front();
        }
        catch (...)
        {
          stream.invalid = true;
          stream.execution_session.reset();
          stream.paused_request_body_chunks.clear();
          reset_stream(entry.first, NGHTTP2_REFUSED_STREAM);
          break;
        }
      }
    }
    return true;
  }

  void initialize_upgrade_request(Http2UpgradeRequest upgrade_request)
  {
    const int head_request = Vajra::request::ascii_case_insensitive_equal(
                                 upgrade_request.request_context.request.request_line.method,
                                 "HEAD")
                                 ? 1
                                 : 0;
    check(
        nghttp2_session_upgrade2(
            session_.get(),
            upgrade_request.settings_payload.empty() ? nullptr : upgrade_request.settings_payload.data(),
            upgrade_request.settings_payload.size(),
            head_request,
            nullptr),
        "nghttp2_session_upgrade2");

    StreamState &stream = streams_[1];
    stream.started_at = std::chrono::steady_clock::now();
    stream.pseudo_complete = true;
    stream.method_seen = true;
    stream.path_seen = true;
    stream.scheme_seen = true;
    stream.method = upgrade_request.request_context.request.request_line.method;
    stream.path = upgrade_request.request_context.request.request_line.target;
    stream.scheme = upgrade_request.request_context.socket.scheme.empty() ? "http" : upgrade_request.request_context.socket.scheme;

    for (const ParsedHeader &header : upgrade_request.request_context.request.headers)
    {
      if (Vajra::request::ascii_case_insensitive_equal(header.name, "connection") ||
          Vajra::request::ascii_case_insensitive_equal(header.name, "upgrade") ||
          Vajra::request::ascii_case_insensitive_equal(header.name, "http2-settings") ||
          Vajra::request::ascii_case_insensitive_equal(header.name, "keep-alive") ||
          Vajra::request::ascii_case_insensitive_equal(header.name, "proxy-connection") ||
          Vajra::request::ascii_case_insensitive_equal(header.name, "transfer-encoding"))
      {
        continue;
      }
      if (Vajra::request::ascii_case_insensitive_equal(header.name, "host"))
      {
        stream.authority_seen = true;
        stream.authority = header.value;
      }
      stream.headers.push_back(header);
    }

    last_request_stream_id_ = 1;
    highest_observed_request_stream_id_ = 1;
    initial_bytes_ = std::move(upgrade_request.trailing_bytes);
    complete_stream(1);
  }

  StreamState &stream_for(std::int32_t stream_id)
  {
    return streams_[stream_id];
  }

  bool precheck_frames(const std::uint8_t *data, std::size_t length)
  {
    std::size_t offset = consume_prechecked_payload(data, length);
    if (offset >= length)
    {
      return false;
    }

    if (client_preface_seen_ && precheck_buffer_.empty())
    {
      return precheck_complete_frames(data + offset, length - offset);
    }

    precheck_buffer_.insert(precheck_buffer_.end(), data + offset, data + length);
    offset = 0;
    constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (!client_preface_seen_)
    {
      if (precheck_buffer_.size() < preface.size())
      {
        return false;
      }
      if (std::memcmp(precheck_buffer_.data(), preface.data(), preface.size()) != 0)
      {
        write_goaway(NGHTTP2_PROTOCOL_ERROR);
        precheck_buffer_.clear();
        return true;
      }
      client_preface_seen_ = true;
      offset = preface.size();
    }

    std::vector<std::pair<std::int32_t, std::int64_t>> stream_window_deltas;
    while (offset + kHttp2FrameHeaderBytes <= precheck_buffer_.size())
    {
      const std::uint8_t *frame = precheck_buffer_.data() + offset;
      const std::size_t frame_length = frame_payload_length(frame);
      const std::uint8_t frame_type = frame[3];
      const std::uint8_t frame_flags = frame[4];
      const std::int32_t stream_id = frame_stream_id(frame);
      if (precheck_frame_header(frame_type, stream_id, frame_length))
      {
        precheck_buffer_.clear();
        return true;
      }
      if (offset + kHttp2FrameHeaderBytes + frame_length > precheck_buffer_.size())
      {
        if (!precheck_frame_needs_payload(frame_type, frame_flags))
        {
          const std::size_t available_payload = precheck_buffer_.size() - offset - kHttp2FrameHeaderBytes;
          precheck_skip_payload_bytes_ = frame_length - available_payload;
          precheck_buffer_.clear();
        }
        break;
      }

      if (precheck_frame_payload(frame_type, stream_id, frame_flags, frame + kHttp2FrameHeaderBytes, frame_length, stream_window_deltas))
      {
        precheck_buffer_.clear();
        return true;
      }
      offset += kHttp2FrameHeaderBytes + frame_length;
    }

    if (offset > 0)
    {
      precheck_buffer_.erase(precheck_buffer_.begin(), precheck_buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return false;
  }

  bool precheck_complete_frames(const std::uint8_t *data, std::size_t length)
  {
    std::size_t offset = 0;
    std::vector<std::pair<std::int32_t, std::int64_t>> stream_window_deltas;
    while (offset + kHttp2FrameHeaderBytes <= length)
    {
      const std::uint8_t *frame = data + offset;
      const std::size_t frame_length = frame_payload_length(frame);
      const std::uint8_t frame_type = frame[3];
      const std::uint8_t frame_flags = frame[4];
      const std::int32_t stream_id = frame_stream_id(frame);
      if (precheck_frame_header(frame_type, stream_id, frame_length))
      {
        return true;
      }
      if (offset + kHttp2FrameHeaderBytes + frame_length > length)
      {
        if (precheck_frame_needs_payload(frame_type, frame_flags))
        {
          precheck_buffer_.assign(frame, data + length);
        }
        else
        {
          const std::size_t available_payload = length - offset - kHttp2FrameHeaderBytes;
          precheck_skip_payload_bytes_ = frame_length - available_payload;
        }
        return false;
      }

      if (precheck_frame_payload(frame_type, stream_id, frame_flags, frame + kHttp2FrameHeaderBytes, frame_length, stream_window_deltas))
      {
        return true;
      }
      offset += kHttp2FrameHeaderBytes + frame_length;
    }

    if (offset < length)
    {
      precheck_buffer_.assign(data + offset, data + length);
    }
    return false;
  }

  std::size_t consume_prechecked_payload(const std::uint8_t *, std::size_t length)
  {
    if (precheck_skip_payload_bytes_ == 0)
    {
      return 0;
    }
    const std::size_t consumed = std::min(precheck_skip_payload_bytes_, length);
    precheck_skip_payload_bytes_ -= consumed;
    return consumed;
  }

  std::size_t frame_payload_length(const std::uint8_t *frame) const
  {
    return (static_cast<std::size_t>(frame[0]) << 16) |
           (static_cast<std::size_t>(frame[1]) << 8) |
           static_cast<std::size_t>(frame[2]);
  }

  std::int32_t frame_stream_id(const std::uint8_t *frame) const
  {
    return (static_cast<std::int32_t>(frame[5] & 0x7f) << 24) |
           (static_cast<std::int32_t>(frame[6]) << 16) |
           (static_cast<std::int32_t>(frame[7]) << 8) |
           static_cast<std::int32_t>(frame[8]);
  }

  bool precheck_frame_header(
      std::uint8_t frame_type,
      std::int32_t stream_id,
      std::size_t frame_length)
  {
    if (frame_type == NGHTTP2_HEADERS && frame_length > kHttp2MaxHeadersFrameBytes)
    {
      write_goaway(NGHTTP2_FRAME_SIZE_ERROR);
      return true;
    }
    if (frame_length > config_.max_frame_size)
    {
      write_goaway(NGHTTP2_FRAME_SIZE_ERROR);
      return true;
    }
    if (frame_type == NGHTTP2_RST_STREAM && stream_id > 0)
    {
      remember_closed_stream(stream_id);
      return false;
    }
    if ((frame_type == NGHTTP2_DATA || frame_type == NGHTTP2_HEADERS) &&
        closed_stream_ids_.find(stream_id) != closed_stream_ids_.end())
    {
      write_goaway(NGHTTP2_STREAM_CLOSED);
      return true;
    }
    if (frame_type == NGHTTP2_HEADERS && stream_id > 0 && (stream_id & 1) == 1)
    {
      if (stream_id < highest_observed_request_stream_id_)
      {
        write_goaway(NGHTTP2_PROTOCOL_ERROR);
        return true;
      }
      highest_observed_request_stream_id_ = stream_id;
    }
    return false;
  }

  bool precheck_frame_needs_payload(std::uint8_t frame_type, std::uint8_t frame_flags) const
  {
    if (frame_type == NGHTTP2_PRIORITY || frame_type == NGHTTP2_WINDOW_UPDATE)
    {
      return true;
    }
    if (frame_type == NGHTTP2_HEADERS && (frame_flags & NGHTTP2_FLAG_PRIORITY) != 0)
    {
      return true;
    }
    return false;
  }

  bool precheck_frame_payload(
      std::uint8_t frame_type,
      std::int32_t stream_id,
      std::uint8_t frame_flags,
      const std::uint8_t *payload,
      std::size_t frame_length,
      std::vector<std::pair<std::int32_t, std::int64_t>> &stream_window_deltas)
  {
    if (frame_type == NGHTTP2_PRIORITY)
    {
      return precheck_priority_frame(stream_id, payload, frame_length);
    }
    if (frame_type == NGHTTP2_WINDOW_UPDATE && stream_id > 0 && frame_length == 4)
    {
      return precheck_window_update_frame(stream_id, payload, stream_window_deltas);
    }
    if (frame_type == NGHTTP2_HEADERS && (frame_flags & NGHTTP2_FLAG_PRIORITY) != 0 && stream_id > 0 && frame_length >= 5)
    {
      return precheck_priority_payload(stream_id, payload);
    }
    return false;
  }

  bool precheck_priority_frame(std::int32_t stream_id, const std::uint8_t *payload, std::size_t frame_length)
  {
    if (stream_id == 0)
    {
      write_goaway(NGHTTP2_PROTOCOL_ERROR);
      return true;
    }
    if (frame_length != 5)
    {
      return false;
    }

    return precheck_priority_payload(stream_id, payload);
  }

  bool precheck_priority_payload(std::int32_t stream_id, const std::uint8_t *payload)
  {
    const std::int32_t dependency =
        (static_cast<std::int32_t>(payload[0] & 0x7f) << 24) |
        (static_cast<std::int32_t>(payload[1]) << 16) |
        (static_cast<std::int32_t>(payload[2]) << 8) |
        static_cast<std::int32_t>(payload[3]);
    const std::int32_t weight = static_cast<std::int32_t>(payload[4]) + 1;
    const bool exclusive = (payload[0] & 0x80) != 0;
    if (!apply_priority(stream_id, dependency, weight, exclusive))
    {
      write_rst_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return true;
    }
    return false;
  }

  bool precheck_window_update_frame(
      std::int32_t stream_id,
      const std::uint8_t *payload,
      std::vector<std::pair<std::int32_t, std::int64_t>> &stream_window_deltas)
  {
    const std::int64_t increment =
        (static_cast<std::uint32_t>(payload[0] & 0x7f) << 24) |
        (static_cast<std::uint32_t>(payload[1]) << 16) |
        (static_cast<std::uint32_t>(payload[2]) << 8) |
        static_cast<std::uint32_t>(payload[3]);
    std::int64_t &delta = precheck_stream_window_delta(stream_id, stream_window_deltas);
    const std::int64_t window = static_cast<std::int64_t>(
                                    nghttp2_session_get_stream_remote_window_size(session_.get(), stream_id)) +
                                delta;
    if (increment > NGHTTP2_MAX_WINDOW_SIZE - window)
    {
      write_rst_stream(stream_id, NGHTTP2_FLOW_CONTROL_ERROR);
      return true;
    }
    delta += increment;
    return false;
  }

  std::int64_t &precheck_stream_window_delta(
      std::int32_t stream_id,
      std::vector<std::pair<std::int32_t, std::int64_t>> &stream_window_deltas)
  {
    for (auto &entry : stream_window_deltas)
    {
      if (entry.first == stream_id)
      {
        return entry.second;
      }
    }
    stream_window_deltas.push_back({stream_id, 0});
    return stream_window_deltas.back().second;
  }

  std::size_t outbound_window_capacity(std::int32_t stream_id, std::size_t requested_length)
  {
    const int32_t connection_window = nghttp2_session_get_remote_window_size(session_.get());
    const int32_t stream_window = nghttp2_session_get_stream_remote_window_size(session_.get(), stream_id);
    const int32_t effective_window = std::min(connection_window, stream_window);
    if (effective_window <= 0)
    {
      return 0;
    }
    return std::min(requested_length, static_cast<std::size_t>(effective_window));
  }

  bool accept_request_stream(std::int32_t stream_id)
  {
    if ((stream_id & 1) == 0 || stream_id <= last_request_stream_id_)
    {
      rejected_stream_ids_.insert(stream_id);
      submit_goaway(NGHTTP2_PROTOCOL_ERROR);
      return false;
    }

    last_request_stream_id_ = stream_id;
    highest_observed_request_stream_id_ = stream_id;
    return true;
  }

  void complete_stream(std::int32_t stream_id)
  {
    auto stream = streams_.find(stream_id);
    if (stream != streams_.end() && !final_request_body_length_valid(stream->second))
    {
      stream->second.invalid = true;
      if (stream->second.execution_session)
      {
        stream->second.execution_session->fail_request_body("HTTP/2 request body length mismatch");
        stream->second.execution_session.reset();
      }
      reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }
    if (stream != streams_.end() && stream->second.execution_session)
    {
      stream->second.execution_session->finish_request_body();
    }
    if (std::find(completed_stream_ids_.begin(), completed_stream_ids_.end(), stream_id) == completed_stream_ids_.end())
    {
      completed_stream_ids_.push_back(stream_id);
    }
  }

  void submit_completed_streams()
  {
    std::vector<std::int32_t> completed;
    completed.swap(completed_stream_ids_);
    sort_stream_ids_by_priority(completed);
    for (const std::int32_t stream_id : completed)
    {
      auto stream = streams_.find(stream_id);
      if (stream == streams_.end())
      {
        continue;
      }
      submit_execution(stream_id, stream->second);
    }
  }

  PriorityNode &priority_node_for(std::int32_t stream_id)
  {
    auto entry = priority_tree_.find(stream_id);
    if (entry != priority_tree_.end())
    {
      return entry->second;
    }

    PriorityNode node;
    node.seen = stream_id == 0;
    auto inserted = priority_tree_.emplace(stream_id, std::move(node));
    if (stream_id != 0)
    {
      PriorityNode &root = priority_node_for(0);
      if (std::find(root.children.begin(), root.children.end(), stream_id) == root.children.end())
      {
        root.children.push_back(stream_id);
      }
    }
    return inserted.first->second;
  }

  const PriorityNode *priority_node(std::int32_t stream_id) const
  {
    auto entry = priority_tree_.find(stream_id);
    if (entry == priority_tree_.end())
    {
      return nullptr;
    }
    return &entry->second;
  }

  bool priority_seen_for(std::int32_t stream_id) const
  {
    const PriorityNode *node = priority_node(stream_id);
    if (node && node->seen)
    {
      return true;
    }
    auto stream = streams_.find(stream_id);
    return stream != streams_.end() && stream->second.priority_seen;
  }

  void remove_priority_child(std::int32_t parent_id, std::int32_t child_id)
  {
    PriorityNode &parent = priority_node_for(parent_id);
    parent.children.erase(
        std::remove(parent.children.begin(), parent.children.end(), child_id),
        parent.children.end());
  }

  void add_priority_child(std::int32_t parent_id, std::int32_t child_id)
  {
    PriorityNode &parent = priority_node_for(parent_id);
    if (std::find(parent.children.begin(), parent.children.end(), child_id) == parent.children.end())
    {
      parent.children.push_back(child_id);
    }
  }

  bool priority_ancestor_of(std::int32_t ancestor_id, std::int32_t stream_id) const
  {
    std::unordered_set<std::int32_t> visited;
    std::int32_t current = stream_id;
    while (current != 0 && visited.insert(current).second)
    {
      const PriorityNode *node = priority_node(current);
      if (!node)
      {
        return false;
      }
      if (node->parent == ancestor_id)
      {
        return true;
      }
      current = node->parent;
    }
    return false;
  }

  bool apply_priority(
      std::int32_t stream_id,
      std::int32_t dependency,
      std::int32_t weight,
      bool exclusive)
  {
    if (stream_id <= 0 || dependency == stream_id || weight < 1 || weight > 256)
    {
      return false;
    }

    priority_node_for(dependency);
    priority_node_for(stream_id);
    priority_node_for(0);

    {
      PriorityNode &root = priority_node_for(0);
      root.parent = 0;
      root.weight = 16;
      root.seen = true;
    }

    const std::int32_t old_parent = priority_node_for(stream_id).parent;
    if (priority_ancestor_of(stream_id, dependency))
    {
      const std::int32_t dependency_old_parent = priority_node_for(dependency).parent;
      remove_priority_child(dependency_old_parent, dependency);
      priority_node_for(dependency).parent = old_parent;
      add_priority_child(old_parent, dependency);
    }

    remove_priority_child(priority_node_for(stream_id).parent, stream_id);

    if (exclusive)
    {
      std::vector<std::int32_t> displaced_children;
      displaced_children.swap(priority_node_for(dependency).children);
      priority_node_for(dependency).children.push_back(stream_id);
      priority_node_for(stream_id).children.clear();
      for (const std::int32_t child_id : displaced_children)
      {
        if (child_id == stream_id)
        {
          continue;
        }
        priority_node_for(child_id).parent = stream_id;
        priority_node_for(stream_id).children.push_back(child_id);
      }
    }
    else
    {
      add_priority_child(dependency, stream_id);
    }

    PriorityNode &stream_node = priority_node_for(stream_id);
    stream_node.parent = dependency;
    stream_node.weight = weight;
    stream_node.exclusive = exclusive;
    stream_node.seen = true;
    stream_node.closed = false;

    auto stream = streams_.find(stream_id);
    if (stream != streams_.end())
    {
      stream->second.priority_seen = true;
      stream->second.priority_dependency = dependency;
      stream->second.priority_weight = weight;
      stream->second.priority_exclusive = exclusive;
    }
    return true;
  }

  std::vector<PriorityPathElement> priority_path_for(std::int32_t stream_id) const
  {
    std::vector<PriorityPathElement> reversed;
    std::unordered_set<std::int32_t> visited;
    std::int32_t current = stream_id;
    while (current != 0 && visited.insert(current).second)
    {
      const PriorityNode *node = priority_node(current);
      if (!node)
      {
        reversed.push_back(PriorityPathElement{current, 16, 0});
        break;
      }
      reversed.push_back(PriorityPathElement{current, node->weight, node->sent_bytes});
      current = node->parent;
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  }

  void note_priority_bytes_sent(std::int32_t stream_id, std::size_t bytes)
  {
    if (bytes == 0)
    {
      return;
    }
    PriorityNode &node = priority_node_for(stream_id);
    const std::uint64_t increment = static_cast<std::uint64_t>(bytes);
    node.sent_bytes = node.sent_bytes > std::numeric_limits<std::uint64_t>::max() - increment
                          ? std::numeric_limits<std::uint64_t>::max()
                          : node.sent_bytes + increment;
  }

  bool priority_less(std::int32_t left, std::int32_t right) const
  {
    if (left == right)
    {
      return false;
    }
    const std::vector<PriorityPathElement> left_path = priority_path_for(left);
    const std::vector<PriorityPathElement> right_path = priority_path_for(right);
    const std::size_t shared = std::min(left_path.size(), right_path.size());
    for (std::size_t index = 0; index < shared; ++index)
    {
      if (left_path[index].stream_id == right_path[index].stream_id)
      {
        continue;
      }
      const long double left_cost =
          static_cast<long double>(left_path[index].sent_bytes) /
          static_cast<long double>(std::max(left_path[index].weight, 1));
      const long double right_cost =
          static_cast<long double>(right_path[index].sent_bytes) /
          static_cast<long double>(std::max(right_path[index].weight, 1));
      if (left_cost < right_cost)
      {
        return true;
      }
      if (right_cost < left_cost)
      {
        return false;
      }
      if (left_path[index].weight != right_path[index].weight)
      {
        return left_path[index].weight > right_path[index].weight;
      }
      return left_path[index].stream_id < right_path[index].stream_id;
    }
    if (left_path.size() != right_path.size())
    {
      return left_path.size() < right_path.size();
    }
    return left < right;
  }

  void sort_stream_ids_by_priority(std::vector<std::int32_t> &stream_ids) const
  {
    std::stable_sort(
        stream_ids.begin(),
        stream_ids.end(),
        [this](std::int32_t left, std::int32_t right)
        {
          return priority_less(left, right);
        });
  }

  void sort_finished_tasks_by_priority(std::vector<std::shared_ptr<ExecutionTask>> &tasks) const
  {
    std::stable_sort(
        tasks.begin(),
        tasks.end(),
        [this](const std::shared_ptr<ExecutionTask> &left, const std::shared_ptr<ExecutionTask> &right)
        {
          return priority_less(left->stream_id, right->stream_id);
        });
  }

  bool should_defer_for_higher_priority(std::int32_t stream_id)
  {
    for (auto &entry : streams_)
    {
      const std::int32_t candidate_id = entry.first;
      StreamState &candidate = entry.second;
      if (candidate_id == stream_id ||
          !priority_less(candidate_id, stream_id) ||
          outbound_window_capacity(candidate_id, 1) == 0 ||
          !stream_has_ready_outbound_data(candidate))
      {
        continue;
      }
      return true;
    }
    return false;
  }

  bool stream_has_ready_outbound_data(StreamState &stream)
  {
    if (stream_has_remaining_response_body(stream))
    {
      return true;
    }
    return stream.http2_stream_state &&
           stream.tunnel_accepted &&
           Vajra::rack::http2_stream_has_outbound(stream.http2_stream_state.get());
  }

  void resume_priority_deferred_streams()
  {
    for (auto &entry : streams_)
    {
      if (stream_has_ready_outbound_data(entry.second))
      {
        nghttp2_session_resume_data(session_.get(), entry.first);
      }
    }
  }

  static bool stream_has_remaining_response_body(const StreamState &stream)
  {
    return stream.response_chunk_index < stream.response_body_chunks.size() ||
           (stream.response_body_file && stream.response_body_file_offset < stream.response_body_size);
  }

  void record_header(StreamState &stream, const std::string &name, const std::string &value)
  {
    if (name.empty())
    {
      stream.invalid = true;
      return;
    }

    if (name[0] == ':')
    {
      if (stream.pseudo_complete)
      {
        stream.invalid = true;
        return;
      }
      record_pseudo_header(stream, name, value);
      return;
    }

    stream.pseudo_complete = true;
    if (connection_specific_header(name))
    {
      stream.invalid = true;
      return;
    }
    stream.headers.push_back(ParsedHeader{name, value});
  }

  void record_priority(std::int32_t stream_id, const nghttp2_priority_spec &priority)
  {
    if (!apply_priority(stream_id, priority.stream_id, priority.weight, priority.exclusive != 0))
    {
      reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
    }
  }

  void record_pseudo_header(StreamState &stream, const std::string &name, const std::string &value)
  {
    if (name == ":method")
    {
      assign_unique(stream.method_seen, stream.method, value, stream);
    }
    else if (name == ":path")
    {
      assign_unique(stream.path_seen, stream.path, value, stream);
    }
    else if (name == ":scheme")
    {
      assign_unique(stream.scheme_seen, stream.scheme, value, stream);
    }
    else if (name == ":authority")
    {
      assign_unique(stream.authority_seen, stream.authority, value, stream);
    }
    else if (name == ":protocol")
    {
      assign_unique(stream.protocol_seen, stream.protocol, value, stream);
    }
    else
    {
      stream.invalid = true;
    }
  }

  void assign_unique(bool &seen, std::string &target, const std::string &value, StreamState &stream)
  {
    if (seen || value.empty())
    {
      stream.invalid = true;
      return;
    }
    seen = true;
    target = value;
  }

  bool valid_request(const StreamState &stream) const
  {
    if (stream.invalid ||
        !stream.method_seen ||
        !stream.scheme_seen ||
        (stream.scheme != "https" && stream.scheme != "http") ||
        !declared_content_length_state(stream).valid ||
        !authority_host_consistent(stream))
    {
      return false;
    }

    if (extended_connect_request(stream))
    {
      return stream.path_seen &&
             stream.authority_seen &&
             !stream.path.empty() &&
             !stream.authority.empty() &&
             !stream.protocol.empty();
    }
    if (stream.method == "CONNECT")
    {
      return false;
    }

    return stream.path_seen && !stream.protocol_seen;
  }

  bool extended_connect_request(const StreamState &stream) const
  {
    return stream.method_seen &&
           stream.protocol_seen &&
           stream.method == "CONNECT";
  }

  std::shared_ptr<Vajra::rack::Http2StreamState> ensure_http2_stream_state(std::int32_t stream_id, StreamState &stream)
  {
    if (!stream.http2_stream_state)
    {
      auto state = std::make_shared<Vajra::rack::Http2StreamState>();
      state->stream_id = stream_id;
      state->protocol = stream.protocol;
      state->websocket = stream.protocol == "websocket";
      stream.http2_stream_state = std::move(state);
    }
    return stream.http2_stream_state;
  }

  bool start_http2_stream_execution(std::int32_t stream_id, StreamState &stream)
  {
    if (stream.executed)
    {
      return true;
    }
    if (!valid_request(stream) || !extended_connect_request(stream))
    {
      reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return false;
    }
    ensure_http2_stream_state(stream_id, stream);
    complete_stream(stream_id);
    return true;
  }

  ContentLengthState declared_content_length_state(const StreamState &stream) const
  {
    ContentLengthState state;
    for (const ParsedHeader &header : stream.headers)
    {
      if (!Vajra::request::ascii_case_insensitive_equal(header.name, "content-length"))
      {
        continue;
      }

      if (state.present)
      {
        state.valid = false;
        return state;
      }

      const std::optional<std::size_t> parsed = parse_content_length_value(header.value);
      if (!parsed)
      {
        state.present = true;
        state.valid = false;
        return state;
      }
      state.present = true;
      state.value = *parsed;
    }
    return state;
  }

  std::optional<std::size_t> declared_content_length(const StreamState &stream) const
  {
    const ContentLengthState state = declared_content_length_state(stream);
    if (!state.valid || !state.present)
    {
      return std::nullopt;
    }
    return state.value;
  }

  bool final_request_body_length_valid(const StreamState &stream) const
  {
    const ContentLengthState state = declared_content_length_state(stream);
    return state.valid && (!state.present || stream.request_body_bytes == state.value);
  }

  bool authority_host_consistent(const StreamState &stream) const
  {
    if (stream.authority.empty())
    {
      return true;
    }
    for (const ParsedHeader &header : stream.headers)
    {
      if (Vajra::request::ascii_case_insensitive_equal(header.name, "host") &&
          !Vajra::request::ascii_case_insensitive_equal(header.value, stream.authority))
      {
        return false;
      }
    }
    return true;
  }

  bool can_defer_http2_body_execution(const StreamState &stream) const
  {
    const std::optional<std::size_t> content_length = declared_content_length(stream);
    if (!content_length)
    {
      return stream.request_body.size() <= kHttp2DirectBodyBytes;
    }
    return *content_length <= config_.max_request_body_bytes &&
           *content_length <= kHttp2DirectBodyBytes;
  }

  bool direct_body_flow_control_can_be_released(const StreamState &stream) const
  {
    const std::optional<std::size_t> content_length = declared_content_length(stream);
    return content_length &&
           *content_length <= config_.max_request_body_bytes &&
           *content_length <= kHttp2DirectBodyBytes;
  }

  void reserve_deferred_body_capacity(StreamState &stream) const
  {
    const std::optional<std::size_t> content_length = declared_content_length(stream);
    if (content_length &&
        *content_length <= config_.max_request_body_bytes &&
        *content_length <= kHttp2DirectBodyBytes &&
        stream.request_body.capacity() < *content_length)
    {
      stream.request_body.reserve(*content_length);
    }
  }

  RequestContext request_context_for(StreamState &stream) const
  {
    if (!authority_host_consistent(stream))
    {
      stream.invalid = true;
    }
    if (!stream.authority.empty() &&
        std::none_of(stream.headers.begin(), stream.headers.end(), [](const ParsedHeader &header)
                     { return Vajra::request::ascii_case_insensitive_equal(header.name, "host"); }))
    {
      stream.headers.push_back(ParsedHeader{"host", stream.authority});
    }

    RequestContext request_context{
        ParsedRequest{ParsedRequestLine{stream.method, stream.path, "HTTP/2"}, stream.headers},
        socket_context_,
        connection_.fd(),
        "",
        nullptr,
        nullptr};
    request_context.http2_stream = stream.http2_stream_state;
    return request_context;
  }

  bool start_execution_session(std::int32_t stream_id, StreamState &stream)
  {
    if (stream.executed)
    {
      return !stream.execution_start_failed;
    }
    stream.executed = true;

    if (!valid_request(stream))
    {
      reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return false;
    }

    RequestContext request_context = request_context_for(stream);
    if (!request_executor_)
    {
      return true;
    }

    try
    {
      stream.execution_session = request_executor_->start(request_context);
      if (stream.execution_session)
      {
        stream.input_state_owner = stream.execution_session->native_input_state_owner();
      }
      return true;
    }
    catch (const QueueCapacityError &)
    {
      stream.queue_capacity_rejected = true;
      stream.execution_start_failed = true;
      stream.response = fallback_response(503, "Service Unavailable");
      return false;
    }
    catch (const RequestTimeoutError &)
    {
      stream.execution_start_failed = true;
      stream.response = fallback_response(504, "Gateway Timeout");
      return false;
    }
    catch (...)
    {
      stream.execution_start_failed = true;
      stream.response = fallback_response(500, "Internal Server Error");
      return false;
    }
  }

  bool start_streaming_execution_session(std::int32_t stream_id, StreamState &stream)
  {
    if (!start_execution_session(stream_id, stream))
    {
      return false;
    }

    if (stream.execution_session && !stream.request_body.empty())
    {
      stream.execution_session->append_request_body_bytes(stream.request_body.data(), stream.request_body.size());
      stream.request_body.clear();
    }
    return true;
  }

  void submit_execution(std::int32_t stream_id, StreamState &stream)
  {
    if (!valid_request(stream))
    {
      reset_stream(stream_id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }

    const bool direct_execution = !stream.executed && stream.execution_session == nullptr;
    if (!stream.executed && !direct_execution)
    {
      start_execution_session(stream_id, stream);
    }

    RequestContext request_context = request_context_for(stream);
    if (direct_execution)
    {
      stream.executed = true;
      request_context.request_body = std::move(stream.request_body);
    }

    auto task = std::make_shared<ExecutionTask>(
        stream_id,
        std::move(request_context),
        std::move(stream.execution_session),
        stream.started_at);
    if (stream.execution_start_failed)
    {
      task->queue_capacity_rejected = stream.queue_capacity_rejected;
      task->response = stream.response;
      enqueue_finished_execution(task);
      return;
    }
    if (!async_execution_enabled())
    {
      task->response = finish_request(task->request_context, task->execution_session.get(), task->stream_id, &task->queue_capacity_rejected);
      enqueue_finished_execution(task);
      return;
    }
    if (async_execution_saturated())
    {
      task->queue_capacity_rejected = true;
      task->response = fallback_response(503, "Service Unavailable");
      enqueue_finished_execution(task);
      return;
    }
    if (direct_execution && request_executor_->async_completion_supported() &&
        enqueue_direct_completion_execution(task))
    {
      return;
    }
    enqueue_execution(task);
  }

  void enqueue_finished_execution(const std::shared_ptr<ExecutionTask> &task)
  {
    pending_execution_count_.fetch_add(1, std::memory_order_acq_rel);
    {
      std::lock_guard<std::mutex> lock(finished_executions_mutex_);
      finished_executions_.push_back(task);
    }
    finished_executions_condition_.notify_one();
  }

  Vajra::response::Response finish_request(
      const RequestContext &request_context,
      RequestExecutionSession *execution_session,
      std::int32_t stream_id,
      bool *queue_capacity_rejected = nullptr) const
  {
    if (!request_executor_)
    {
      return Vajra::response::Response{
          Vajra::response::Status{200, "OK"},
          {Vajra::response::Header{"Content-Type", "text/plain"}},
          "OK",
          Vajra::response::ConnectionBehavior::close};
    }

    try
    {
      if (execution_session == nullptr)
      {
        try
        {
          std::optional<Vajra::response::Response> response = request_executor_->execute(request_context);
          if (response)
          {
            return *response;
          }
        }
#ifdef VAJRA_RUNTIME_TESTING
        catch (const std::logic_error &error)
        {
          if (std::string(error.what()) != "request executor must override execute() or start()")
          {
            throw;
          }

          std::unique_ptr<RequestExecutionSession> fallback_session = request_executor_->start(request_context);
          std::optional<Vajra::response::Response> response = fallback_session->finish();
          if (response)
          {
            return *response;
          }
        }
#else
        catch (const std::logic_error &)
        {
          throw;
        }
#endif
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }

      std::optional<Vajra::response::Response> response = execution_session->finish();
      if (response)
      {
        return *response;
      }
    }
    catch (const QueueCapacityError &)
    {
      if (queue_capacity_rejected != nullptr)
      {
        *queue_capacity_rejected = true;
      }
      return fallback_response(503, "Service Unavailable");
    }
    catch (const RequestTimeoutError &)
    {
      return fallback_response(504, "Gateway Timeout");
    }
    catch (const std::exception &error)
    {
      std::cerr << "[Vajra][error] HTTP/2 stream execution failed"
                << " stream_id=" << stream_id
                << " method=" << request_context.request.request_line.method
                << " target=" << request_context.request.request_line.target
                << " error=" << error.what()
                << std::endl;
      return fallback_response(500, "Internal Server Error");
    }
    catch (...)
    {
      std::cerr << "[Vajra][error] HTTP/2 stream execution failed"
                << " stream_id=" << stream_id
                << " method=" << request_context.request.request_line.method
                << " target=" << request_context.request.request_line.target
                << " error=unknown exception"
                << std::endl;
      return fallback_response(500, "Internal Server Error");
    }

    return Vajra::response::Response{
        Vajra::response::Status{200, "OK"},
        {Vajra::response::Header{"Content-Type", "text/plain"}},
        "OK",
        Vajra::response::ConnectionBehavior::close};
  }

  void enqueue_execution(const std::shared_ptr<ExecutionTask> &task)
  {
    const auto started_at = std::chrono::steady_clock::now();
    pending_execution_count_.fetch_add(1, std::memory_order_acq_rel);
    try
    {
      execution_pool_->enqueue([this, task]()
                               {
        task->queue_wait_nanoseconds = elapsed_nanoseconds(task->enqueued_at);
        try
        {
          task->response = finish_request(task->request_context, task->execution_session.get(), task->stream_id, &task->queue_capacity_rejected);
        }
        catch (const std::exception &error)
        {
          std::cerr << "[Vajra][error] HTTP/2 async stream execution failed"
                    << " stream_id=" << task->stream_id
                    << " method=" << task->request_context.request.request_line.method
                    << " target=" << task->request_context.request.request_line.target
                    << " error=" << error.what()
                    << std::endl;
          task->response = fallback_response(500, "Internal Server Error");
        }
        catch (...)
        {
          std::cerr << "[Vajra][error] HTTP/2 async stream execution failed"
                    << " stream_id=" << task->stream_id
                    << " method=" << task->request_context.request.request_line.method
                    << " target=" << task->request_context.request.request_line.target
                    << " error=unknown exception"
                    << std::endl;
          task->response = fallback_response(500, "Internal Server Error");
        }
        {
          std::lock_guard<std::mutex> lock(finished_executions_mutex_);
          finished_executions_.push_back(task);
        }
        finished_executions_condition_.notify_one(); });
    }
    catch (...)
    {
      pending_execution_count_.fetch_sub(1, std::memory_order_acq_rel);
      throw;
    }
    Vajra::runtime::note_worker_http2_execution_enqueue_time(elapsed_nanoseconds(started_at));
  }

  bool enqueue_direct_completion_execution(const std::shared_ptr<ExecutionTask> &task)
  {
    const auto started_at = std::chrono::steady_clock::now();
    pending_execution_count_.fetch_add(1, std::memory_order_acq_rel);
    RequestContext execution_context = task->request_context;
    execution_context.request_body = std::move(task->request_context.request_body);
    const bool enqueued = request_executor_->execute_async(
        std::move(execution_context),
        [this, task](
            std::optional<Vajra::response::Response> response,
            std::string error_message,
            std::int64_t queue_wait_nanoseconds)
        {
          task->queue_wait_nanoseconds = queue_wait_nanoseconds;
          if (!error_message.empty())
          {
            std::cerr << "[Vajra][error] HTTP/2 async stream execution failed"
                      << " stream_id=" << task->stream_id
                      << " method=" << task->request_context.request.request_line.method
                      << " target=" << task->request_context.request.request_line.target
                      << " error=" << error_message
                      << std::endl;
            task->response = fallback_response(500, "Internal Server Error");
          }
          else if (response)
          {
            task->response = std::move(*response);
          }
          else
          {
            task->response = Vajra::response::Response{
                Vajra::response::Status{200, "OK"},
                {Vajra::response::Header{"Content-Type", "text/plain"}},
                "OK",
                Vajra::response::ConnectionBehavior::close};
          }
          {
            std::lock_guard<std::mutex> lock(finished_executions_mutex_);
            finished_executions_.push_back(task);
          }
          finished_executions_condition_.notify_one();
        });
    if (!enqueued)
    {
      pending_execution_count_.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
    Vajra::runtime::note_worker_http2_execution_enqueue_time(elapsed_nanoseconds(started_at));
    return true;
  }

  bool has_pending_executions() const
  {
    return pending_execution_count_.load(std::memory_order_acquire) > 0;
  }

  bool async_execution_enabled() const
  {
    return request_executor_ && request_executor_->async_execution_supported();
  }

  bool async_execution_saturated() const
  {
    return config_.max_pending_executions > 0 &&
           pending_execution_count_.load(std::memory_order_acquire) >= config_.max_pending_executions;
  }

  bool has_finished_executions() const
  {
    std::lock_guard<std::mutex> lock(finished_executions_mutex_);
    return !finished_executions_.empty();
  }

  void wait_for_finished_execution()
  {
    std::unique_lock<std::mutex> lock(finished_executions_mutex_);
    if (!finished_executions_.empty())
    {
      return;
    }
    finished_executions_condition_.wait_for(
        lock,
        std::chrono::milliseconds(kHttp2PendingWorkWaitMilliseconds),
        [this]()
        { return !finished_executions_.empty(); });
  }

  bool has_active_request_body_flow_control() const
  {
    if (!pending_receive_bytes_.empty())
    {
      return true;
    }
    for (const auto &entry : streams_)
    {
      const StreamState &stream = entry.second;
      if (!stream.paused_request_body_chunks.empty())
      {
        return true;
      }
      if (stream.input_state_owner &&
          !Vajra::rack::native_input_fully_consumed(stream.input_state_owner.get()))
      {
        return true;
      }
      if (stream.http2_stream_state)
      {
        std::lock_guard<std::mutex> lock(stream.http2_stream_state->mutex);
        if (stream.http2_stream_state->accepted ||
            stream.http2_stream_state->app_closed ||
            stream.http2_stream_state->reset ||
            stream.http2_stream_state->consumed_since_last_observation > 0 ||
            !stream.http2_stream_state->outbound_chunks.empty())
        {
          return true;
        }
      }
    }
    return false;
  }

  void drain_finished_executions()
  {
    const auto started_at = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<ExecutionTask>> finished;
    {
      std::unique_lock<std::mutex> lock(finished_executions_mutex_);
      if (!finished_executions_.empty() &&
          (pending_execution_count_.load(std::memory_order_acquire) > finished_executions_.size() ||
           has_higher_priority_pending_execution(finished_executions_)))
      {
        finished_executions_condition_.wait_for(
            lock,
            std::chrono::milliseconds(kHttp2PriorityCoalesceWaitMilliseconds));
      }
      finished.swap(finished_executions_);
    }
    sort_finished_tasks_by_priority(finished);

    for (const auto &task : finished)
    {
      auto stream = streams_.find(task->stream_id);
      if (stream == streams_.end())
      {
        --pending_execution_count_;
        continue;
      }
      Vajra::runtime::note_worker_http2_execution_queue_wait_time(task->queue_wait_nanoseconds);
      complete_execution(*task, stream->second);
      --pending_execution_count_;
    }
    Vajra::runtime::note_worker_http2_execution_drain_time(elapsed_nanoseconds(started_at));
  }

  bool has_higher_priority_pending_execution(const std::vector<std::shared_ptr<ExecutionTask>> &finished) const
  {
    if (finished.empty())
    {
      return false;
    }
    for (const auto &entry : streams_)
    {
      const std::int32_t stream_id = entry.first;
      const StreamState &stream = entry.second;
      if (!priority_seen_for(stream_id) ||
          stream.invalid ||
          rejected_stream_ids_.find(stream_id) != rejected_stream_ids_.end() ||
          !stream.response_body_chunks.empty() ||
          stream.tunnel_accepted)
      {
        continue;
      }
      const bool already_finished = std::any_of(
          finished.begin(),
          finished.end(),
          [stream_id](const std::shared_ptr<ExecutionTask> &task)
          { return task->stream_id == stream_id; });
      if (!already_finished)
      {
        for (const auto &task : finished)
        {
          if (priority_less(stream_id, task->stream_id))
          {
            return true;
          }
        }
      }
    }
    return false;
  }

  void complete_execution(ExecutionTask &task, StreamState &stream)
  {
    if (stream.http2_stream_state)
    {
      flush_http2_stream_events();
      bool accepted = false;
      {
        std::lock_guard<std::mutex> lock(stream.http2_stream_state->mutex);
        accepted = stream.http2_stream_state->accepted;
      }
      if (accepted)
      {
        Vajra::runtime::note_worker_request_completed();
        Vajra::runtime::note_worker_request_time(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - task.started_at)
                .count());
        log_access_if_enabled(task, stream);
        ++completed_request_count_;
        if (config_.max_keepalive_requests > 0 &&
            completed_request_count_ >= config_.max_keepalive_requests &&
            !goaway_submitted_)
        {
          check(
              nghttp2_submit_goaway(
                  session_.get(),
                  NGHTTP2_FLAG_NONE,
                  last_request_stream_id_,
                  NGHTTP2_NO_ERROR,
                  nullptr,
                  0),
              "nghttp2_submit_goaway");
          goaway_submitted_ = true;
        }
        return;
      }
    }

    stream.response = std::move(task.response);
    stream.response_body_chunks.clear();
    stream.response_body_file.reset();
    stream.response_body_size = Vajra::response::response_body_size(stream.response);
    if (Vajra::response::response_has_body_chunks(stream.response))
    {
      stream.response_body_chunks = std::move(stream.response.body_chunks);
    }
    else if (!stream.response.body.empty())
    {
      stream.response_body_chunks.push_back(std::move(stream.response.body));
    }
    else if (Vajra::response::response_has_body_file(stream.response))
    {
      stream.response_body_file = stream.response.body_file;
    }
    stream.response_chunk_index = 0;
    stream.response_chunk_offset = 0;
    stream.response_body_file_offset = 0;
    if (!validate_http2_response_or_replace_with_fallback(stream))
    {
      stream.response_body_chunks.clear();
      stream.response_body_file.reset();
      stream.response_body_size = Vajra::response::response_body_size(stream.response);
      if (!stream.response.body.empty())
      {
        stream.response_body_chunks.push_back(stream.response.body);
      }
    }
    submit_response(task.stream_id, stream);
    Vajra::runtime::note_worker_request_completed();
    Vajra::runtime::note_worker_request_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - task.started_at)
            .count());
    log_access_if_enabled(task, stream);
    if (task.queue_capacity_rejected)
    {
      submit_goaway(NGHTTP2_ENHANCE_YOUR_CALM);
    }
    ++completed_request_count_;
    if (config_.max_keepalive_requests > 0 &&
        completed_request_count_ >= config_.max_keepalive_requests &&
        !goaway_submitted_)
    {
      check(
          nghttp2_submit_goaway(
              session_.get(),
              NGHTTP2_FLAG_NONE,
              last_request_stream_id_,
              NGHTTP2_NO_ERROR,
              nullptr,
              0),
          "nghttp2_submit_goaway");
      goaway_submitted_ = true;
    }
  }

  void wait_for_execution_completion()
  {
    while (has_pending_executions())
    {
      drain_finished_executions();
      flush_http2_stream_events();
      if (has_pending_executions())
      {
        wait_for_finished_execution();
      }
    }
  }

  void flush_http2_stream_events()
  {
    for (auto &entry : streams_)
    {
      const std::int32_t stream_id = entry.first;
      StreamState &stream = entry.second;
      if (!stream.http2_stream_state)
      {
        continue;
      }

      std::uint32_t reset_error = 0;
      bool reset_requested = false;
      bool app_closed = false;
      {
        std::lock_guard<std::mutex> lock(stream.http2_stream_state->mutex);
        reset_requested = stream.http2_stream_state->reset;
        reset_error = stream.http2_stream_state->reset_error_code;
        app_closed = stream.http2_stream_state->app_closed;
      }
      if (reset_requested)
      {
        reset_stream(stream_id, reset_error == 0 ? kHttp2CancelErrorCode : reset_error);
        stream.tunnel_end_stream_queued = true;
        continue;
      }

      int status = 0;
      std::vector<Vajra::response::Header> accept_headers;
      if (Vajra::rack::http2_stream_take_accept(stream.http2_stream_state.get(), status, accept_headers))
      {
        submit_http2_stream_accept(stream_id, stream, status, accept_headers);
      }

      const bool has_outbound = Vajra::rack::http2_stream_has_outbound(stream.http2_stream_state.get());
      if (has_outbound && stream.tunnel_data_provider_active)
      {
        const int result = nghttp2_session_resume_data(session_.get(), stream_id);
        if (result < 0 && result != NGHTTP2_ERR_INVALID_STATE && result != NGHTTP2_ERR_INVALID_ARGUMENT)
        {
          check(result, "nghttp2_session_resume_data");
        }
      }

      if (app_closed && stream.tunnel_accepted && !stream.tunnel_end_stream_queued)
      {
        stream.tunnel_end_stream_queued = true;
        if (stream.tunnel_data_provider_active)
        {
          const int result = nghttp2_session_resume_data(session_.get(), stream_id);
          if (result < 0 && result != NGHTTP2_ERR_INVALID_STATE && result != NGHTTP2_ERR_INVALID_ARGUMENT)
          {
            check(result, "nghttp2_session_resume_data");
          }
        }
      }

      if (stream.tunnel_accepted &&
          !stream.tunnel_data_provider_active &&
          (has_outbound || stream.tunnel_end_stream_queued))
      {
        nghttp2_data_provider provider;
        provider.source.ptr = nullptr;
        provider.read_callback = data_read_callback;
        check(
            nghttp2_submit_data(
                session_.get(),
                stream.tunnel_end_stream_queued && stream.response_chunk_index >= stream.response_body_chunks.size() ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
                stream_id,
                &provider),
            "nghttp2_submit_data");
        stream.tunnel_data_provider_active = true;
      }
    }
  }

  void submit_http2_stream_accept(
      std::int32_t stream_id,
      StreamState &stream,
      int status,
      const std::vector<Vajra::response::Header> &accept_headers)
  {
    if (!validate_http2_response_head(status, accept_headers))
    {
      reset_stream(stream_id, NGHTTP2_INTERNAL_ERROR);
      return;
    }

    std::vector<OwnedHttp2Header> owned_headers;
    owned_headers.reserve(accept_headers.size() + 1);
    owned_headers.push_back(OwnedHttp2Header{":status", std::to_string(status)});
    for (const Vajra::response::Header &header : accept_headers)
    {
      if (Vajra::response::http2_forbidden_response_header_name(header.name))
      {
        continue;
      }
      owned_headers.push_back(OwnedHttp2Header{lower_ascii(header.name), header.value});
    }

    std::vector<nghttp2_nv> headers;
    headers.reserve(owned_headers.size());
    for (const OwnedHttp2Header &header : owned_headers)
    {
      headers.push_back(header_nv(header.name, header.value));
    }

    check(
        nghttp2_submit_headers(
            session_.get(),
            NGHTTP2_FLAG_NONE,
            stream_id,
            nullptr,
            headers.data(),
            headers.size(),
            nullptr),
        "nghttp2_submit_headers");
    stream.tunnel_accepted = true;
    stream.response.status = Vajra::response::Status{status, status == 200 ? "OK" : ""};
  }

  bool validate_http2_response_head(
      int status,
      const std::vector<Vajra::response::Header> &headers) const
  {
    try
    {
      Vajra::response::ResponseSerializer serializer;
      Vajra::response::Response validation_response{
          Vajra::response::Status{status, ""},
          headers,
          "",
          Vajra::response::ConnectionBehavior::close};
      std::vector<Vajra::response::Header> filtered_headers;
      filtered_headers.reserve(headers.size());
      for (const Vajra::response::Header &header : headers)
      {
        validation_response.headers = {header};
        if (!Vajra::response::framing_header_name(header.name))
        {
          serializer.validate(validation_response);
        }
        else
        {
          if (header.name.empty() || Vajra::response::contains_invalid_http_text_bytes(header.value))
          {
            throw Vajra::response::SerializationError("response header syntax is invalid");
          }
          for (const unsigned char character : header.name)
          {
            if (!Vajra::response::valid_header_name_character(character))
            {
              throw Vajra::response::SerializationError("response header name contains an unsafe character");
            }
          }
        }
      }
      validation_response.headers.clear();
      serializer.validate(validation_response);
      return true;
    }
    catch (const std::exception &error)
    {
      std::cerr << "[Vajra][error] HTTP/2 response validation failed: " << error.what() << std::endl;
      return false;
    }
  }

  bool validate_http2_response_or_replace_with_fallback(StreamState &stream) const
  {
    if (validate_http2_response_head(stream.response.status.code, stream.response.headers) &&
        !(Vajra::response::status_forbids_message_body(stream.response.status.code) &&
          stream.response_body_size > 0))
    {
      return true;
    }

    stream.response = fallback_response(500, "Internal Server Error");
    stream.response_chunk_index = 0;
    stream.response_chunk_offset = 0;
    stream.response_body_file_offset = 0;
    return false;
  }

  void submit_response(std::int32_t stream_id, StreamState &stream)
  {
    const auto started_at = std::chrono::steady_clock::now();
    std::vector<OwnedHttp2Header> owned_headers;
    owned_headers.reserve(stream.response.headers.size() + 2);
    owned_headers.push_back(OwnedHttp2Header{":status", std::to_string(stream.response.status.code)});

    for (const Vajra::response::Header &header : stream.response.headers)
    {
      if (Vajra::response::http2_forbidden_response_header_name(header.name))
      {
        continue;
      }
      owned_headers.push_back(OwnedHttp2Header{lower_ascii(header.name), header.value});
    }
    const bool status_forbids_body = Vajra::response::status_forbids_message_body(stream.response.status.code);
    if (stream.response_body_size > 0 && !status_forbids_body)
    {
      owned_headers.push_back(OwnedHttp2Header{"content-length", std::to_string(stream.response_body_size)});
    }

    std::vector<nghttp2_nv> headers;
    headers.reserve(owned_headers.size());
    for (const OwnedHttp2Header &header : owned_headers)
    {
      headers.push_back(header_nv(header.name, header.value));
    }

    nghttp2_data_provider provider;
    provider.source.ptr = nullptr;
    provider.read_callback = data_read_callback;
    const bool suppress_response_body = stream.method == "HEAD" || status_forbids_body;
    const nghttp2_data_provider *provider_ptr =
        stream.response_body_size == 0 || suppress_response_body ? nullptr : &provider;
    check(
        nghttp2_submit_response(
            session_.get(),
            stream_id,
            headers.data(),
            headers.size(),
            provider_ptr),
        "nghttp2_submit_response");
    Vajra::runtime::note_worker_http2_response_submit_time(elapsed_nanoseconds(started_at));
  }

  void resume_deferred_data()
  {
    for (const auto &entry : streams_)
    {
      const std::int32_t stream_id = entry.first;
      const StreamState &stream = entry.second;
      if (stream_has_remaining_response_body(stream) ||
          (stream.http2_stream_state && stream.tunnel_data_provider_active))
      {
        const int result = nghttp2_session_resume_data(session_.get(), stream_id);
        if (result < 0 && result != NGHTTP2_ERR_INVALID_STATE && result != NGHTTP2_ERR_INVALID_ARGUMENT)
        {
          check(result, "nghttp2_session_resume_data");
        }
      }
    }
  }

  void reset_stream(std::int32_t stream_id, std::uint32_t error_code)
  {
    check(
        nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, stream_id, error_code),
        "nghttp2_submit_rst_stream");
  }

  void submit_goaway(std::uint32_t error_code)
  {
    if (goaway_submitted_)
    {
      return;
    }
    goaway_submitted_ = true;
    check(
        nghttp2_submit_goaway(
            session_.get(),
            NGHTTP2_FLAG_NONE,
            nghttp2_session_get_last_proc_stream_id(session_.get()),
            error_code,
            nullptr,
            0),
        "nghttp2_submit_goaway");
  }

  void write_rst_stream(std::int32_t stream_id, std::uint32_t error_code)
  {
    const std::array<std::uint8_t, 13> frame = rst_stream_frame(stream_id, error_code);
    write_control_frame(frame.data(), frame.size());
  }

  void write_goaway(std::uint32_t error_code)
  {
    const std::array<std::uint8_t, 17> frame = goaway_frame(highest_observed_request_stream_id_, error_code);
    write_control_frame(frame.data(), frame.size());
  }

  void write_control_frame(const std::uint8_t *data, std::size_t length)
  {
    std::size_t written = 0;
    while (written < length)
    {
      const ssize_t result = connection_.write(
          reinterpret_cast<const char *>(data + written),
          length - written);
      if (result <= 0)
      {
        throw std::runtime_error("HTTP/2 control frame write failed");
      }
      written += static_cast<std::size_t>(result);
    }
  }

  void remember_closed_stream(std::int32_t stream_id)
  {
    if (stream_id <= 0)
    {
      return;
    }

    if (closed_stream_ids_.insert(stream_id).second)
    {
      closed_stream_order_.push_back(stream_id);
    }
    while (closed_stream_order_.size() > kHttp2ClosedStreamRetention)
    {
      closed_stream_ids_.erase(closed_stream_order_.front());
      closed_stream_order_.pop_front();
    }
    mark_priority_stream_closed(stream_id);
  }

  void mark_priority_stream_closed(std::int32_t stream_id)
  {
    auto entry = priority_tree_.find(stream_id);
    if (entry == priority_tree_.end())
    {
      return;
    }
    entry->second.closed = true;
    prune_closed_priority_leaf(stream_id);
  }

  void prune_closed_priority_leaf(std::int32_t stream_id)
  {
    std::int32_t current = stream_id;
    while (current != 0)
    {
      auto entry = priority_tree_.find(current);
      if (entry == priority_tree_.end() || !entry->second.closed || !entry->second.children.empty())
      {
        return;
      }
      const std::int32_t parent = entry->second.parent;
      remove_priority_child(parent, current);
      priority_tree_.erase(entry);
      current = parent;
    }
  }

  void consume_drained_request_body_bytes()
  {
    for (auto &entry : streams_)
    {
      StreamState &stream = entry.second;
      if (stream.buffered_request_body_bytes_to_consume > 0)
      {
        const int result = nghttp2_session_consume(session_.get(), entry.first, stream.buffered_request_body_bytes_to_consume);
        if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT && result != NGHTTP2_ERR_INVALID_STATE)
        {
          throw std::runtime_error("nghttp2 consume failed: " + std::string(nghttp2_strerror(result)));
        }
        stream.buffered_request_body_bytes_to_consume = 0;
        nghttp2_session_resume_data(session_.get(), entry.first);
      }
      if (stream.http2_stream_state)
      {
        const std::size_t consumed = Vajra::rack::http2_stream_take_consumed_bytes(stream.http2_stream_state.get());
        if (consumed > 0)
        {
          const int result = nghttp2_session_consume(session_.get(), entry.first, consumed);
          if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT && result != NGHTTP2_ERR_INVALID_STATE)
          {
            throw std::runtime_error("nghttp2 consume failed: " + std::string(nghttp2_strerror(result)));
          }
          nghttp2_session_resume_data(session_.get(), entry.first);
        }
      }
      if (!stream.input_state_owner)
      {
        continue;
      }

      const std::size_t consumed = Vajra::rack::native_input_take_consumed_bytes(stream.input_state_owner.get());
      if (consumed == 0)
      {
        continue;
      }

      const int result = nghttp2_session_consume(session_.get(), entry.first, consumed);
      if (result != 0 && result != NGHTTP2_ERR_INVALID_ARGUMENT && result != NGHTTP2_ERR_INVALID_STATE)
      {
        throw std::runtime_error("nghttp2 consume failed: " + std::string(nghttp2_strerror(result)));
      }
      nghttp2_session_resume_data(session_.get(), entry.first);
    }
  }

  void check(int result, const char *operation) const
  {
    if (result < 0)
    {
      throw std::runtime_error(std::string(operation) + " failed: " + nghttp2_strerror(result));
    }
  }

  void log_access_if_enabled(const ExecutionTask &task, const StreamState &stream) const
  {
    if (!Vajra::runtime::access_logging_enabled())
    {
      return;
    }

    Vajra::runtime::log_access_event(Vajra::runtime::AccessLogEvent{
        task.request_context.request.request_line.method,
        task.request_context.request.request_line.target,
        stream.response.status.code,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - task.started_at)
            .count(),
        stream.response_body_size,
        task.request_context.socket.remote_address,
        task.request_context.request.request_line.version,
        stream.authority,
        "",
        "",
        "",
        getpid(),
        static_cast<int>(Vajra::runtime::current_worker_index()),
        "keepalive",
        "",
        ""});
  }

  Vajra::transport::Connection &connection_;
  SocketContext socket_context_;
  Http2Config config_;
  std::shared_ptr<const RequestExecutor> request_executor_;
  std::shared_ptr<Http2ExecutionPool> execution_pool_;
  std::unique_ptr<nghttp2_session_callbacks, CallbackDeleter> callbacks_;
  std::unique_ptr<nghttp2_session, SessionDeleter> session_;
  std::unordered_map<std::int32_t, StreamState> streams_;
  std::unordered_map<std::int32_t, PriorityNode> priority_tree_;
  std::unordered_set<std::int32_t> rejected_stream_ids_;
  std::unordered_set<std::int32_t> closed_stream_ids_;
  std::deque<std::int32_t> closed_stream_order_;
  std::vector<std::uint8_t> precheck_buffer_;
  std::size_t precheck_skip_payload_bytes_ = 0;
  std::vector<std::int32_t> completed_stream_ids_;
  std::vector<std::shared_ptr<ExecutionTask>> finished_executions_;
  mutable std::mutex finished_executions_mutex_;
  std::condition_variable finished_executions_condition_;
  std::atomic<std::size_t> pending_execution_count_{0};
  std::size_t completed_request_count_ = 0;
  std::int32_t last_request_stream_id_ = 0;
  std::int32_t highest_observed_request_stream_id_ = 0;
  EntryMode entry_mode_ = EntryMode::client_preface;
  std::string initial_bytes_;
  bool goaway_submitted_ = false;
  bool peer_goaway_received_ = false;
  bool client_preface_seen_ = false;
  bool resume_deferred_data_pending_ = false;
  std::vector<std::uint8_t> pending_receive_bytes_;
};

Vajra::request::Http2Session::Http2Session(
    Vajra::transport::Connection &connection,
    SocketContext socket_context,
    Http2Config config,
    std::shared_ptr<const RequestExecutor> request_executor,
    std::shared_ptr<Http2ExecutionPool> execution_pool)
    : impl_(std::make_unique<Impl>(
          connection,
          std::move(socket_context),
          config,
          std::move(request_executor),
          std::move(execution_pool)))
{
}

Vajra::request::Http2Session::Http2Session(
    Vajra::transport::Connection &connection,
    SocketContext socket_context,
    Http2Config config,
    std::shared_ptr<const RequestExecutor> request_executor,
    std::shared_ptr<Http2ExecutionPool> execution_pool,
    std::string initial_bytes)
    : impl_(std::make_unique<Impl>(
          connection,
          std::move(socket_context),
          config,
          std::move(request_executor),
          std::move(execution_pool),
          Impl::EntryMode::prior_knowledge,
          std::move(initial_bytes)))
{
}

Vajra::request::Http2Session::Http2Session(
    Vajra::transport::Connection &connection,
    SocketContext socket_context,
    Http2Config config,
    std::shared_ptr<const RequestExecutor> request_executor,
    std::shared_ptr<Http2ExecutionPool> execution_pool,
    Http2UpgradeRequest upgrade_request)
    : impl_(std::make_unique<Impl>(
          connection,
          std::move(socket_context),
          config,
          std::move(request_executor),
          std::move(execution_pool),
          Impl::EntryMode::upgrade,
          "",
          std::move(upgrade_request)))
{
}

Vajra::request::Http2Session::~Http2Session() = default;

void Vajra::request::Http2Session::run()
{
  impl_->run();
}
