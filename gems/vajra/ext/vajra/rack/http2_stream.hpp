// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_HTTP2_STREAM_HPP
#define VAJRA_RACK_HTTP2_STREAM_HPP

#ifdef VAJRA_RUNTIME_TESTING
#ifndef VAJRA_RUBY_VALUE_STUB
#define VAJRA_RUBY_VALUE_STUB
using VALUE = unsigned long;
constexpr VALUE Qnil = 0;
#endif
#else
#include "ruby.h"
#endif

#include "response/response.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Vajra
{
  namespace rack
  {
    struct Http2StreamState
    {
      mutable std::mutex mutex;
      std::condition_variable data_condition;
      std::condition_variable capacity_condition;
      std::condition_variable event_condition;
      std::deque<std::string> inbound_chunks;
      std::deque<std::string> outbound_chunks;
      std::vector<Vajra::response::Header> accept_headers;
      std::size_t inbound_bytes = 0;
      std::size_t outbound_bytes = 0;
      std::size_t high_watermark = 1024 * 1024;
      std::size_t low_watermark = 512 * 1024;
      std::int32_t stream_id = 0;
      int accept_status = 200;
      std::uint32_t reset_error_code = 0;
      std::size_t consumed_since_last_observation = 0;
      std::string protocol;
      bool websocket = false;
      bool accepted = false;
      bool accept_observed = false;
      bool peer_closed = false;
      bool app_closed = false;
      bool reset = false;
      bool closed = false;
    };

    void initialize_http2_stream();
    VALUE create_http2_stream(std::shared_ptr<Http2StreamState> state);
    bool http2_stream_p(VALUE value);
    void http2_stream_append_inbound(Http2StreamState *state, const char *data, std::size_t length);
    bool http2_stream_try_append_inbound(Http2StreamState *state, const char *data, std::size_t length);
    void http2_stream_finish_inbound(Http2StreamState *state);
    void http2_stream_reset(Http2StreamState *state, std::uint32_t error_code);
    std::size_t http2_stream_take_consumed_bytes(Http2StreamState *state);
    bool http2_stream_has_outbound(Http2StreamState *state);
    bool http2_stream_app_closed(Http2StreamState *state);
    std::size_t http2_stream_drain_outbound(Http2StreamState *state, std::uint8_t *buffer, std::size_t length);
    bool http2_stream_take_accept(
        Http2StreamState *state,
        int &status,
        std::vector<Vajra::response::Header> &headers);
    bool http2_stream_closed(Http2StreamState *state);
  }
}

#endif
