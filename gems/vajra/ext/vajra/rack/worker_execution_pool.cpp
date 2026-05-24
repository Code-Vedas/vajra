// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/worker_execution_pool.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

Vajra::rack::WorkerExecutionPool::WorkerExecutionPool(std::size_t max_active_executions)
    : max_active_executions_(max_active_executions)
{
}

bool Vajra::rack::WorkerExecutionPool::enqueue(const std::shared_ptr<WorkerExecutionTask> &task)
{
  bool accepted = true;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (input_closed_ || aborted_)
    {
      accepted = false;
    }
    else
    {
      queued_tasks_.push_back(task);
      condition_.notify_one();
    }
  }

  if (!accepted)
  {
    task->cancel_due_to_pool_abort();
  }

  return accepted;
}

std::shared_ptr<Vajra::rack::WorkerExecutionTask> Vajra::rack::WorkerExecutionPool::wait_for_task()
{
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this]() {
    return !queued_tasks_.empty() || input_closed_ || aborted_;
  });

  if (queued_tasks_.empty())
  {
    return nullptr;
  }

  if (active_execution_count_ >= max_active_executions_)
  {
    // Treat capacity oversubscription as an internal invariant violation.
    // The queued task stays at the front so the caller can fail fast and
    // tear down the worker loop instead of silently dropping work.
    throw std::logic_error("worker execution pool exceeded configured active execution capacity");
  }
  std::shared_ptr<WorkerExecutionTask> task = std::move(queued_tasks_.front());
  queued_tasks_.pop_front();
  ++active_execution_count_;
  if (active_execution_count_ > max_observed_active_execution_count_)
  {
    max_observed_active_execution_count_ = active_execution_count_;
  }
  return task;
}

void Vajra::rack::WorkerExecutionPool::finish_task()
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (active_execution_count_ > 0)
    {
      --active_execution_count_;
    }
  }

  condition_.notify_one();
}

void Vajra::rack::WorkerExecutionPool::close_input()
{
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    input_closed_ = true;
  }
  condition_.notify_all();
}

void Vajra::rack::WorkerExecutionPool::abort()
{
  std::vector<std::shared_ptr<WorkerExecutionTask>> canceled_tasks;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_)
    {
      return;
    }

    aborted_ = true;
    input_closed_ = true;
    canceled_tasks.assign(queued_tasks_.begin(), queued_tasks_.end());
    queued_tasks_.clear();
  }

  for (const auto &task : canceled_tasks)
  {
    task->cancel_due_to_pool_abort();
  }

  condition_.notify_all();
}

std::size_t Vajra::rack::WorkerExecutionPool::max_active_executions() const
{
  return max_active_executions_;
}

std::size_t Vajra::rack::WorkerExecutionPool::active_execution_count() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return active_execution_count_;
}

std::size_t Vajra::rack::WorkerExecutionPool::queued_task_count() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return queued_tasks_.size();
}

std::size_t Vajra::rack::WorkerExecutionPool::max_observed_active_execution_count() const
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return max_observed_active_execution_count_;
}
