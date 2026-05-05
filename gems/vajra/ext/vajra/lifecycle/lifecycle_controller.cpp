// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "lifecycle/lifecycle_controller.hpp"

#include <stdexcept>
#include <utility>

namespace Vajra
{
  namespace lifecycle
  {
    Controller::Controller()
        : state_(State::stopped),
          last_stop_reason_(StopReason::none),
          listener_owned_(false),
          pending_stop_before_start_(false),
          port_(-1),
          listener_fd_(-1),
          observer_()
    {
    }

    void Controller::begin_startup()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != State::stopped)
      {
        throw std::logic_error("lifecycle startup can only begin from stopped");
      }

      state_ = State::booting;
      last_stop_reason_ = StopReason::none;
      listener_owned_ = false;
      pending_stop_before_start_ = false;
      port_ = -1;
      listener_fd_ = -1;
    }

    void Controller::mark_listening(int listener_fd, int port)
    {
      Snapshot snapshot_value;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::booting)
        {
          throw std::logic_error("lifecycle can only enter listening from booting");
        }

        state_ = State::listening;
        listener_owned_ = true;
        port_ = port;
        listener_fd_ = listener_fd;
        snapshot_value = snapshot_unlocked();
      }

      notify(HookPoint::boot_complete, snapshot_value);
    }

    void Controller::mark_serving()
    {
      Snapshot snapshot_value;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::serving || state_ == State::draining)
        {
          return;
        }

        if (state_ != State::listening)
        {
          throw std::logic_error("lifecycle can only enter serving from listening");
        }

        state_ = State::serving;
        snapshot_value = snapshot_unlocked();
      }

      notify(HookPoint::serving_entered, snapshot_value);
    }

    void Controller::request_stop(StopReason reason)
    {
      if (reason == StopReason::none)
      {
        throw std::logic_error("lifecycle stop requests require an explicit reason");
      }

      Snapshot snapshot_value;
      bool notify_observer = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::failed || state_ == State::draining)
        {
          if (last_stop_reason_ == StopReason::none)
          {
            last_stop_reason_ = reason;
          }
          return;
        }

        if (state_ == State::stopped)
        {
          if (last_stop_reason_ != StopReason::none)
          {
            return;
          }

          last_stop_reason_ = reason;
          pending_stop_before_start_ = true;
        }
        else
        {
          if (state_ != State::booting && state_ != State::listening && state_ != State::serving)
          {
            throw std::logic_error("lifecycle stop can only be requested from active states");
          }

          state_ = State::draining;
          last_stop_reason_ = reason;
          pending_stop_before_start_ = false;
          snapshot_value = snapshot_unlocked();
          notify_observer = true;
        }
      }

      if (notify_observer)
      {
        notify(HookPoint::drain_requested, snapshot_value);
      }
    }

    void Controller::finish_stop()
    {
      Snapshot snapshot_value;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::failed)
        {
          listener_owned_ = false;
          pending_stop_before_start_ = false;
          listener_fd_ = -1;
          snapshot_value = snapshot_unlocked();
        }
        else
        {
          if (state_ != State::listening && state_ != State::serving && state_ != State::draining)
          {
            throw std::logic_error("lifecycle stop can only finish from an active state");
          }

          state_ = State::stopped;
          listener_owned_ = false;
          pending_stop_before_start_ = false;
          listener_fd_ = -1;
          snapshot_value = snapshot_unlocked();
        }
      }

      notify(HookPoint::stop_completed, snapshot_value);
    }

    void Controller::mark_failed(StopReason reason)
    {
      if (reason != StopReason::startup_failure && reason != StopReason::listener_failure)
      {
        throw std::logic_error("lifecycle failure requires a failure stop reason");
      }

      Snapshot snapshot_value;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::failed;
        last_stop_reason_ = reason;
        listener_owned_ = false;
        pending_stop_before_start_ = false;
        listener_fd_ = -1;
        snapshot_value = snapshot_unlocked();
      }

      notify(HookPoint::failed_entered, snapshot_value);
    }

    Snapshot Controller::snapshot() const
    {
      std::lock_guard<std::mutex> lock(mutex_);
      return snapshot_unlocked();
    }

    bool Controller::consume_pending_stop_before_start()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != State::stopped || !pending_stop_before_start_)
      {
        return false;
      }

      pending_stop_before_start_ = false;
      return true;
    }

    Snapshot Controller::snapshot_unlocked() const
    {
      return Snapshot{
          state_,
          last_stop_reason_,
          listener_owned_,
          port_,
          listener_fd_,
      };
    }

    void Controller::set_observer(Observer observer)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observer_ = std::move(observer);
    }

    void Controller::notify(HookPoint hook_point, const Snapshot &snapshot_value)
    {
      Observer observer;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        observer = observer_;
      }

      if (observer)
      {
        observer(hook_point, snapshot_value);
      }
    }
  }
}
