// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_RUBY_RACK_TRANSPORT_HPP
#define VAJRA_RACK_RUBY_RACK_TRANSPORT_HPP

#include "rack/rack_request_executor.hpp"

#include <chrono>

namespace Vajra
{
  namespace rack
  {
    std::shared_ptr<const RackExecutionTransport> same_process_rack_execution_transport();
    void configure_same_process_rack_execution_threads(std::size_t max_threads);
    void ensure_same_process_rack_execution_threads_started();
    bool wait_for_same_process_rack_execution_idle(std::chrono::milliseconds timeout);
    void shutdown_same_process_rack_execution_threads();
    std::optional<Vajra::response::Response> execute_current_thread_rack_request(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        const std::string &request_body,
        int client_fd = -1);
    std::optional<Vajra::response::Response> execute_current_thread_rack_request(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        VALUE rack_input,
        int client_fd = -1,
        std::shared_ptr<Vajra::rack::NativeInputState> input_state = nullptr);
  }
}

#endif
