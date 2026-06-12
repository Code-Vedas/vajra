// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/http2_stream.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
  using Vajra::rack::Http2StreamState;

  void expect_true(bool condition, const std::string &message)
  {
    if (!condition)
    {
      VajraSpecCpp::fail(message);
    }
  }

  void test_append_respects_high_watermark()
  {
    Http2StreamState state;
    state.high_watermark = 4;
    state.low_watermark = 2;

    expect_true(Vajra::rack::http2_stream_try_append_inbound(&state, "abcd", 4),
        "first inbound append should fit the high watermark");
    expect_true(!Vajra::rack::http2_stream_try_append_inbound(&state, "e", 1),
        "inbound append above high watermark should be refused");

    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.inbound_chunks.pop_front();
      state.inbound_bytes = 0;
      state.consumed_since_last_observation += 4;
    }
    state.capacity_condition.notify_all();

    expect_true(Vajra::rack::http2_stream_try_append_inbound(&state, "e", 1),
        "inbound append should resume after consumer drains bytes");
    expect_true(Vajra::rack::http2_stream_take_consumed_bytes(&state) == 4,
        "consumed-byte observation should report drained inbound bytes exactly once");
    expect_true(Vajra::rack::http2_stream_take_consumed_bytes(&state) == 0,
        "consumed-byte observation should reset after being read");
  }

  void test_finish_inbound_wakes_waiters()
  {
    Http2StreamState state;
    std::atomic<bool> observed_peer_close{false};

    std::thread waiter([&state, &observed_peer_close]() {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.data_condition.wait(lock, [&state]() { return state.peer_closed; });
      observed_peer_close.store(true);
    });

    Vajra::rack::http2_stream_finish_inbound(&state);
    waiter.join();

    expect_true(observed_peer_close.load(), "finish_inbound should wake blocked inbound readers");
  }

  void test_reset_closes_and_wakes_waiters()
  {
    Http2StreamState state;
    std::atomic<bool> observed_reset{false};

    std::thread waiter([&state, &observed_reset]() {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.data_condition.wait(lock, [&state]() { return state.reset || state.closed; });
      observed_reset.store(state.reset && state.reset_error_code == 8);
    });

    Vajra::rack::http2_stream_reset(&state, 8);
    waiter.join();

    expect_true(observed_reset.load(), "reset should wake blocked readers with the reset code");
    expect_true(Vajra::rack::http2_stream_closed(&state), "reset should mark the stream closed");
  }

  void test_accept_is_observed_once()
  {
    Http2StreamState state;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.accepted = true;
      state.accept_status = 201;
      state.accept_headers.push_back({"content-type", "application/octet-stream"});
    }

    int status = 0;
    std::vector<Vajra::response::Header> headers;
    expect_true(Vajra::rack::http2_stream_take_accept(&state, status, headers),
        "first accept observation should report accepted stream");
    expect_true(status == 201, "accept observation should preserve status");
    expect_true(headers.size() == 1 && headers.front().name == "content-type",
        "accept observation should preserve response headers");
    expect_true(!Vajra::rack::http2_stream_take_accept(&state, status, headers),
        "accept observation should be single-use");
  }

  void test_outbound_drain_releases_capacity_only_after_provider_consumes_bytes()
  {
    Http2StreamState state;
    state.high_watermark = 6;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.outbound_chunks.push_back("one");
      state.outbound_chunks.push_back("two");
      state.outbound_bytes = 6;
    }

    expect_true(Vajra::rack::http2_stream_has_outbound(&state),
        "outbound availability should reflect queued chunks before provider drain");
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      expect_true(state.outbound_bytes == 6,
          "outbound bytes should remain charged while queued for the provider");
    }

    std::uint8_t buffer[4] = {};
    const std::size_t first_read = Vajra::rack::http2_stream_drain_outbound(&state, buffer, sizeof(buffer));
    expect_true(first_read == 4, "first provider drain should copy the requested byte count");
    expect_true(std::string(reinterpret_cast<char *>(buffer), first_read) == "onet",
        "provider drain should preserve outbound byte order across chunks");
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      expect_true(state.outbound_bytes == 2,
          "outbound bytes should be released only as the provider consumes DATA bytes");
    }

    std::uint8_t remaining[8] = {};
    const std::size_t second_read = Vajra::rack::http2_stream_drain_outbound(&state, remaining, sizeof(remaining));
    expect_true(second_read == 2, "second provider drain should return remaining queued bytes");
    expect_true(std::string(reinterpret_cast<char *>(remaining), second_read) == "wo",
        "second provider drain should preserve remaining outbound bytes");

    {
      std::lock_guard<std::mutex> lock(state.mutex);
      expect_true(state.outbound_chunks.empty(), "provider drain should clear fully consumed chunks");
      expect_true(state.outbound_bytes == 0, "provider drain should release all consumed outbound bytes");
    }
    expect_true(!Vajra::rack::http2_stream_has_outbound(&state),
        "outbound availability should be false after provider drain");
  }

  void test_app_closed_reports_closed_for_provider_eof()
  {
    Http2StreamState state;
    expect_true(!Vajra::rack::http2_stream_app_closed(&state),
        "fresh stream should not report app-side closure");
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.app_closed = true;
      state.closed = true;
    }
    expect_true(Vajra::rack::http2_stream_app_closed(&state),
        "app closure should be observable by the provider");
  }
}

void VajraSpecCpp::run_http2_stream_tests()
{
  test_append_respects_high_watermark();
  test_finish_inbound_wakes_waiters();
  test_reset_closes_and_wakes_waiters();
  test_accept_is_observed_once();
  test_outbound_drain_releases_capacity_only_after_provider_consumes_bytes();
  test_app_closed_reports_closed_for_provider_eof();
}
