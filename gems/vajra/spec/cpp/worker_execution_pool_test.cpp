// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/worker_execution_pool.hpp"
#include "test_support.hpp"
#include "test_suites.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
  using namespace std::chrono_literals;

  class BlockingTask final : public Vajra::rack::WorkerExecutionTask
  {
  public:
    BlockingTask(
        std::size_t id,
        std::vector<std::size_t> &started_order,
        std::mutex &started_mutex,
        std::condition_variable &started_condition,
        std::atomic<std::size_t> &started_count,
        bool block,
        bool &release_blocked_tasks,
        std::mutex &release_mutex,
        std::condition_variable &release_condition)
        : id_(id),
          started_order_(started_order),
          started_mutex_(started_mutex),
          started_condition_(started_condition),
          started_count_(started_count),
          block_(block),
          release_blocked_tasks_(release_blocked_tasks),
          release_mutex_(release_mutex),
          release_condition_(release_condition)
    {
    }

    void execute() override
    {
      {
        const std::lock_guard<std::mutex> lock(started_mutex_);
        started_order_.push_back(id_);
        started_count_.fetch_add(1, std::memory_order_release);
      }
      started_condition_.notify_all();

      if (!block_)
      {
        return;
      }

      std::unique_lock<std::mutex> lock(release_mutex_);
      release_condition_.wait(lock, [this]() { return release_blocked_tasks_; });
    }

    void cancel_due_to_pool_abort() override
    {
      canceled_.store(true, std::memory_order_release);
    }

    bool canceled() const
    {
      return canceled_.load(std::memory_order_acquire);
    }

  private:
    const std::size_t id_;
    std::vector<std::size_t> &started_order_;
    std::mutex &started_mutex_;
    std::condition_variable &started_condition_;
    std::atomic<std::size_t> &started_count_;
    const bool block_;
    bool &release_blocked_tasks_;
    std::mutex &release_mutex_;
    std::condition_variable &release_condition_;
    std::atomic_bool canceled_{false};
  };

  void run_pool_workers(
      Vajra::rack::WorkerExecutionPool &pool,
      std::size_t worker_count,
      std::vector<std::thread> &threads)
  {
    threads.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index)
    {
      threads.emplace_back([&pool]() {
        for (;;)
        {
          std::shared_ptr<Vajra::rack::WorkerExecutionTask> task = pool.wait_for_task();
          if (!task)
          {
            return;
          }

          task->execute();
          pool.finish_task();
        }
      });
    }
  }

  void join_threads(std::vector<std::thread> &threads)
  {
    for (std::thread &thread : threads)
    {
      if (thread.joinable())
      {
        thread.join();
      }
    }
  }

  void test_worker_execution_pool_never_exceeds_configured_capacity()
  {
    Vajra::rack::WorkerExecutionPool pool(2);
    std::vector<std::thread> threads;
    run_pool_workers(pool, 2, threads);

    std::vector<std::size_t> started_order;
    std::mutex started_mutex;
    std::condition_variable started_condition;
    std::atomic<std::size_t> started_count{0};
    bool release_blocked_tasks = false;
    std::mutex release_mutex;
    std::condition_variable release_condition;

    std::vector<std::shared_ptr<BlockingTask>> tasks;
    tasks.reserve(4);
    tasks.push_back(std::make_shared<BlockingTask>(
        1, started_order, started_mutex, started_condition, started_count, true,
        release_blocked_tasks, release_mutex, release_condition));
    tasks.push_back(std::make_shared<BlockingTask>(
        2, started_order, started_mutex, started_condition, started_count, true,
        release_blocked_tasks, release_mutex, release_condition));
    tasks.push_back(std::make_shared<BlockingTask>(
        3, started_order, started_mutex, started_condition, started_count, false,
        release_blocked_tasks, release_mutex, release_condition));
    tasks.push_back(std::make_shared<BlockingTask>(
        4, started_order, started_mutex, started_condition, started_count, false,
        release_blocked_tasks, release_mutex, release_condition));

    for (const auto &task : tasks)
    {
      if (!pool.enqueue(task))
      {
        VajraSpecCpp::fail("worker execution pool unexpectedly rejected a task");
      }
    }

    {
      std::unique_lock<std::mutex> lock(started_mutex);
      if (!started_condition.wait_for(lock, 2s, [&started_count]() {
            return started_count.load(std::memory_order_acquire) == 2;
          }))
      {
        VajraSpecCpp::fail("worker execution pool did not start the expected active tasks");
      }
    }

    if (pool.active_execution_count() != 2)
    {
      VajraSpecCpp::fail("worker execution pool exceeded the configured active execution capacity");
    }
    if (pool.queued_task_count() != 2)
    {
      VajraSpecCpp::fail("worker execution pool did not keep queued tasks behind the active slots");
    }

    {
      const std::lock_guard<std::mutex> lock(release_mutex);
      release_blocked_tasks = true;
    }
    release_condition.notify_all();
    pool.close_input();
    join_threads(threads);

    if (pool.max_observed_active_execution_count() != 2)
    {
      VajraSpecCpp::fail("worker execution pool observed more active executions than configured");
    }
    if (started_order.size() != 4)
    {
      VajraSpecCpp::fail("worker execution pool did not run every queued task");
    }
    if (!(started_order[0] == 1 && started_order[1] == 2))
    {
      VajraSpecCpp::fail("worker execution pool did not start queued tasks in FIFO order");
    }
    const auto third_task = std::find(started_order.begin(), started_order.end(), 3);
    const auto fourth_task = std::find(started_order.begin(), started_order.end(), 4);
    if (third_task == started_order.end() || fourth_task == started_order.end() || third_task > fourth_task)
    {
      VajraSpecCpp::fail("worker execution pool did not preserve FIFO order for queued tasks");
    }
  }

  void test_worker_execution_pool_abort_cancels_queued_tasks()
  {
    Vajra::rack::WorkerExecutionPool pool(1);
    std::vector<std::thread> threads;
    run_pool_workers(pool, 1, threads);

    std::vector<std::size_t> started_order;
    std::mutex started_mutex;
    std::condition_variable started_condition;
    std::atomic<std::size_t> started_count{0};
    bool release_blocked_tasks = false;
    std::mutex release_mutex;
    std::condition_variable release_condition;

    auto blocking_task = std::make_shared<BlockingTask>(
        1, started_order, started_mutex, started_condition, started_count, true,
        release_blocked_tasks, release_mutex, release_condition);
    auto queued_task = std::make_shared<BlockingTask>(
        2, started_order, started_mutex, started_condition, started_count, false,
        release_blocked_tasks, release_mutex, release_condition);

    pool.enqueue(blocking_task);
    pool.enqueue(queued_task);

    {
      std::unique_lock<std::mutex> lock(started_mutex);
      if (!started_condition.wait_for(lock, 2s, [&started_count]() {
            return started_count.load(std::memory_order_acquire) == 1;
          }))
      {
        VajraSpecCpp::fail("worker execution pool did not start the blocking task");
      }
    }

    pool.abort();
    {
      const std::lock_guard<std::mutex> lock(release_mutex);
      release_blocked_tasks = true;
    }
    release_condition.notify_all();
    join_threads(threads);

    if (!queued_task->canceled())
    {
      VajraSpecCpp::fail("worker execution pool did not cancel queued tasks on abort");
    }
  }
}

void VajraSpecCpp::run_worker_execution_pool_tests()
{
  test_worker_execution_pool_never_exceeds_configured_capacity();
  test_worker_execution_pool_abort_cancels_queued_tasks();
}
