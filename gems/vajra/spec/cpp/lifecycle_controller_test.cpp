// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "lifecycle/lifecycle_controller.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <stdexcept>
#include <vector>

namespace VajraSpecCpp
{
  namespace
  {
    void test_lifecycle_controller_tracks_normal_transitions()
    {
      Vajra::lifecycle::Controller controller;
      std::vector<Vajra::lifecycle::HookPoint> hook_points;
      controller.set_observer([&](Vajra::lifecycle::HookPoint hook_point, const Vajra::lifecycle::Snapshot &) {
        hook_points.push_back(hook_point);
      });

      controller.begin_startup();
      controller.mark_listening(7, 3000);
      controller.mark_serving();
      controller.request_stop(Vajra::lifecycle::StopReason::programmatic_stop);
      controller.finish_stop();

      const Vajra::lifecycle::Snapshot snapshot = controller.snapshot();
      if (snapshot.state != Vajra::lifecycle::State::stopped ||
          snapshot.last_stop_reason != Vajra::lifecycle::StopReason::programmatic_stop ||
          snapshot.listener_owned)
      {
        fail("lifecycle controller did not preserve the expected stopped snapshot");
      }

      const std::vector<Vajra::lifecycle::HookPoint> expected_hooks = {
          Vajra::lifecycle::HookPoint::boot_complete,
          Vajra::lifecycle::HookPoint::serving_entered,
          Vajra::lifecycle::HookPoint::drain_requested,
          Vajra::lifecycle::HookPoint::stop_completed,
      };
      if (hook_points != expected_hooks)
      {
        fail("lifecycle controller did not emit the expected hook sequence");
      }
    }

    void test_lifecycle_controller_rejects_invalid_transitions()
    {
      Vajra::lifecycle::Controller controller;

      try
      {
        controller.mark_serving();
        fail("lifecycle controller accepted serving before startup");
      }
      catch (const std::logic_error &)
      {
      }

      controller.begin_startup();

      try
      {
        controller.finish_stop();
        fail("lifecycle controller accepted finish_stop before any stop request");
      }
      catch (const std::logic_error &)
      {
      }
    }

    void test_lifecycle_controller_preserves_failure_state()
    {
      Vajra::lifecycle::Controller controller;
      controller.begin_startup();
      controller.mark_failed(Vajra::lifecycle::StopReason::startup_failure);
      controller.finish_stop();

      const Vajra::lifecycle::Snapshot snapshot = controller.snapshot();
      if (snapshot.state != Vajra::lifecycle::State::failed ||
          snapshot.last_stop_reason != Vajra::lifecycle::StopReason::startup_failure ||
          snapshot.listener_owned)
      {
        fail("lifecycle controller did not preserve failed terminal state");
      }
    }

    void test_lifecycle_controller_stop_requests_are_idempotent()
    {
      Vajra::lifecycle::Controller controller;
      controller.begin_startup();
      controller.mark_listening(7, 3000);
      controller.request_stop(Vajra::lifecycle::StopReason::signal_shutdown);
      controller.request_stop(Vajra::lifecycle::StopReason::programmatic_stop);
      controller.finish_stop();

      const Vajra::lifecycle::Snapshot snapshot = controller.snapshot();
      if (snapshot.last_stop_reason != Vajra::lifecycle::StopReason::signal_shutdown)
      {
        fail("lifecycle controller did not preserve the first stop reason");
      }
    }

    void test_lifecycle_controller_ignores_serving_transition_after_drain_begins()
    {
      Vajra::lifecycle::Controller controller;
      controller.begin_startup();
      controller.mark_listening(7, 3000);
      controller.request_stop(Vajra::lifecycle::StopReason::programmatic_stop);
      controller.mark_serving();

      const Vajra::lifecycle::Snapshot snapshot = controller.snapshot();
      if (snapshot.state != Vajra::lifecycle::State::draining ||
          snapshot.last_stop_reason != Vajra::lifecycle::StopReason::programmatic_stop)
      {
        fail("lifecycle controller did not preserve draining state when serving raced with stop");
      }
    }

    void test_lifecycle_controller_tracks_stop_before_start_separately_from_stopped_state()
    {
      Vajra::lifecycle::Controller controller;
      controller.request_stop(Vajra::lifecycle::StopReason::programmatic_stop);

      const Vajra::lifecycle::Snapshot requested_snapshot = controller.snapshot();
      if (requested_snapshot.state != Vajra::lifecycle::State::stopped ||
          requested_snapshot.last_stop_reason != Vajra::lifecycle::StopReason::programmatic_stop ||
          !controller.consume_pending_stop_before_start())
      {
        fail("lifecycle controller did not preserve stop-before-start request separately");
      }

      if (controller.consume_pending_stop_before_start())
      {
        fail("lifecycle controller consumed stop-before-start request more than once");
      }

      controller.request_stop(Vajra::lifecycle::StopReason::programmatic_stop);
      if (controller.consume_pending_stop_before_start())
      {
        fail("lifecycle controller re-entered pending stop after already-stopped stop request");
      }
    }
  }

  void run_lifecycle_controller_tests()
  {
    test_lifecycle_controller_tracks_normal_transitions();
    test_lifecycle_controller_rejects_invalid_transitions();
    test_lifecycle_controller_preserves_failure_state();
    test_lifecycle_controller_stop_requests_are_idempotent();
    test_lifecycle_controller_ignores_serving_transition_after_drain_begins();
    test_lifecycle_controller_tracks_stop_before_start_separately_from_stopped_state();
  }
}
