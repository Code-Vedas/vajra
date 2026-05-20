// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_EXTENSION_HPP
#define VAJRA_EXTENSION_HPP

#include "server.hpp"
#include "request/request_head_error.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace VajraNative
{
  void begin_runtime_shutdown();
  bool shutdown_requested();
  void start(
      std::string host = "0.0.0.0",
      int port = 3000,
      int workers = 1,
      std::size_t min_threads = 1,
      std::size_t max_threads = 1,
      std::size_t max_connections = 10'000,
      std::size_t queue_capacity = static_cast<std::size_t>(std::numeric_limits<long>::max()),
      std::string scheduler_policy = "least_loaded",
      std::size_t max_request_head_bytes = Vajra::request::kDefaultMaxRequestHeadBytes,
      std::size_t request_timeout_seconds = 25,
      int request_head_timeout_seconds = 5,
      int first_data_timeout_seconds = 30,
      int persistent_timeout_seconds = 30,
      int worker_timeout_seconds = 60,
      std::string log_level = "info");
  void stop();
}

#endif
