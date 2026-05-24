// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUNTIME_CONFIG_HPP
#define VAJRA_RUNTIME_RUNTIME_CONFIG_HPP

#include "ruby.h"

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace runtime
  {
    struct RuntimeConfig
    {
      std::string host;
      int port;
      int workers;
      std::size_t min_threads;
      std::size_t max_threads;
      std::size_t max_connections;
      std::size_t queue_capacity;
      std::string scheduler_policy;
      std::size_t max_request_head_bytes;
      std::size_t request_timeout_seconds;
      int request_head_timeout_seconds;
      int first_data_timeout_seconds;
      int persistent_timeout_seconds;
      int worker_timeout_seconds;
      std::string log_level;
      std::string access_log;
      std::string error_log;
      bool structured_logs;
      std::string stats_path;
      std::string metrics_endpoint;
    };

    class RuntimeConfigLoader final
    {
    public:
      static void initialize_ids();
      static RuntimeConfig configured_runtime(VALUE options);
    };
  }
}

#endif
