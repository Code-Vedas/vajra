// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_size_validator.hpp"
#include "request/request_body_reader.hpp"
#include "request/http_field_utils.hpp"
#include "request/http2_session.hpp"
#include "request/request_processor.hpp"
#include "rack/http2_stream.hpp"
#include "response/response_serializer.hpp"
#include "response/response_writer.hpp"
#include "runtime/runtime_logging.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"
#include "vendor/nghttp2/lib/includes/nghttp2/nghttp2.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace VajraSpecCpp
{
  namespace
  {
    class RaisingRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw std::runtime_error("boom");
      }
    };

    class HeadErrorRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw Vajra::request::bad_request_error("invalid Rack environment translation");
      }
    };

    class InvalidResponseRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Bad Header", "value"}},
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class FramingHeadersRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {
                Vajra::response::Header{"Content-Type", "text/plain"},
                Vajra::response::Header{"Content-Length", "999"},
                Vajra::response::Header{"Connection", "keep-alive"},
                Vajra::response::Header{"Transfer-Encoding", "chunked"},
            },
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class EchoRequestBodyExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &request_context) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "application/octet-stream"}},
            request_context.request_body,
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class TraceHeaderRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {
                Vajra::response::Header{"Content-Type", "text/plain"},
                Vajra::response::Header{"X-Vajra-Internal-Trace-Id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
            },
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class EchoHostRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &request_context) const override
      {
        std::string host;
        for (const Vajra::request::ParsedHeader &header : request_context.request.headers)
        {
          if (Vajra::request::ascii_case_insensitive_equal(header.name, "host"))
          {
            host = header.value;
          }
        }
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            host,
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class HeadBodyRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "body-for-head",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class NoBodyStatusRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      explicit NoBodyStatusRequestExecutor(int status)
          : status_(status)
      {
      }

      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{status_, ""},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "body-for-no-body-status",
            Vajra::response::ConnectionBehavior::close};
      }

    private:
      int status_;
    };

    class ForbiddenHttp2ResponseHeadersRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {
                Vajra::response::Header{"X-Allowed", "yes"},
                Vajra::response::Header{"Upgrade", "websocket"},
                Vajra::response::Header{"Keep-Alive", "timeout=5"},
                Vajra::response::Header{"Proxy-Connection", "keep-alive"},
            },
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }
    };

    class AcceptingTunnelRequestExecutionSession final : public Vajra::request::RequestExecutionSession
    {
    public:
      explicit AcceptingTunnelRequestExecutionSession(
          std::shared_ptr<Vajra::rack::Http2StreamState> stream,
          bool invalid_accept_headers = false)
          : stream_(std::move(stream)), invalid_accept_headers_(invalid_accept_headers)
      {
      }

      void append_request_body_bytes(const char *, std::size_t) override
      {
      }

      std::optional<Vajra::response::Response> finish() override
      {
        if (!stream_)
        {
          fail("HTTP/2 extended CONNECT did not receive a native stream object");
        }
        {
          std::lock_guard<std::mutex> lock(stream_->mutex);
          stream_->accepted = true;
          stream_->accept_status = 200;
          if (invalid_accept_headers_)
          {
            stream_->accept_headers = {Vajra::response::Header{"Bad Header", "value"}};
          }
          else
          {
            stream_->accept_headers = {
                Vajra::response::Header{"X-Allowed", "yes"},
                Vajra::response::Header{"Upgrade", "websocket"},
                Vajra::response::Header{"Keep-Alive", "timeout=5"},
                Vajra::response::Header{"Proxy-Connection", "keep-alive"},
            };
          }
          stream_->app_closed = true;
        }
        stream_->event_condition.notify_all();
        return std::nullopt;
      }

    private:
      std::shared_ptr<Vajra::rack::Http2StreamState> stream_;
      bool invalid_accept_headers_;
    };

    class AcceptingTunnelRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      explicit AcceptingTunnelRequestExecutor(bool invalid_accept_headers = false)
          : invalid_accept_headers_(invalid_accept_headers)
      {
      }

      bool async_execution_supported() const override
      {
        return true;
      }

      std::unique_ptr<Vajra::request::RequestExecutionSession> start(
          const Vajra::request::RequestContext &request_context) const override
      {
        return std::make_unique<AcceptingTunnelRequestExecutionSession>(
            request_context.http2_stream,
            invalid_accept_headers_);
      }

    private:
      bool invalid_accept_headers_;
    };

    class CountingDirectRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::unique_ptr<Vajra::request::RequestExecutionSession> start(
          const Vajra::request::RequestContext &request_context) const override
      {
        start_count.fetch_add(1, std::memory_order_release);
        return Vajra::request::RequestExecutor::start(request_context);
      }

      std::optional<Vajra::response::Response> execute(
          const Vajra::request::RequestContext &request_context) const override
      {
        execute_count.fetch_add(1, std::memory_order_release);
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "direct:" + request_context.request_body,
            Vajra::response::ConnectionBehavior::close};
      }

      mutable std::atomic<std::size_t> start_count{0};
      mutable std::atomic<std::size_t> execute_count{0};
    };

    class ThrowingRequestExecutionSession final : public Vajra::request::RequestExecutionSession
    {
    public:
      void append_request_body_bytes(const char *, std::size_t) override
      {
      }

      std::optional<Vajra::response::Response> finish() override
      {
        throw 1;
      }
    };

    class UnknownThrowingAsyncRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      bool async_execution_supported() const override
      {
        return true;
      }

      std::unique_ptr<Vajra::request::RequestExecutionSession> start(const Vajra::request::RequestContext &) const override
      {
        return std::make_unique<ThrowingRequestExecutionSession>();
      }

      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw 1;
      }
    };

    class QueueCapacityRequestExecutionSession final : public Vajra::request::RequestExecutionSession
    {
    public:
      void append_request_body_bytes(const char *, std::size_t) override
      {
      }

      std::optional<Vajra::response::Response> finish() override
      {
        throw Vajra::request::QueueCapacityError("worker queue saturated");
      }
    };

    class QueueCapacityAsyncRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      bool async_execution_supported() const override
      {
        return true;
      }

      std::unique_ptr<Vajra::request::RequestExecutionSession> start(const Vajra::request::RequestContext &) const override
      {
        return std::make_unique<QueueCapacityRequestExecutionSession>();
      }

      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        throw Vajra::request::QueueCapacityError("worker queue saturated");
      }
    };

    class PausingBodyRequestExecutionSession final : public Vajra::request::RequestExecutionSession
    {
    public:
      explicit PausingBodyRequestExecutionSession(std::atomic<std::size_t> &accepted_body_bytes)
          : accepted_body_bytes_(accepted_body_bytes)
      {
      }

      void append_request_body_bytes(const char *data, std::size_t length) override
      {
        request_body_.append(data, length);
        accepted_body_bytes_.fetch_add(length, std::memory_order_acq_rel);
      }

      bool try_append_request_body_bytes(const char *data, std::size_t length) override
      {
        if (!paused_once_)
        {
          paused_once_ = true;
          return false;
        }
        append_request_body_bytes(data, length);
        return true;
      }

      std::optional<Vajra::response::Response> finish() override
      {
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "paused:" + std::to_string(request_body_.size()),
            Vajra::response::ConnectionBehavior::close};
      }

    private:
      std::atomic<std::size_t> &accepted_body_bytes_;
      bool paused_once_ = false;
      std::string request_body_;
    };

    class PausingBodyRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      std::unique_ptr<Vajra::request::RequestExecutionSession> start(const Vajra::request::RequestContext &) const override
      {
        return std::make_unique<PausingBodyRequestExecutionSession>(accepted_body_bytes);
      }

      std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
      {
        return std::nullopt;
      }

      mutable std::atomic<std::size_t> accepted_body_bytes{0};
    };

    class BlockingRequestExecutionSession final : public Vajra::request::RequestExecutionSession
    {
    public:
      BlockingRequestExecutionSession(
          std::atomic<std::size_t> &started_count,
          std::mutex &mutex,
          std::condition_variable &condition,
          bool &release)
          : started_count_(started_count),
            mutex_(mutex),
            condition_(condition),
            release_(release)
      {
      }

      void append_request_body_bytes(const char *, std::size_t) override
      {
      }

      std::optional<Vajra::response::Response> finish() override
      {
        started_count_.fetch_add(1, std::memory_order_release);
        condition_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]()
                        { return release_; });
        return Vajra::response::Response{
            Vajra::response::Status{200, "OK"},
            {Vajra::response::Header{"Content-Type", "text/plain"}},
            "OK",
            Vajra::response::ConnectionBehavior::close};
      }

    private:
      std::atomic<std::size_t> &started_count_;
      std::mutex &mutex_;
      std::condition_variable &condition_;
      bool &release_;
    };

    class BlockingAsyncRequestExecutor final : public Vajra::request::RequestExecutor
    {
    public:
      BlockingAsyncRequestExecutor(
          std::atomic<std::size_t> &started_count,
          std::mutex &mutex,
          std::condition_variable &condition,
          bool &release)
          : started_count_(started_count),
            mutex_(mutex),
            condition_(condition),
            release_(release)
      {
      }

      bool async_execution_supported() const override
      {
        return true;
      }

      std::unique_ptr<Vajra::request::RequestExecutionSession> start(const Vajra::request::RequestContext &) const override
      {
        return std::make_unique<BlockingRequestExecutionSession>(started_count_, mutex_, condition_, release_);
      }

    private:
      std::atomic<std::size_t> &started_count_;
      std::mutex &mutex_;
      std::condition_variable &condition_;
      bool &release_;
    };

    class BufferedConnection final : public Vajra::transport::Connection
    {
    public:
      explicit BufferedConnection(std::string input)
          : input_(std::move(input))
      {
      }

      int fd() const override
      {
        return -1;
      }

      bool wait_readable(int) override
      {
        return read_offset_ < input_.size();
      }

      ssize_t read(char *buffer, std::size_t length) override
      {
        if (read_offset_ >= input_.size())
        {
          return 0;
        }

        const std::size_t available = input_.size() - read_offset_;
        const std::size_t copied = std::min(length, available);
        std::memcpy(buffer, input_.data() + read_offset_, copied);
        read_offset_ += copied;
        return static_cast<ssize_t>(copied);
      }

      ssize_t write(const char *buffer, std::size_t length) override
      {
        output_.append(buffer, length);
        return static_cast<ssize_t>(length);
      }

      std::string protocol() const override
      {
        return "h2";
      }

      bool tls() const override
      {
        return true;
      }

      const std::string &output() const
      {
        return output_;
      }

    private:
      std::string input_;
      std::size_t read_offset_ = 0;
      std::string output_;
    };

    void append_h2_frame(std::string &buffer, std::uint8_t type, std::uint8_t flags, std::int32_t stream_id, const std::string &payload)
    {
      buffer.push_back(static_cast<char>((payload.size() >> 16) & 0xff));
      buffer.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
      buffer.push_back(static_cast<char>(payload.size() & 0xff));
      buffer.push_back(static_cast<char>(type));
      buffer.push_back(static_cast<char>(flags));
      buffer.push_back(static_cast<char>((stream_id >> 24) & 0x7f));
      buffer.push_back(static_cast<char>((stream_id >> 16) & 0xff));
      buffer.push_back(static_cast<char>((stream_id >> 8) & 0xff));
      buffer.push_back(static_cast<char>(stream_id & 0xff));
      buffer.append(payload);
    }

    void append_h2_uint32(std::string &buffer, std::uint32_t value)
    {
      buffer.push_back(static_cast<char>((value >> 24) & 0xff));
      buffer.push_back(static_cast<char>((value >> 16) & 0xff));
      buffer.push_back(static_cast<char>((value >> 8) & 0xff));
      buffer.push_back(static_cast<char>(value & 0xff));
    }

    struct H2Frame
    {
      std::uint8_t type = 0;
      std::uint8_t flags = 0;
      std::int32_t stream_id = 0;
      std::string payload;
    };

    std::vector<H2Frame> h2_frames_from(const std::string &output)
    {
      std::vector<H2Frame> frames;
      std::size_t offset = 0;
      while (offset + 9 <= output.size())
      {
        const std::size_t length =
            (static_cast<unsigned char>(output[offset]) << 16) |
            (static_cast<unsigned char>(output[offset + 1]) << 8) |
            static_cast<unsigned char>(output[offset + 2]);
        if (offset + 9 + length > output.size())
        {
          break;
        }
        const std::int32_t stream_id =
            (static_cast<std::int32_t>(static_cast<unsigned char>(output[offset + 5]) & 0x7f) << 24) |
            (static_cast<std::int32_t>(static_cast<unsigned char>(output[offset + 6])) << 16) |
            (static_cast<std::int32_t>(static_cast<unsigned char>(output[offset + 7])) << 8) |
            static_cast<unsigned char>(output[offset + 8]);
        frames.push_back(H2Frame{
            static_cast<std::uint8_t>(output[offset + 3]),
            static_cast<std::uint8_t>(output[offset + 4]),
            stream_id,
            output.substr(offset + 9, length)});
        offset += 9 + length;
      }
      return frames;
    }

    bool h2_output_contains_rst_stream_error(
        const std::string &output,
        std::int32_t expected_stream_id,
        std::uint32_t expected_error_code)
    {
      for (const H2Frame &frame : h2_frames_from(output))
      {
        if (frame.type != 3 || frame.stream_id != expected_stream_id || frame.payload.size() < 4)
        {
          continue;
        }
        const std::uint32_t error_code =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[0])) << 24) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[1])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(frame.payload[2])) << 8) |
            static_cast<unsigned char>(frame.payload[3]);
        if (error_code == expected_error_code)
        {
          return true;
        }
      }
      return false;
    }

    bool h2_output_contains_data_frame(const std::string &output, std::int32_t expected_stream_id)
    {
      for (const H2Frame &frame : h2_frames_from(output))
      {
        if (frame.type == 0 && frame.stream_id == expected_stream_id)
        {
          return true;
        }
      }
      return false;
    }

    std::string h2_response_body_from(const std::string &output, std::int32_t expected_stream_id)
    {
      std::string body;
      for (const H2Frame &frame : h2_frames_from(output))
      {
        if (frame.type == 0 && frame.stream_id == expected_stream_id)
        {
          body.append(frame.payload);
        }
      }
      return body;
    }

    std::vector<std::pair<std::string, std::string>> h2_response_headers_from(
        const std::string &output,
        std::int32_t expected_stream_id)
    {
      nghttp2_hd_inflater *inflater = nullptr;
      if (nghttp2_hd_inflate_new(&inflater) != 0 || inflater == nullptr)
      {
        fail("failed to create HPACK inflater");
      }

      std::vector<std::pair<std::string, std::string>> headers;
      for (const H2Frame &frame : h2_frames_from(output))
      {
        if (frame.type != 1 || frame.stream_id != expected_stream_id)
        {
          continue;
        }

        std::size_t offset = 0;
        while (offset < frame.payload.size())
        {
          nghttp2_nv nv;
          int inflate_flags = 0;
          const ssize_t consumed = nghttp2_hd_inflate_hd(
              inflater,
              &nv,
              &inflate_flags,
              reinterpret_cast<std::uint8_t *>(const_cast<char *>(frame.payload.data() + offset)),
              frame.payload.size() - offset,
              1);
          if (consumed < 0)
          {
            nghttp2_hd_inflate_del(inflater);
            fail("failed to inflate HTTP/2 response headers");
          }
          offset += static_cast<std::size_t>(consumed);
          if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) != 0)
          {
            headers.emplace_back(
                std::string(reinterpret_cast<char *>(nv.name), nv.namelen),
                std::string(reinterpret_cast<char *>(nv.value), nv.valuelen));
          }
          if ((inflate_flags & NGHTTP2_HD_INFLATE_FINAL) != 0)
          {
            nghttp2_hd_inflate_end_headers(inflater);
            break;
          }
          if (consumed == 0)
          {
            break;
          }
        }
      }

      nghttp2_hd_inflate_del(inflater);
      return headers;
    }

    bool h2_headers_contain(
        const std::vector<std::pair<std::string, std::string>> &headers,
        const std::string &name,
        const std::string &value)
    {
      return std::find(headers.begin(), headers.end(), std::make_pair(name, value)) != headers.end();
    }

    std::string h2_literal_string(const std::string &value)
    {
      if (value.size() > 127)
      {
        fail("test HTTP/2 literal string is too large for single-byte HPACK encoding");
      }

      std::string encoded;
      encoded.push_back(static_cast<char>(value.size()));
      encoded.append(value);
      return encoded;
    }

    void append_h2_literal_header(std::string &header_block, std::uint8_t name_index, const std::string &value)
    {
      header_block.push_back(static_cast<char>(0x40 | name_index));
      header_block.append(h2_literal_string(value));
    }

    std::string h2_request_header_block(
        const std::string &method,
        const std::string &path,
        const std::string &authority,
        const std::vector<std::pair<std::string, std::string>> &headers = {})
    {
      std::string header_block;
      if (method == "GET")
      {
        header_block.push_back(static_cast<char>(0x82));
      }
      else if (method == "POST")
      {
        header_block.push_back(static_cast<char>(0x83));
      }
      else
      {
        append_h2_literal_header(header_block, 2, method);
      }
      if (path == "/")
      {
        header_block.push_back(static_cast<char>(0x84));
      }
      else
      {
        append_h2_literal_header(header_block, 4, path);
      }
      header_block.push_back(static_cast<char>(0x86));
      append_h2_literal_header(header_block, 1, authority);
      for (const auto &header : headers)
      {
        if (header.first == "host")
        {
          append_h2_literal_header(header_block, 38, header.second);
        }
        else if (header.first == "content-length")
        {
          append_h2_literal_header(header_block, 28, header.second);
        }
        else
        {
          header_block.push_back(static_cast<char>(0x40));
          header_block.append(h2_literal_string(header.first));
          header_block.append(h2_literal_string(header.second));
        }
      }
      return header_block;
    }

    std::string h2_request_bytes(const std::string &header_block, const std::string &body = "")
    {
      std::string request = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
      append_h2_frame(request, 4, 0, 0, "");
      append_h2_frame(request, 1, body.empty() ? 0x5 : 0x4, 1, header_block);
      if (!body.empty())
      {
        append_h2_frame(request, 0, 0x1, 1, body);
      }
      return request;
    }

    std::string h2_get_request_bytes()
    {
      std::string header_block;
      header_block.push_back(static_cast<char>(0x82));
      header_block.push_back(static_cast<char>(0x84));
      header_block.push_back(static_cast<char>(0x86));
      header_block.push_back(static_cast<char>(0x41));
      header_block.append(h2_literal_string("localhost"));

      std::string request = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
      append_h2_frame(request, 4, 0, 0, "");
      append_h2_frame(request, 1, 0x5, 1, header_block);
      return request;
    }

    std::string h2_two_get_request_bytes()
    {
      std::string header_block;
      header_block.push_back(static_cast<char>(0x82));
      header_block.push_back(static_cast<char>(0x84));
      header_block.push_back(static_cast<char>(0x86));
      header_block.push_back(static_cast<char>(0x41));
      header_block.append(h2_literal_string("localhost"));

      std::string request = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
      append_h2_frame(request, 4, 0, 0, "");
      append_h2_frame(request, 1, 0x5, 1, header_block);
      append_h2_frame(request, 1, 0x5, 3, header_block);
      return request;
    }

    bool h2_output_contains_goaway_error(const std::string &output, std::uint32_t expected_error_code)
    {
      std::size_t offset = 0;
      while (offset + 9 <= output.size())
      {
        const std::size_t length =
            (static_cast<unsigned char>(output[offset]) << 16) |
            (static_cast<unsigned char>(output[offset + 1]) << 8) |
            static_cast<unsigned char>(output[offset + 2]);
        const std::uint8_t type = static_cast<std::uint8_t>(output[offset + 3]);
        if (offset + 9 + length > output.size())
        {
          return false;
        }
        if (type == 7 && length >= 8)
        {
          const std::size_t error_offset = offset + 13;
          const std::uint32_t error_code =
              (static_cast<std::uint32_t>(static_cast<unsigned char>(output[error_offset])) << 24) |
              (static_cast<std::uint32_t>(static_cast<unsigned char>(output[error_offset + 1])) << 16) |
              (static_cast<std::uint32_t>(static_cast<unsigned char>(output[error_offset + 2])) << 8) |
              static_cast<unsigned char>(output[error_offset + 3]);
          if (error_code == expected_error_code)
          {
            return true;
          }
        }
        offset += 9 + length;
      }

      return false;
    }

    std::thread start_request_processor_thread(
        const Vajra::request::RequestProcessor &processor,
        FileDescriptorGuard &server_socket)
    {
      const int owned_fd = dup(server_socket.get());
      if (owned_fd < 0)
      {
        fail("dup failed while transferring request processor socket ownership");
      }

      server_socket.close_if_open();
      return std::thread([&processor, owned_fd]()
                         {
                           FileDescriptorGuard guard(owned_fd);
                           Vajra::transport::PlainConnection connection(owned_fd);
                           processor.handle(connection, Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"});
                         });
    }

    void expect_serialization_error(
        const Vajra::response::Response &response,
        const std::string &expected_message)
    {
      try
      {
        Vajra::response::ResponseSerializer serializer;
        (void)serializer.serialize(response);
      }
      catch (const Vajra::response::SerializationError &error)
      {
        if (std::string(error.what()).find(expected_message) == std::string::npos)
        {
          fail("response serializer raised the wrong validation error");
        }

        return;
      }

      fail("response serializer accepted an invalid response");
    }

    void test_response_serializer_serializes_status_headers_and_body()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{201, "Created"},
          {
              Vajra::response::Header{"Content-Type", "text/plain"},
              Vajra::response::Header{"X-Trace-Id", "trace-123"},
          },
          "done",
          Vajra::response::ConnectionBehavior::keep_alive};

      const std::string serialized = serializer.serialize(response);
      const std::string expected =
          "HTTP/1.1 201 Created\r\n"
          "Content-Type: text/plain\r\n"
          "X-Trace-Id: trace-123\r\n"
          "Content-Length: 4\r\n"
          "\r\n"
          "done";

      if (serialized != expected)
      {
        fail("response serializer did not produce the expected wire format");
      }
    }

    void test_response_serializer_serializes_chunked_response_storage()
    {
      Vajra::response::ResponseSerializer serializer;
      Vajra::response::Response response{
          Vajra::response::Status{200, "OK"},
          {Vajra::response::Header{"Content-Type", "text/plain"}},
          "",
          Vajra::response::ConnectionBehavior::keep_alive};
      response.body_chunks = {"ab", "cd"};

      const std::string serialized = serializer.serialize(response);
      const std::string expected =
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain\r\n"
          "Content-Length: 4\r\n"
          "\r\n"
          "abcd";

      if (serialized != expected)
      {
        fail("response serializer did not preserve chunked response body bytes");
      }
    }

    std::shared_ptr<Vajra::response::ResponseBodyFile> response_body_file_from(const std::string &body)
    {
      FILE *file = std::tmpfile();
      if (file == nullptr)
      {
        fail("tmpfile failed while creating response body fixture");
      }
      auto body_file = std::make_shared<Vajra::response::ResponseBodyFile>(file);
      if (std::fwrite(body.data(), 1, body.size(), file) != body.size())
      {
        fail("fwrite failed while creating response body fixture");
      }
      body_file->size = body.size();
      return body_file;
    }

    void test_response_serializer_and_writer_stream_file_backed_body()
    {
      const std::string body = std::string(2048, 'a') + std::string(2048, 'b');
      Vajra::response::Response response{
          Vajra::response::Status{200, "OK"},
          {Vajra::response::Header{"Content-Type", "text/plain"}},
          "",
          Vajra::response::ConnectionBehavior::close};
      response.body_file = response_body_file_from(body);

      Vajra::response::ResponseSerializer serializer;
      const std::string serialized = serializer.serialize(response);
      if (serialized.find("Content-Length: 4096\r\n") == std::string::npos)
      {
        fail("file-backed response body did not report the correct content length");
      }
      if (serialized.substr(serialized.size() - body.size()) != body)
      {
        fail("file-backed response serializer did not preserve body bytes");
      }

      const std::string sent = send_response_through_socket(response);
      if (sent.find("Content-Length: 4096\r\n") == std::string::npos ||
          sent.substr(sent.size() - body.size()) != body)
      {
        fail("file-backed response writer did not stream exact response bytes");
      }
    }

    void test_response_serializer_allows_empty_reason_phrase()
    {
      Vajra::response::ResponseSerializer serializer;
      const std::string serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, ""},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"});

      if (serialized.find("HTTP/1.1 200 \r\n") != 0)
      {
        fail("response serializer did not preserve an empty reason phrase");
      }
    }

    void test_response_serializer_omits_content_length_for_no_body_status()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{204, "No Content"},
          {Vajra::response::Header{"Content-Type", "text/plain"}},
          ""};

      const std::string serialized = serializer.serialize(response);
      if (serialized.find("Content-Length:") != std::string::npos)
      {
        fail("no-body statuses must not emit content length framing");
      }

      if (serialized.find("\r\n\r\n") == std::string::npos)
      {
        fail("no-body statuses did not terminate headers correctly");
      }
    }

    void test_response_serializer_emits_connection_close_only_when_requested()
    {
      Vajra::response::ResponseSerializer serializer;
      const std::string close_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK",
              Vajra::response::ConnectionBehavior::close});

      if (close_serialized.find("Connection: close\r\n") == std::string::npos)
      {
        fail("response serializer omitted the close framing header");
      }

      const std::string keep_alive_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK",
              Vajra::response::ConnectionBehavior::keep_alive});

      if (keep_alive_serialized.find("Connection: close\r\n") != std::string::npos)
      {
        fail("response serializer emitted close framing for a reusable response");
      }
    }

    void test_response_serializer_preserves_header_order()
    {
      Vajra::response::ResponseSerializer serializer;
      const Vajra::response::Response response{
          Vajra::response::Status{200, "OK"},
          {
              Vajra::response::Header{"X-First", "1"},
              Vajra::response::Header{"X-Second", "2"},
              Vajra::response::Header{"X-Third", "3"},
          },
          "OK"};

      const std::string serialized = serializer.serialize(response);
      const std::size_t first = serialized.find("X-First: 1\r\n");
      const std::size_t second = serialized.find("X-Second: 2\r\n");
      const std::size_t third = serialized.find("X-Third: 3\r\n");

      if (first == std::string::npos || second == std::string::npos || third == std::string::npos)
      {
        fail("response serializer omitted headers");
      }

      if (!(first < second && second < third))
      {
        fail("response serializer did not preserve header order");
      }
    }

    void test_response_serializer_rejects_invalid_header_name()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Bad Header", "value"}},
              "OK"},
          "response header name contains an unsafe character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X(Header)", "value"}},
              "OK"},
          "response header name contains an unsafe character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{std::string("X-\xFF", 3), "value"}},
              "OK"},
          "response header name contains an unsafe character");
    }

    void test_response_serializer_rejects_invalid_header_value()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X-Test", "bad\r\nvalue"}},
              "OK"},
          "response header value contains an unsafe control character");
    }

    void test_response_serializer_rejects_forbidden_framing_headers()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Length", "999"}},
              "OK"},
          "response headers must not override HTTP framing headers");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Transfer-Encoding", "chunked"}},
              "OK"},
          "response headers must not override HTTP framing headers");
    }

    void test_response_serializer_rejects_other_control_characters()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, std::string("O\0K", 3)},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response reason phrase contains an unsafe control character");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"X-Test", std::string("bad\0value", 9)}},
              "OK"},
          "response header value contains an unsafe control character");
    }

    void test_response_serializer_rejects_invalid_status()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{99, "OK"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response status code must be within the HTTP/1.1 range 100-599");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{600, "Custom"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "OK"},
          "response status code must be within the HTTP/1.1 range 100-599");
    }

    void test_response_serializer_rejects_bodies_for_no_body_statuses()
    {
      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{204, "No Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "not allowed"},
          "response status must not include a message body");

      expect_serialization_error(
          Vajra::response::Response{
              Vajra::response::Status{205, "Reset Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              "not allowed"},
          "response status must not include a message body");

      Vajra::response::ResponseSerializer serializer;
      const std::string serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{304, "Not Modified"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              ""});

      if (serialized.find("Content-Length:") != std::string::npos)
      {
        fail("no-body statuses must not emit content length framing");
      }

      if (serialized.find("\r\n\r\n") == std::string::npos)
      {
        fail("no-body status response did not terminate headers correctly");
      }

      if (serialized.find("\r\n\r\nNot Modified") != std::string::npos)
      {
        fail("no-body statuses must not serialize a payload");
      }

      const std::string reset_content_serialized = serializer.serialize(
          Vajra::response::Response{
              Vajra::response::Status{205, "Reset Content"},
              {Vajra::response::Header{"Content-Type", "text/plain"}},
              ""});

      if (reset_content_serialized.find("Content-Length:") != std::string::npos)
      {
        fail("205 responses must not emit content length framing");
      }
    }

    void test_response_writer_send_returns_false_on_serialization_failure()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up invalid response writer test");
      }

      FileDescriptorGuard reader_socket(sockets[0]);
      FileDescriptorGuard writer_socket(sockets[1]);

      Vajra::response::ResponseWriter writer;
      const bool sent = writer.send(
          writer_socket.get(),
          Vajra::response::Response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Connection", "keep-alive"}},
              "OK"});

      if (sent)
      {
        fail("response writer reported success for an invalid response");
      }

      writer_socket.close_if_open();
      const std::string response = read_all(reader_socket.get());
      if (!response.empty())
      {
        fail("response writer emitted bytes for an invalid response");
      }
    }

    void test_response_writer_sends_structured_success_response()
    {
      Vajra::response::ResponseWriter writer;
      const std::string response =
          send_response_through_socket(writer.success_response(Vajra::response::ConnectionBehavior::keep_alive));

      if (response.find("HTTP/1.1 200 OK\r\n") != 0)
      {
        fail("success response did not start with a 200 OK status line");
      }

      if (response.find("Content-Type: text/plain\r\n") == std::string::npos)
      {
        fail("success response omitted content type");
      }

      if (response.find("Content-Length: 2\r\n") == std::string::npos)
      {
        fail("success response used the wrong content length");
      }

      if (response.find("Connection: close\r\n") != std::string::npos)
      {
        fail("success response unexpectedly forced connection close");
      }

      if (response.find("\r\n\r\nOK") == std::string::npos)
      {
        fail("success response body did not match the serialized payload");
      }
    }

    void test_response_writer_sends_structured_error_response()
    {
      Vajra::response::ResponseWriter writer;
      const std::string response = send_response_through_socket(
          writer.request_head_failure_response(Vajra::request::HeadFailureKind::header_too_large));

      if (response.find("HTTP/1.1 431 Request Header Fields Too Large\r\n") != 0)
      {
        fail("error response did not start with the expected status line");
      }

      if (response.find("Content-Length: 31\r\n") == std::string::npos)
      {
        fail("error response used the wrong content length");
      }

      if (response.find("Connection: close\r\n") == std::string::npos)
      {
        fail("error response did not force connection close");
      }

      if (response.find("\r\n\r\nRequest Header Fields Too Large") == std::string::npos)
      {
        fail("error response body did not match the serialized payload");
      }
    }

    void test_request_processor_keeps_connection_open_for_sequential_requests()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up sequential request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /first HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send first keep-alive request");
        }

        const std::string first_response = read_http_response(client_socket.get());
        if (first_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("first keep-alive response did not succeed");
        }

        if (first_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("first keep-alive response unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "POST /zero HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 000\r\n"
                "\r\n"))
        {
          fail("failed to send zero-valued content length request");
        }

        const std::string zero_length_response = read_http_response(client_socket.get());
        if (zero_length_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("zero-valued content length request did not receive a success response");
        }

        if (zero_length_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("zero-valued content length request unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "GET /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send second request on a reusable connection");
        }

        const std::string second_response = read_http_response(client_socket.get());
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second keep-alive response did not succeed");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second response did not advertise connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor did not close the connection after the close response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_handles_pipelined_read_ahead_without_losing_the_next_request()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up pipelined request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /first HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "\r\n"
                "GET /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send pipelined request bytes");
        }

        const std::string all_responses = read_all(client_socket.get());
        const std::size_t first_response_end = all_responses.find("\r\n\r\nOK");
        if (first_response_end == std::string::npos)
        {
          fail("pipelined responses did not contain the first complete payload");
        }

        const std::string first_response = all_responses.substr(0, first_response_end + 6);
        if (first_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("first pipelined response did not succeed");
        }

        if (first_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("first pipelined response unexpectedly forced connection close");
        }

        const std::string second_response = all_responses.substr(first_response_end + 6);
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second pipelined response did not succeed");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second pipelined response did not advertise connection close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_closes_after_parse_error_response()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up parse error request processor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /only-two-parts\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send malformed request");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("parse error response did not use 400 Bad Request");
        }

        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("parse error response did not force connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor left the connection open after a parse error");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_keeps_connection_open_after_request_body_framing()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up request body framing test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST / HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 4\r\n"
                "\r\n"
                "body"))
        {
          fail("failed to send request with body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("request with body framing did not receive the success response");
        }

        if (response.size() < 4 || response.compare(response.size() - 4, 4, "body") != 0)
        {
          fail("request processor did not transport the fixed-length request body");
        }

        if (response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("request with body framing unexpectedly forced connection close");
        }

        if (!send_all(
                client_socket.get(),
                "POST /second HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 6\r\n"
                "Connection: close\r\n"
                "\r\n"
                "second"))
        {
          fail("failed to send second request after request body framing");
        }

        const std::string second_response = read_http_response(client_socket.get());
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second request with body framing did not receive the success response");
        }

        if (second_response.size() < 6 || second_response.compare(second_response.size() - 6, 6, "second") != 0)
        {
          fail("request processor did not transport the second fixed-length request body");
        }

        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second request with explicit close did not advertise connection close");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor left the connection open after explicit close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_closes_http_1_0_without_keep_alive()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up HTTP/1.0 close test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /legacy HTTP/1.0\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send HTTP/1.0 request");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("HTTP/1.0 request did not receive success response");
        }
        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("HTTP/1.0 response did not force connection close by default");
        }
        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("HTTP/1.0 connection stayed open without Connection: keep-alive");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_keeps_http_1_0_alive_when_requested()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up HTTP/1.0 keep-alive test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /first HTTP/1.0\r\n"
                "Host: example.test\r\n"
                "Connection: keep-alive\r\n"
                "\r\n"
                "GET /second HTTP/1.0\r\n"
                "Host: example.test\r\n"
                "\r\n"))
        {
          fail("failed to send HTTP/1.0 keep-alive requests");
        }

        const std::string first_response = read_http_response(client_socket.get());
        if (first_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("first HTTP/1.0 keep-alive request did not receive success response");
        }
        if (first_response.find("Connection: close\r\n") != std::string::npos)
        {
          fail("HTTP/1.0 keep-alive response closed the first request");
        }

        const std::string second_response = read_http_response(client_socket.get());
        if (second_response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("second HTTP/1.0 request did not receive success response");
        }
        if (second_response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("second HTTP/1.0 response did not close when keep-alive was absent");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_body_reader_preserves_buffered_suffix_after_fixed_length_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "4"}}};

      const Vajra::request::BodyReadResult result = reader.read(
          -1,
          request,
          "bodyGET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");

      if (result.body != "body")
      {
        fail("fixed-length body reader did not preserve the request body");
      }

      if (result.remaining_buffered_bytes != "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n")
      {
        fail("fixed-length body reader did not preserve unread buffered bytes");
      }
    }

    void test_request_body_reader_preserves_buffered_suffix_after_chunked_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      const Vajra::request::BodyReadResult result = reader.read(
          -1,
          request,
          "3\r\nabc\r\n0\r\nX-Trailer: done\r\n\r\nGET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");

      if (result.body != "abc")
      {
        fail("chunked body reader did not decode the request body");
      }

      if (result.remaining_buffered_bytes != "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n")
      {
        fail("chunked body reader did not preserve unread buffered bytes");
      }
    }

    void test_request_body_reader_accepts_mixed_case_chunked_transfer_encoding_token()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "ChuNkEd"}}};

      const Vajra::request::BodyReadPlan plan = reader.plan_for(request);
      if (plan.framing != Vajra::request::BodyFraming::chunked)
      {
        fail("mixed-case chunked transfer-encoding token was not recognized");
      }
    }

    void test_request_body_reader_rejects_unsupported_transfer_encoding_lists()
    {
      for (const char *value : {"chunked, gzip", "gzip, chunked", "gzip", "gzip, xchunked", "chunked, chunked"})
      {
        Vajra::request::RequestBodyReader reader;
        const Vajra::request::ParsedRequest request{
            Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
            {Vajra::request::ParsedHeader{"Transfer-Encoding", value}}};

        try
        {
          (void)reader.plan_for(request);
        }
        catch (const Vajra::request::HeadError &error)
        {
          if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
              std::string(error.what()).find("unsupported Transfer-Encoding header") == std::string::npos)
          {
            fail("unsupported transfer-encoding list raised the wrong error");
          }
          continue;
        }

        fail("unsupported transfer-encoding list was accepted: " + std::string(value));
      }
    }

    void test_request_body_reader_rejects_strict_content_length_violations()
    {
      for (const char *value : {"5x", "+5", "184467440737095516160000000000000000000"})
      {
        Vajra::request::RequestBodyReader reader;
        const Vajra::request::ParsedRequest request{
            Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
            {Vajra::request::ParsedHeader{"Content-Length", value}}};

        try
        {
          (void)reader.plan_for(request);
        }
        catch (const Vajra::request::HeadError &error)
        {
          if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
              std::string(error.what()).find("invalid Content-Length header") == std::string::npos)
          {
            fail("strict content-length violation raised the wrong error");
          }
          continue;
        }

        fail("strict content-length violation was accepted: " + std::string(value));
      }

      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest duplicate_request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {
              Vajra::request::ParsedHeader{"Content-Length", "5"},
              Vajra::request::ParsedHeader{"Content-Length", "5"},
          }};

      try
      {
        (void)reader.plan_for(duplicate_request);
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("invalid Content-Length header") == std::string::npos)
        {
          fail("duplicate content-length raised the wrong error");
        }
        return;
      }

      fail("duplicate content-length was accepted");
    }

    void test_request_body_reader_rejects_chunk_size_trailing_junk()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      try
      {
        (void)reader.read(-1, request, "3 junk\r\nabc\r\n0\r\n\r\n");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("request body framing is invalid") == std::string::npos)
        {
          fail("chunk size trailing junk raised the wrong error");
        }
        return;
      }

      fail("chunk size trailing junk was accepted");
    }

    void test_request_body_reader_rejects_oversized_content_length_body()
    {
      const Vajra::request::RequestBodyReader reader(4);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "5"}}};

      try
      {
        (void)reader.read(-1, request, "");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("request body exceeds maximum size") == std::string::npos)
        {
          fail("oversized content-length body raised the wrong error");
        }

        return;
      }

      fail("oversized content-length body was accepted");
    }

    void test_request_body_reader_uses_direct_execution_for_small_content_length_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "1024"}}};

      const Vajra::request::BodyReadPlan plan = reader.plan_for(request);
      if (!reader.can_read_without_streaming(plan, ""))
      {
        fail("small content-length body was not eligible for direct execution");
      }
    }

    void test_request_body_reader_streams_large_content_length_body_until_buffered()
    {
      Vajra::request::RequestBodyReader reader;
      const std::size_t content_length = Vajra::request::kDefaultDirectRequestBodyBytes + 1;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", std::to_string(content_length)}}};

      const Vajra::request::BodyReadPlan plan = reader.plan_for(request);
      if (reader.can_read_without_streaming(plan, ""))
      {
        fail("large content-length body was eligible for direct execution before buffering");
      }
      if (!reader.can_read_without_streaming(plan, std::string(content_length, 'x')))
      {
        fail("fully buffered large content-length body was not eligible for direct execution");
      }
    }

    void test_request_body_reader_streams_chunked_body()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      const Vajra::request::BodyReadPlan plan = reader.plan_for(request);
      if (reader.can_read_without_streaming(plan, "4\r\nbody\r\n0\r\n\r\n"))
      {
        fail("chunked body was eligible for direct execution");
      }
    }

    void test_request_body_reader_rejects_overlong_chunk_metadata()
    {
      const Vajra::request::RequestBodyReader reader(
          Vajra::request::kDefaultMaxRequestBodyBytes,
          4,
          Vajra::request::kDefaultMaxTrailerLineBytes);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      try
      {
        (void)reader.read(-1, request, "12345");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request ||
            std::string(error.what()).find("request body metadata exceeds maximum size") == std::string::npos)
        {
          fail("overlong chunk metadata raised the wrong error");
        }

        return;
      }

      fail("overlong chunk metadata was accepted");
    }

    void test_request_body_reader_treats_chunk_metadata_at_limit_without_crlf_as_incomplete()
    {
      const Vajra::request::RequestBodyReader reader(
          Vajra::request::kDefaultMaxRequestBodyBytes,
          4,
          Vajra::request::kDefaultMaxTrailerLineBytes);
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Transfer-Encoding", "chunked"}}};

      try
      {
        (void)reader.read(-1, request, "1234");
      }
      catch (const Vajra::request::BodyReadIncompleteError &)
      {
        return;
      }

      fail("chunk metadata at the limit was not treated as an incomplete read");
    }

    void test_request_body_reader_treats_transport_failures_as_incomplete_reads()
    {
      Vajra::request::RequestBodyReader reader;
      const Vajra::request::ParsedRequest request{
          Vajra::request::ParsedRequestLine{"POST", "/", "HTTP/1.1"},
          {Vajra::request::ParsedHeader{"Content-Length", "4"}}};

      try
      {
        (void)reader.read(-1, request, "");
      }
      catch (const Vajra::request::BodyReadIncompleteError &)
      {
        return;
      }
      catch (const Vajra::request::HeadError &)
      {
        fail("transport failure was misclassified as invalid request body framing");
      }

      fail("transport failure did not raise an incomplete body read");
    }

    void test_request_processor_reads_fragmented_fixed_length_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up fragmented fixed-length body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /fragmented HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                "hello"))
        {
          fail("failed to send fixed-length body prefix");
        }

        if (!send_all(client_socket.get(), " world"))
        {
          fail("failed to send fixed-length body suffix");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 11) != "hello world")
        {
          fail("fragmented fixed-length request body was not reassembled");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_decodes_chunked_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up chunked body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /chunked HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "3\r\nabc\r\n"
                "6\r\n123456\r\n"
                "0\r\n\r\n"))
        {
          fail("failed to send chunked request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 9) != "abc123456")
        {
          fail("chunked request body was not decoded correctly");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_decodes_chunked_request_body_with_extensions_and_trailers()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up chunked extension test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /chunked-extensions HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "5;foo=bar\r\nhello\r\n"
                "1\r\n \r\n"
                "0\r\n"
                "X-Trailer: kept-native-only\r\n"
                "\r\n"))
        {
          fail("failed to send chunked request with extensions and trailers");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.substr(response.size() - 6) != "hello ")
        {
          fail("chunked body extensions or trailers were not handled correctly");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_uses_direct_execution_for_prebuffered_bodies()
    {
      const auto request_executor = std::make_shared<CountingDirectRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);

      BufferedConnection get_connection(
          "GET /fast HTTP/1.1\r\n"
          "Host: example.test\r\n"
          "Connection: close\r\n"
          "\r\n");
      processor.handle(
          get_connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"});
      if (get_connection.output().find("\r\n\r\ndirect:") == std::string::npos)
      {
        fail("direct no-body request did not execute synchronously");
      }

      BufferedConnection post_connection(
          "POST /fast HTTP/1.1\r\n"
          "Host: example.test\r\n"
          "Content-Length: 5\r\n"
          "Connection: close\r\n"
          "\r\n"
          "hello");
      processor.handle(
          post_connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"});
      if (post_connection.output().find("\r\n\r\ndirect:hello") == std::string::npos)
      {
        fail("direct prebuffered body request did not preserve the body");
      }

      if (request_executor->start_count.load(std::memory_order_acquire) != 0)
      {
        fail("request processor started a streaming session for a prebuffered request");
      }
      if (request_executor->execute_count.load(std::memory_order_acquire) != 2)
      {
        fail("request processor did not execute both fast-path requests");
      }
    }

    void test_request_processor_rejects_conflicting_request_body_framing()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up conflicting request body framing test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /conflict HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 3\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "3\r\nabc\r\n0\r\n\r\n"))
        {
          fail("failed to send conflicting request body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("conflicting request body framing did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_rejects_malformed_chunked_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up malformed chunked body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /malformed-chunked HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "Z\r\nabc\r\n0\r\n\r\n"))
        {
          fail("failed to send malformed chunked request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("malformed chunked request body did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_rejects_oversized_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up oversized request body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /too-large HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 16777217\r\n"
                "\r\n"))
        {
          fail("failed to send oversized request body framing");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("oversized request body did not receive a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_closes_quietly_when_request_body_is_incomplete()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up incomplete request body test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /partial HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 8\r\n"
                "\r\n"
                "body"))
        {
          fail("failed to send partial request body");
        }

        if (shutdown(client_socket.get(), SHUT_WR) < 0)
        {
          fail("failed to half-close partial request body socket");
        }

        if (!peer_closed_within(client_socket.get(), 500))
        {
          fail("request processor did not close after an incomplete request body");
        }

        const std::string response = read_all(client_socket.get());
        if (!response.empty())
        {
          fail("incomplete request body unexpectedly produced a response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_times_out_stalled_fixed_length_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up fixed-length request body timeout test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          Vajra::request::kDefaultMaxRequestBodyBytes,
          5,
          30,
          1,
          30,
          0,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /timeout HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Content-Length: 8\r\n"
                "\r\n"
                "body"))
        {
          fail("failed to send stalled fixed-length request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 408 Request Timeout\r\n") != 0)
        {
          fail("stalled fixed-length request body did not receive a 408 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_times_out_stalled_chunked_request_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up chunked request body timeout test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          Vajra::request::kDefaultMaxRequestBodyBytes,
          5,
          30,
          1,
          30,
          0,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "POST /timeout-chunked HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "5\r\nab"))
        {
          fail("failed to send stalled chunked request body");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 408 Request Timeout\r\n") != 0)
        {
          fail("stalled chunked request body did not receive a 408 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_suppresses_head_response_body()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up HEAD response body suppression test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<HeadBodyRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "HEAD /head HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send HEAD request");
        }

        const std::string response = read_all(client_socket.get());
        const std::size_t header_boundary = response.find("\r\n\r\n");
        if (header_boundary == std::string::npos)
        {
          fail("HEAD response did not include complete headers");
        }
        if (response.find("Content-Length: 13\r\n") == std::string::npos)
        {
          fail("HEAD response did not preserve content-length semantics");
        }
        if (!response.substr(header_boundary + 4).empty())
        {
          fail("HTTP/1 HEAD response emitted a response body");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_internal_server_error_when_executor_raises()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up executor failure test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<RaisingRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /boom HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for executor failure test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 500 Internal Server Error\r\n") != 0)
        {
          fail("executor failure did not return a 500 response");
        }

        if (response.find("Connection: close\r\n") == std::string::npos)
        {
          fail("executor failure did not force connection close");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_bad_request_when_executor_raises_head_error()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up head error executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<HeadErrorRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /bad HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for head error executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 400 Bad Request\r\n") != 0)
        {
          fail("executor head error did not return a 400 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_returns_internal_server_error_when_executor_response_is_invalid()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up invalid response executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<InvalidResponseRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /invalid HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for invalid response executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 500 Internal Server Error\r\n") != 0)
        {
          fail("invalid executor response did not return a 500 response");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_strips_executor_framing_headers_before_sending()
    {
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        fail("socketpair failed while setting up framing header executor test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());

      const auto request_executor = std::make_shared<FramingHeadersRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /headers HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for framing header executor test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("framing header executor response did not succeed");
        }

        if (response.find("Content-Length: 2\r\n") == std::string::npos)
        {
          fail("server did not restore correct content length framing");
        }

        if (response.find("Transfer-Encoding:") != std::string::npos)
        {
          fail("server leaked executor transfer-encoding framing");
        }

        if (response.find("Connection: keep-alive\r\n") != std::string::npos)
        {
          fail("server leaked executor connection framing");
        }

        client_socket.close_if_open();
        processor_thread.join();
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        throw;
      }
    }

    void test_request_processor_does_not_mix_partial_internal_trace_context_with_traceparent()
    {
      char log_path[] = "/tmp/vajra-access-log-XXXXXX";
      const int log_fd = mkstemp(log_path);
      if (log_fd < 0)
      {
        fail("mkstemp failed while setting up access log correlation test");
      }
      close(log_fd);

      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
      {
        std::remove(log_path);
        fail("socketpair failed while setting up access log correlation test");
      }

      FileDescriptorGuard client_socket(sockets[0]);
      FileDescriptorGuard server_socket(sockets[1]);
      suppress_sigpipe(client_socket.get());
      Vajra::runtime::stop_runtime_logging_worker();
      Vajra::runtime::configure_runtime_logging(false, log_path, "", "json");
      Vajra::runtime::start_runtime_logging_worker();
      const auto request_executor = std::make_shared<TraceHeaderRequestExecutor>();
      const Vajra::request::RequestProcessor processor(
          Vajra::request::kDefaultMaxRequestHeadBytes,
          request_executor);
      std::thread processor_thread = start_request_processor_thread(processor, server_socket);

      try
      {
        if (!send_all(
                client_socket.get(),
                "GET /trace HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "Traceparent: 00-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-cccccccccccccccc-01\r\n"
                "Connection: close\r\n"
                "\r\n"))
        {
          fail("failed to send request for access log correlation test");
        }

        const std::string response = read_http_response(client_socket.get());
        if (response.find("HTTP/1.1 200 OK\r\n") != 0)
        {
          fail("trace header executor response did not succeed");
        }
        if (response.find("X-Vajra-Internal-Trace-Id:") != std::string::npos)
        {
          fail("internal trace header leaked to client response");
        }

        client_socket.close_if_open();
        processor_thread.join();
        Vajra::runtime::stop_runtime_logging_worker();
        std::ifstream log_file(log_path);
        const std::string access_log((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
        if (access_log.find("\"trace_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") == std::string::npos)
        {
          fail("access log did not preserve app-provided trace id");
        }
        if (access_log.find("\"span_id\"") != std::string::npos ||
            access_log.find("cccccccccccccccc") != std::string::npos)
        {
          fail("access log mixed incoming traceparent span id with app-provided trace id");
        }
        Vajra::runtime::configure_runtime_logging(false, "/dev/null", "", "text");
        std::remove(log_path);
      }
      catch (...)
      {
        client_socket.close_if_open();
        if (processor_thread.joinable())
        {
          processor_thread.join();
        }
        Vajra::runtime::stop_runtime_logging_worker();
        Vajra::runtime::configure_runtime_logging(false, "/dev/null", "", "text");
        std::remove(log_path);
        throw;
      }
    }

    void test_http2_async_unknown_exception_completes_session_shutdown()
    {
      BufferedConnection connection(h2_get_request_bytes());
      const auto request_executor = std::make_shared<UnknownThrowingAsyncRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      {
        Vajra::request::Http2Session session(
            connection,
            Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
            Vajra::request::Http2Config{},
            request_executor,
            execution_pool);
        session.run();
      }

      if (connection.output().empty())
      {
        fail("HTTP/2 async unknown exception path did not emit any response frames");
      }
    }

    void test_http2_queue_capacity_error_emits_503_and_goaway()
    {
      BufferedConnection connection(h2_get_request_bytes());
      const auto request_executor = std::make_shared<QueueCapacityAsyncRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (connection.output().find("Service Unavailable") == std::string::npos)
      {
        fail("HTTP/2 queue capacity rejection did not emit a 503 body");
      }
      if (!h2_output_contains_goaway_error(connection.output(), NGHTTP2_ENHANCE_YOUR_CALM))
      {
        fail("HTTP/2 queue capacity rejection did not emit ENHANCE_YOUR_CALM GOAWAY");
      }
    }

    void test_http2_pending_execution_limit_rejects_second_stream()
    {
      BufferedConnection connection(h2_two_get_request_bytes());
      std::atomic<std::size_t> started_count{0};
      std::mutex mutex;
      std::condition_variable condition;
      bool release = false;
      const auto request_executor = std::make_shared<BlockingAsyncRequestExecutor>(
          started_count,
          mutex,
          condition,
          release);
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Config config;
      config.max_pending_executions = 1;

      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          config,
          request_executor,
          execution_pool);

      std::thread session_thread([&session]()
                                 { session.run(); });
      {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(2), [&started_count]()
                                { return started_count.load(std::memory_order_acquire) == 1; }))
        {
          fail("HTTP/2 pending execution limit test did not start the first async stream");
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
      }
      condition.notify_all();
      session_thread.join();

      if (connection.output().find("Service Unavailable") == std::string::npos)
      {
        fail("HTTP/2 pending execution saturation did not emit a 503 body");
      }
      if (!h2_output_contains_goaway_error(connection.output(), NGHTTP2_ENHANCE_YOUR_CALM))
      {
        fail("HTTP/2 pending execution saturation did not emit ENHANCE_YOUR_CALM GOAWAY");
      }
    }

    void test_http2_prior_knowledge_initial_bytes_serves_request()
    {
      BufferedConnection connection("");
      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool,
          h2_get_request_bytes());
      session.run();

      if (connection.output().empty())
      {
        fail("HTTP/2 prior-knowledge initial bytes did not emit response frames");
      }
    }

    void test_http2_upgrade_request_serves_stream_one()
    {
      std::string trailing_bytes;
      append_h2_frame(trailing_bytes, 4, 0, 0, "");
      BufferedConnection connection("");
      const auto request_executor = std::make_shared<TraceHeaderRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2UpgradeRequest upgrade_request{
          Vajra::request::RequestContext{
              Vajra::request::ParsedRequest{
                  Vajra::request::ParsedRequestLine{"GET", "/upgrade", "HTTP/1.1"},
                  {Vajra::request::ParsedHeader{"Host", "localhost"}}},
              Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"},
              -1,
              "",
              nullptr,
              nullptr},
          {},
          trailing_bytes};
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "http"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool,
          std::move(upgrade_request));
      session.run();

      if (connection.output().empty())
      {
        fail("HTTP/2 upgrade stream 1 did not emit response frames");
      }
    }

    void test_http2_rejects_short_content_length_body()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("POST", "/", "localhost", {{"content-length", "5"}})));
      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (!h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_PROTOCOL_ERROR))
      {
        fail("HTTP/2 short content-length request body was not reset");
      }
    }

    void test_http2_rejects_long_content_length_body()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("POST", "/", "localhost", {{"content-length", "1"}}),
          "ab"));
      const auto request_executor = std::make_shared<EchoRequestBodyExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (!h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_PROTOCOL_ERROR))
      {
        fail("HTTP/2 long content-length request body was not reset");
      }
    }

    void test_http2_paused_request_body_bytes_resume_without_goaway()
    {
      const std::size_t declared_body_size = 64 * 1024 + 1;
      const std::string body(1024, 'x');
      std::string request = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
      append_h2_frame(request, 4, 0, 0, "");
      append_h2_frame(
          request,
          1,
          0x4,
          1,
          h2_request_header_block("POST", "/", "localhost", {{"content-length", std::to_string(declared_body_size)}}));
      append_h2_frame(request, 0, 0x1, 1, body);

      BufferedConnection connection(std::move(request));
      const auto request_executor = std::make_shared<PausingBodyRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (h2_output_contains_goaway_error(connection.output(), NGHTTP2_PROTOCOL_ERROR))
      {
        fail("HTTP/2 paused request body produced a connection-level protocol GOAWAY");
      }
      const std::size_t accepted_body_bytes = request_executor->accepted_body_bytes.load(std::memory_order_acquire);
      if (accepted_body_bytes != body.size())
      {
        fail(
            "HTTP/2 paused request body bytes were not retried and delivered: expected " +
            std::to_string(body.size()) +
            ", got " +
            std::to_string(accepted_body_bytes));
      }
    }

    void test_http2_rejects_conflicting_authority_and_host()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "example.test", {{"host", "other.test"}})));
      const auto request_executor = std::make_shared<EchoHostRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (!h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_PROTOCOL_ERROR))
      {
        fail("HTTP/2 conflicting authority and host request was not reset");
      }
    }

    void test_http2_accepts_matching_authority_and_host()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "example.test", {{"host", "EXAMPLE.test"}})));
      const auto request_executor = std::make_shared<EchoHostRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_PROTOCOL_ERROR) ||
          connection.output().find("EXAMPLE.test") == std::string::npos)
      {
        fail("HTTP/2 matching authority and host request was not served");
      }
    }

    void test_http2_synthesizes_host_from_authority()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "example.test")));
      const auto request_executor = std::make_shared<EchoHostRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_PROTOCOL_ERROR) ||
          connection.output().find("example.test") == std::string::npos)
      {
        fail("HTTP/2 authority was not synthesized as Rack host");
      }
    }

    void test_http2_streams_file_backed_response_body()
    {
      class FileBackedResponseRequestExecutor final : public Vajra::request::RequestExecutor
      {
      public:
        explicit FileBackedResponseRequestExecutor(std::string body)
            : body_(std::move(body))
        {
        }

        std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
        {
          Vajra::response::Response response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "application/octet-stream"}},
              "",
              Vajra::response::ConnectionBehavior::close};
          response.body_file = response_body_file_from(body_);
          return response;
        }

      private:
        std::string body_;
      };

      const std::string body = std::string(16 * 1024, 'a') + std::string(16 * 1024, 'b');
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "localhost")));
      const auto request_executor = std::make_shared<FileBackedResponseRequestExecutor>(body);
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      const std::vector<std::pair<std::string, std::string>> headers =
          h2_response_headers_from(connection.output(), 1);
      if (!h2_headers_contain(headers, "content-length", std::to_string(body.size())))
      {
        fail("HTTP/2 file-backed response did not advertise the correct content length");
      }
      if (h2_response_body_from(connection.output(), 1) != body)
      {
        fail("HTTP/2 file-backed response did not send exact response bytes");
      }
    }

    void test_http2_resumes_deferred_file_backed_response_body_after_window_update()
    {
      class FileBackedResponseRequestExecutor final : public Vajra::request::RequestExecutor
      {
      public:
        explicit FileBackedResponseRequestExecutor(std::string body)
            : body_(std::move(body))
        {
        }

        std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
        {
          Vajra::response::Response response{
              Vajra::response::Status{200, "OK"},
              {Vajra::response::Header{"Content-Type", "application/octet-stream"}},
              "",
              Vajra::response::ConnectionBehavior::close};
          response.body_file = response_body_file_from(body_);
          return response;
        }

      private:
        std::string body_;
      };

      class FlowControlledConnection final : public Vajra::transport::Connection
      {
      public:
        FlowControlledConnection(std::string initial_input, std::string window_update_input)
            : initial_input_(std::move(initial_input)),
              window_update_input_(std::move(window_update_input))
        {
        }

        int fd() const override
        {
          return -1;
        }

        bool wait_readable(int) override
        {
          return initial_read_offset_ < initial_input_.size() ||
                 (!window_update_sent_ && h2_output_contains_data_frame(output_, 1));
        }

        ssize_t read(char *buffer, std::size_t length) override
        {
          if (initial_read_offset_ < initial_input_.size())
          {
            const std::size_t available = initial_input_.size() - initial_read_offset_;
            const std::size_t copied = std::min(length, available);
            std::memcpy(buffer, initial_input_.data() + initial_read_offset_, copied);
            initial_read_offset_ += copied;
            return static_cast<ssize_t>(copied);
          }
          if (!window_update_sent_ && h2_output_contains_data_frame(output_, 1))
          {
            const std::size_t copied = std::min(length, window_update_input_.size());
            std::memcpy(buffer, window_update_input_.data(), copied);
            window_update_sent_ = true;
            return static_cast<ssize_t>(copied);
          }
          return 0;
        }

        ssize_t write(const char *buffer, std::size_t length) override
        {
          output_.append(buffer, length);
          return static_cast<ssize_t>(length);
        }

        std::string protocol() const override
        {
          return "h2";
        }

        bool tls() const override
        {
          return true;
        }

        const std::string &output() const
        {
          return output_;
        }

      private:
        std::string initial_input_;
        std::string window_update_input_;
        std::string output_;
        std::size_t initial_read_offset_ = 0;
        bool window_update_sent_ = false;
      };

      std::string initial_window_settings;
      initial_window_settings.push_back('\0');
      initial_window_settings.push_back(static_cast<char>(4));
      append_h2_uint32(initial_window_settings, 1024);

      std::string initial_input = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
      append_h2_frame(initial_input, 4, 0, 0, initial_window_settings);
      append_h2_frame(initial_input, 1, 0x5, 1, h2_request_header_block("GET", "/", "localhost"));

      std::string window_update_payload;
      append_h2_uint32(window_update_payload, 256 * 1024);
      std::string window_update_input;
      append_h2_frame(window_update_input, 8, 0, 0, window_update_payload);
      append_h2_frame(window_update_input, 8, 0, 1, window_update_payload);

      const std::string body = std::string(96 * 1024, 'f');
      FlowControlledConnection connection(initial_input, window_update_input);
      const auto request_executor = std::make_shared<FileBackedResponseRequestExecutor>(body);
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (h2_response_body_from(connection.output(), 1) != body)
      {
        fail("HTTP/2 file-backed response did not resume after WINDOW_UPDATE");
      }
    }

    void test_http2_rejects_invalid_response_headers()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "localhost")));
      const auto request_executor = std::make_shared<InvalidResponseRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      const std::vector<std::pair<std::string, std::string>> headers =
          h2_response_headers_from(connection.output(), 1);
      if (!h2_headers_contain(headers, ":status", "500") ||
          h2_response_body_from(connection.output(), 1).find("Internal Server Error") == std::string::npos)
      {
        fail("HTTP/2 invalid response headers did not produce the fallback response");
      }
    }

    void test_http2_suppresses_head_response_data()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("HEAD", "/", "localhost")));
      const auto request_executor = std::make_shared<HeadBodyRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (h2_output_contains_data_frame(connection.output(), 1))
      {
        fail("HTTP/2 HEAD response emitted a DATA frame");
      }
    }

    void test_http2_rejects_no_body_status_response_body()
    {
      for (const int status : {204, 304})
      {
        BufferedConnection connection(h2_request_bytes(
            h2_request_header_block("GET", "/", "localhost")));
        const auto request_executor = std::make_shared<NoBodyStatusRequestExecutor>(status);
        const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
        Vajra::request::Http2Session session(
            connection,
            Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
            Vajra::request::Http2Config{},
            request_executor,
            execution_pool);
        session.run();

        const std::vector<std::pair<std::string, std::string>> headers =
            h2_response_headers_from(connection.output(), 1);
        if (!h2_headers_contain(headers, ":status", "500") ||
            h2_response_body_from(connection.output(), 1).find("Internal Server Error") == std::string::npos)
        {
          fail("HTTP/2 no-body status response with a body did not produce the fallback response");
        }
      }
    }

    void test_http2_strips_connection_specific_response_headers()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block("GET", "/", "localhost")));
      const auto request_executor = std::make_shared<ForbiddenHttp2ResponseHeadersRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      const std::vector<std::pair<std::string, std::string>> headers =
          h2_response_headers_from(connection.output(), 1);
      if (!h2_headers_contain(headers, "x-allowed", "yes"))
      {
        fail("HTTP/2 response header filter removed an allowed response header");
      }
      if (h2_headers_contain(headers, "upgrade", "websocket") ||
          h2_headers_contain(headers, "keep-alive", "timeout=5") ||
          h2_headers_contain(headers, "proxy-connection", "keep-alive"))
      {
        fail("HTTP/2 response header filter preserved a connection-specific response header");
      }
    }

    void test_http2_strips_connection_specific_extended_connect_accept_headers()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block(
              "CONNECT",
              "/",
              "localhost",
              {{":protocol", "websocket"}})));
      const auto request_executor = std::make_shared<AcceptingTunnelRequestExecutor>();
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      const std::vector<std::pair<std::string, std::string>> headers =
          h2_response_headers_from(connection.output(), 1);
      if (!h2_headers_contain(headers, "x-allowed", "yes"))
      {
        fail("HTTP/2 extended CONNECT accept header filter removed an allowed response header");
      }
      if (h2_headers_contain(headers, "upgrade", "websocket") ||
          h2_headers_contain(headers, "keep-alive", "timeout=5") ||
          h2_headers_contain(headers, "proxy-connection", "keep-alive"))
      {
        fail("HTTP/2 extended CONNECT accept preserved a connection-specific response header");
      }
    }

    void test_http2_rejects_invalid_extended_connect_accept_headers()
    {
      BufferedConnection connection(h2_request_bytes(
          h2_request_header_block(
              "CONNECT",
              "/",
              "localhost",
              {{":protocol", "websocket"}})));
      const auto request_executor = std::make_shared<AcceptingTunnelRequestExecutor>(true);
      const auto execution_pool = std::make_shared<Vajra::request::Http2ExecutionPool>(1);
      Vajra::request::Http2Session session(
          connection,
          Vajra::request::SocketContext{"127.0.0.1", 12'345, "127.0.0.1", 3000, "https"},
          Vajra::request::Http2Config{},
          request_executor,
          execution_pool);
      session.run();

      if (!h2_output_contains_rst_stream_error(connection.output(), 1, NGHTTP2_INTERNAL_ERROR))
      {
        fail("HTTP/2 extended CONNECT invalid accept headers were not rejected");
      }
    }
  }

  void run_response_tests()
  {
    test_response_serializer_serializes_status_headers_and_body();
    test_response_serializer_serializes_chunked_response_storage();
    test_response_serializer_and_writer_stream_file_backed_body();
    test_response_serializer_allows_empty_reason_phrase();
    test_response_serializer_omits_content_length_for_no_body_status();
    test_response_serializer_emits_connection_close_only_when_requested();
    test_response_serializer_preserves_header_order();
    test_response_serializer_rejects_invalid_header_name();
    test_response_serializer_rejects_invalid_header_value();
    test_response_serializer_rejects_forbidden_framing_headers();
    test_response_serializer_rejects_other_control_characters();
    test_response_serializer_rejects_invalid_status();
    test_response_serializer_rejects_bodies_for_no_body_statuses();
    test_response_writer_sends_structured_success_response();
    test_response_writer_sends_structured_error_response();
    test_response_writer_send_returns_false_on_serialization_failure();
    test_request_processor_keeps_connection_open_for_sequential_requests();
    test_request_processor_handles_pipelined_read_ahead_without_losing_the_next_request();
    test_request_processor_closes_after_parse_error_response();
    test_request_processor_keeps_connection_open_after_request_body_framing();
    test_request_processor_closes_http_1_0_without_keep_alive();
    test_request_processor_keeps_http_1_0_alive_when_requested();
    test_request_body_reader_preserves_buffered_suffix_after_fixed_length_body();
    test_request_body_reader_preserves_buffered_suffix_after_chunked_body();
    test_request_body_reader_accepts_mixed_case_chunked_transfer_encoding_token();
    test_request_body_reader_rejects_unsupported_transfer_encoding_lists();
    test_request_body_reader_rejects_strict_content_length_violations();
    test_request_body_reader_rejects_chunk_size_trailing_junk();
    test_request_body_reader_rejects_oversized_content_length_body();
    test_request_body_reader_uses_direct_execution_for_small_content_length_body();
    test_request_body_reader_streams_large_content_length_body_until_buffered();
    test_request_body_reader_streams_chunked_body();
    test_request_body_reader_rejects_overlong_chunk_metadata();
    test_request_body_reader_treats_chunk_metadata_at_limit_without_crlf_as_incomplete();
    test_request_body_reader_treats_transport_failures_as_incomplete_reads();
    test_request_processor_reads_fragmented_fixed_length_request_body();
    test_request_processor_decodes_chunked_request_body();
    test_request_processor_decodes_chunked_request_body_with_extensions_and_trailers();
    test_request_processor_uses_direct_execution_for_prebuffered_bodies();
    test_request_processor_rejects_conflicting_request_body_framing();
    test_request_processor_rejects_malformed_chunked_request_body();
    test_request_processor_rejects_oversized_request_body();
    test_request_processor_closes_quietly_when_request_body_is_incomplete();
    test_request_processor_times_out_stalled_fixed_length_request_body();
    test_request_processor_times_out_stalled_chunked_request_body();
    test_request_processor_suppresses_head_response_body();
    test_request_processor_returns_internal_server_error_when_executor_raises();
    test_request_processor_returns_bad_request_when_executor_raises_head_error();
    test_request_processor_returns_internal_server_error_when_executor_response_is_invalid();
    test_request_processor_strips_executor_framing_headers_before_sending();
    test_request_processor_does_not_mix_partial_internal_trace_context_with_traceparent();
    test_http2_async_unknown_exception_completes_session_shutdown();
    test_http2_queue_capacity_error_emits_503_and_goaway();
    test_http2_pending_execution_limit_rejects_second_stream();
    test_http2_prior_knowledge_initial_bytes_serves_request();
    test_http2_upgrade_request_serves_stream_one();
    test_http2_rejects_short_content_length_body();
    test_http2_rejects_long_content_length_body();
    test_http2_paused_request_body_bytes_resume_without_goaway();
    test_http2_rejects_conflicting_authority_and_host();
    test_http2_accepts_matching_authority_and_host();
    test_http2_synthesizes_host_from_authority();
    test_http2_streams_file_backed_response_body();
    test_http2_resumes_deferred_file_backed_response_body_after_window_update();
    test_http2_rejects_invalid_response_headers();
    test_http2_suppresses_head_response_data();
    test_http2_rejects_no_body_status_response_body();
    test_http2_strips_connection_specific_response_headers();
    test_http2_strips_connection_specific_extended_connect_accept_headers();
    test_http2_rejects_invalid_extended_connect_accept_headers();
  }
}
