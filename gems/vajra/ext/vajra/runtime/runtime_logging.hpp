// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUNTIME_LOGGING_HPP
#define VAJRA_RUNTIME_RUNTIME_LOGGING_HPP

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace runtime
  {
    std::string utc_timestamp();
    std::string runtime_environment_name();
    bool debug_logging_enabled(const std::string &log_level);
    void log_runtime_banner_start(
        const std::string &host,
        int port,
        int workers,
        std::size_t min_threads,
        std::size_t max_threads);
    void log_worker_booted(int worker_index, int pid, double boot_seconds);
    void log_runtime_shutdown_begin();
    void log_runtime_shutdown_complete();
  }
}

#endif
