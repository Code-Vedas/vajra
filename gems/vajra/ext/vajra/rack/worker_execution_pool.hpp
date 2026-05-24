// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_WORKER_EXECUTION_POOL_HPP
#define VAJRA_RACK_WORKER_EXECUTION_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace Vajra
{
  namespace rack
  {
    class WorkerExecutionTask
    {
    public:
      virtual ~WorkerExecutionTask() = default;
      virtual void execute() = 0;
      virtual void cancel_due_to_pool_abort() = 0;
    };

    class WorkerExecutionPool final
    {
    public:
      explicit WorkerExecutionPool(std::size_t max_active_executions);

      bool enqueue(const std::shared_ptr<WorkerExecutionTask> &task);
      std::shared_ptr<WorkerExecutionTask> wait_for_task();
      void finish_task();
      void close_input();
      void abort();

      std::size_t max_active_executions() const;
      std::size_t active_execution_count() const;
      std::size_t queued_task_count() const;
      std::size_t max_observed_active_execution_count() const;

    private:
      const std::size_t max_active_executions_;
      mutable std::mutex mutex_;
      std::condition_variable condition_;
      std::deque<std::shared_ptr<WorkerExecutionTask>> queued_tasks_;
      std::size_t active_execution_count_ = 0;
      std::size_t max_observed_active_execution_count_ = 0;
      bool input_closed_ = false;
      bool aborted_ = false;
    };
  }
}

#endif
