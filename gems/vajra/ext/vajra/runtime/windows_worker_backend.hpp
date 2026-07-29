// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_WINDOWS_WORKER_BACKEND_HPP
#define VAJRA_RUNTIME_WINDOWS_WORKER_BACKEND_HPP

#ifdef _WIN32

#include "runtime/runtime_config.hpp"
#include "runtime/runtime_state.hpp"
#include "runtime/worker_pool.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace Vajra::runtime
{
  bool windows_worker_bootstrap_present();
  void run_windows_worker_process(const RuntimeConfig &invocation_config);

  class WindowsWorkerSupervisor final
  {
  public:
    WindowsWorkerSupervisor(RuntimeConfig config, RuntimeState *runtime_state);
    ~WindowsWorkerSupervisor();

    WindowsWorkerSupervisor(const WindowsWorkerSupervisor &) = delete;
    WindowsWorkerSupervisor &operator=(const WindowsWorkerSupervisor &) = delete;

    void start();
    void run();
    void request_stop();
    std::vector<std::shared_ptr<SharedWorkerState>> worker_states() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
  };
}

#endif
#endif
