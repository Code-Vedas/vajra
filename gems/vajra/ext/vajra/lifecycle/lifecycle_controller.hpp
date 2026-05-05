// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_LIFECYCLE_CONTROLLER_HPP
#define VAJRA_LIFECYCLE_CONTROLLER_HPP

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>

namespace Vajra
{
  namespace lifecycle
  {
    enum class State
    {
      booting,
      listening,
      serving,
      draining,
      stopped,
      failed,
    };

    enum class StopReason
    {
      none,
      programmatic_stop,
      signal_shutdown,
      startup_failure,
      listener_failure,
    };

    enum class HookPoint
    {
      boot_complete,
      serving_entered,
      drain_requested,
      stop_completed,
      failed_entered,
    };

    struct Snapshot
    {
      State state;
      StopReason last_stop_reason;
      bool listener_owned;
      int port;
      int listener_fd;
    };

    class Controller
    {
    public:
      using Observer = std::function<void(HookPoint, const Snapshot &)>;

      Controller();

      void begin_startup();
      void mark_listening(int listener_fd, int port);
      void mark_serving();
      void request_stop(StopReason reason);
      void finish_stop();
      void mark_failed(StopReason reason);
      bool consume_pending_stop_before_start();

      Snapshot snapshot() const;
      void set_observer(Observer observer);

    private:
      Snapshot snapshot_unlocked() const;
      void notify(HookPoint hook_point, const Snapshot &snapshot);

      mutable std::mutex mutex_;
      State state_;
      StopReason last_stop_reason_;
      bool listener_owned_;
      bool pending_stop_before_start_;
      int port_;
      int listener_fd_;
      Observer observer_;
    };
  }
}

#endif
