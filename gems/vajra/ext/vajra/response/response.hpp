// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_HPP
#define VAJRA_RESPONSE_HPP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Vajra
{
  namespace response
  {
    enum class ConnectionBehavior
    {
      keep_alive,
      close,
    };

    struct Header
    {
      std::string name;
      std::string value;
    };

    struct Status
    {
      int code;
      std::string reason_phrase;
    };

    struct ResponseBodyFile
    {
      explicit ResponseBodyFile(FILE *body_file) : file(body_file) {}
      ResponseBodyFile(const ResponseBodyFile &) = delete;
      ResponseBodyFile &operator=(const ResponseBodyFile &) = delete;
      ~ResponseBodyFile()
      {
        if (file != nullptr)
        {
          std::fclose(file);
          file = nullptr;
        }
      }

      FILE *file = nullptr;
      std::size_t size = 0;
    };

    // A connection-owned byte ledger shared by every H2 buffering owner.  It
    // intentionally lives below the Rack layer so a live Rack body can hold
    // a lease after its HTTP/2 StreamState has been erased by a peer reset.
    // A maximum of zero keeps the historical "unlimited" behaviour while
    // still exposing truthful tracked/peak byte counts to the session.
    class ConnectionBufferBudget
    {
    public:
      using Observer = std::function<void(std::ptrdiff_t byte_delta, bool reservation_rejected)>;
      struct Snapshot
      {
        std::size_t maximum_bytes = 0;
        std::size_t used_bytes = 0;
        std::size_t peak_bytes = 0;
        std::size_t rejected_reservations = 0;
        std::size_t release_underflows = 0;
      };

      explicit ConnectionBufferBudget(std::size_t maximum_bytes)
          : maximum_bytes_(maximum_bytes)
      {
      }

      bool try_reserve(std::size_t bytes)
      {
        if (bytes == 0)
        {
          return true;
        }

        Observer observer;
        bool rejected = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          observer = observer_;
          // Keep the accounting counter finite even for an otherwise
          // unlimited budget.  Counter wrap would make the next bounded
          // reservation appear to have capacity that does not exist.
          const bool counter_overflow = bytes > std::numeric_limits<std::size_t>::max() - used_bytes_;
          const bool exceeds_capacity =
              maximum_bytes_ != 0 &&
              (used_bytes_ > maximum_bytes_ || bytes > maximum_bytes_ - used_bytes_);
          if (counter_overflow || exceeds_capacity)
          {
            ++rejected_reservations_;
            rejected = true;
          }
          else
          {
            used_bytes_ += bytes;
            peak_bytes_ = std::max(peak_bytes_, used_bytes_);
          }
        }
        if (observer)
        {
          // Metrics must never turn a successful reservation into a leaked
          // lease or terminate a noexcept release path.
          try
          {
            observer(rejected ? 0 : static_cast<std::ptrdiff_t>(bytes), rejected);
          }
          catch (...)
          {
          }
        }
        return !rejected;
      }

      // A release is deliberately defensive.  A terminal path must never
      // wrap the counter and turn a bounded session into an unbounded one;
      // the underflow is retained for diagnostics/tests instead.
      void release(std::size_t bytes) noexcept
      {
        if (bytes == 0)
        {
          return;
        }

        Observer observer;
        std::size_t released_bytes = 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          observer = observer_;
          if (bytes > used_bytes_)
          {
            released_bytes = used_bytes_;
            used_bytes_ = 0;
            ++release_underflows_;
          }
          else
          {
            used_bytes_ -= bytes;
            released_bytes = bytes;
          }
        }
        if (observer && released_bytes > 0)
        {
          try
          {
            observer(-static_cast<std::ptrdiff_t>(released_bytes), false);
          }
          catch (...)
          {
          }
        }
        capacity_condition_.notify_all();
      }

      void set_observer(Observer observer)
      {
        std::lock_guard<std::mutex> lock(mutex_);
        observer_ = std::move(observer);
      }

      void wait_for_capacity()
      {
        if (maximum_bytes_ == 0)
        {
          return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        capacity_condition_.wait(lock, [this]()
                                 { return used_bytes_ < maximum_bytes_; });
      }

      std::size_t available_bytes() const
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maximum_bytes_ == 0)
        {
          return std::numeric_limits<std::size_t>::max();
        }
        return used_bytes_ >= maximum_bytes_ ? 0 : maximum_bytes_ - used_bytes_;
      }

      std::size_t used_bytes() const
      {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_bytes_;
      }

      Snapshot snapshot() const
      {
        std::lock_guard<std::mutex> lock(mutex_);
        return Snapshot{
            maximum_bytes_,
            used_bytes_,
            peak_bytes_,
            rejected_reservations_,
            release_underflows_};
      }

    private:
      const std::size_t maximum_bytes_;
      std::size_t used_bytes_ = 0;
      std::size_t peak_bytes_ = 0;
      std::size_t rejected_reservations_ = 0;
      std::size_t release_underflows_ = 0;
      Observer observer_;
      mutable std::mutex mutex_;
      std::condition_variable capacity_condition_;
    };

    // A response body produced by Rack while the transport consumes it.  The
    // queue is deliberately bounded: a slow peer must eventually stop the
    // Rack enumerable instead of growing native memory or spilling to disk.
    class ResponseBodyStream
    {
    public:
      enum class ReadStatus
      {
        data,
        pending,
        complete,
        failed,
        cancelled,
      };

      struct ReadResult
      {
        ReadStatus status = ReadStatus::pending;
        std::size_t size = 0;
      };

      enum class PushStatus
      {
        accepted,
        pending,
        stopped,
      };

      struct PushResult
      {
        PushStatus status = PushStatus::pending;
        std::size_t size = 0;
      };

      static constexpr std::size_t kDefaultCapacityBytes = 64 * 1024;
      static constexpr std::size_t kMaximumChunkBytes = 16 * 1024;

      explicit ResponseBodyStream(
          std::size_t capacity_bytes = kDefaultCapacityBytes,
          std::optional<std::size_t> expected_bytes = std::nullopt,
          std::shared_ptr<ConnectionBufferBudget> connection_buffer_budget = nullptr)
          : capacity_bytes_(capacity_bytes == 0 ? 1 : capacity_bytes),
            expected_bytes_(expected_bytes),
            connection_buffer_budget_(std::move(connection_buffer_budget))
      {
      }

      ResponseBodyStream(const ResponseBodyStream &) = delete;
      ResponseBodyStream &operator=(const ResponseBodyStream &) = delete;

      ~ResponseBodyStream()
      {
        std::shared_ptr<ConnectionBufferBudget> budget;
        std::size_t bytes_to_release = 0;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          budget = connection_buffer_budget_;
          bytes_to_release = buffered_bytes_;
          buffered_bytes_ = 0;
          chunks_.clear();
          front_offset_ = 0;
        }
        if (budget && bytes_to_release > 0)
        {
          budget->release(bytes_to_release);
        }
      }

      // The budget normally arrives before Rack calls body.each.  Supporting
      // an already-buffered source keeps non-streaming completion paths safe
      // too, without giving a later HTTP/2 consumer an unaccounted queue.
      bool attach_connection_buffer_budget(
          std::shared_ptr<ConnectionBufferBudget> connection_buffer_budget)
      {
        if (!connection_buffer_budget)
        {
          return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_buffer_budget_)
        {
          return connection_buffer_budget_ == connection_buffer_budget;
        }
        if (!connection_buffer_budget->try_reserve(buffered_bytes_))
        {
          return false;
        }
        connection_buffer_budget_ = std::move(connection_buffer_budget);
        return true;
      }

      // Blocks until the complete payload has entered the bounded queue or a
      // consumer cancels/fails the stream. Callers that hold the Ruby GVL must
      // invoke this through rb_thread_call_without_gvl while it can block.
      bool push(const char *data, std::size_t length)
      {
        std::size_t offset = 0;
        while (offset < length)
        {
          std::unique_lock<std::mutex> lock(mutex_);
          writable_.wait(lock, [this]()
                         { return cancelled_ || failed_ || finished_ || buffered_bytes_ < capacity_bytes_; });
          if (cancelled_ || failed_ || finished_)
          {
            return false;
          }

          const std::size_t available = capacity_bytes_ - buffered_bytes_;
          std::size_t chunk_size = std::min(
              std::min(kMaximumChunkBytes, available),
              length - offset);
          if (connection_buffer_budget_)
          {
            chunk_size = std::min(chunk_size, connection_buffer_budget_->available_bytes());
            if (chunk_size == 0)
            {
              const std::shared_ptr<ConnectionBufferBudget> budget = connection_buffer_budget_;
              lock.unlock();
              budget->wait_for_capacity();
              continue;
            }
          }
          if (expected_bytes_ &&
              (produced_bytes_ > *expected_bytes_ || chunk_size > *expected_bytes_ - produced_bytes_))
          {
            failed_ = true;
            error_message_ = "Rack response body exceeded its known Content-Length";
            lock.unlock();
            readable_.notify_all();
            writable_.notify_all();
            return false;
          }
          if (connection_buffer_budget_ && !connection_buffer_budget_->try_reserve(chunk_size))
          {
            const std::shared_ptr<ConnectionBufferBudget> budget = connection_buffer_budget_;
            lock.unlock();
            budget->wait_for_capacity();
            continue;
          }
          try
          {
            chunks_.emplace_back(data + offset, chunk_size);
          }
          catch (...)
          {
            if (connection_buffer_budget_)
            {
              connection_buffer_budget_->release(chunk_size);
            }
            throw;
          }
          buffered_bytes_ += chunk_size;
          produced_bytes_ += chunk_size;
          offset += chunk_size;
          readable_.notify_all();
        }
        return true;
      }

      // Pushes at most one bounded queue segment.  Ruby calls this through a
      // short no-GVL region so an asynchronous Ruby interrupt can be observed
      // between waits rather than long-jumping across C++ ownership.
      PushResult push_for(
          const char *data,
          std::size_t length,
          std::chrono::milliseconds maximum_wait)
      {
        if (length == 0)
        {
          return PushResult{PushStatus::accepted, 0};
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = writable_.wait_for(
            lock,
            maximum_wait,
            [this]()
            { return cancelled_ || failed_ || finished_ || buffered_bytes_ < capacity_bytes_; });
        if (!ready)
        {
          return PushResult{PushStatus::pending, 0};
        }
        if (cancelled_ || failed_ || finished_)
        {
          return PushResult{PushStatus::stopped, 0};
        }

        const std::size_t available = capacity_bytes_ - buffered_bytes_;
        std::size_t chunk_size = std::min(
            std::min(kMaximumChunkBytes, available),
            length);
        if (connection_buffer_budget_)
        {
          chunk_size = std::min(chunk_size, connection_buffer_budget_->available_bytes());
          if (chunk_size == 0 || !connection_buffer_budget_->try_reserve(chunk_size))
          {
            return PushResult{PushStatus::pending, 0};
          }
        }
        if (expected_bytes_ &&
            (produced_bytes_ > *expected_bytes_ || chunk_size > *expected_bytes_ - produced_bytes_))
        {
          if (connection_buffer_budget_)
          {
            connection_buffer_budget_->release(chunk_size);
          }
          failed_ = true;
          error_message_ = "Rack response body exceeded its known Content-Length";
          lock.unlock();
          readable_.notify_all();
          writable_.notify_all();
          return PushResult{PushStatus::stopped, 0};
        }
        try
        {
          chunks_.emplace_back(data, chunk_size);
        }
        catch (...)
        {
          if (connection_buffer_budget_)
          {
            connection_buffer_budget_->release(chunk_size);
          }
          throw;
        }
        buffered_bytes_ += chunk_size;
        produced_bytes_ += chunk_size;
        lock.unlock();
        readable_.notify_all();
        return PushResult{PushStatus::accepted, chunk_size};
      }

      void finish()
      {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          if (cancelled_ || failed_)
          {
            return;
          }
          if (expected_bytes_ && produced_bytes_ != *expected_bytes_)
          {
            failed_ = true;
            error_message_ = "Rack response body did not match its known Content-Length";
          }
          else
          {
            finished_ = true;
          }
        }
        readable_.notify_all();
        writable_.notify_all();
      }

      void fail(std::string message)
      {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          if (cancelled_ || finished_ || failed_)
          {
            return;
          }
          failed_ = true;
          error_message_ = std::move(message);
        }
        readable_.notify_all();
        writable_.notify_all();
      }

      void cancel()
      {
        std::shared_ptr<ConnectionBufferBudget> budget;
        std::size_t bytes_to_release = 0;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          if (cancelled_)
          {
            return;
          }
          cancelled_ = true;
          budget = connection_buffer_budget_;
          bytes_to_release = buffered_bytes_;
          chunks_.clear();
          front_offset_ = 0;
          buffered_bytes_ = 0;
        }
        if (budget && bytes_to_release > 0)
        {
          budget->release(bytes_to_release);
        }
        readable_.notify_all();
        writable_.notify_all();
      }

      // Cancellation is initiated by the consumer and deliberately does not
      // mean the Rack producer has returned from body.each/close.  The
      // execution scheduler retains its admission until the producer reports
      // this final transition.
      void mark_producer_finished()
      {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          producer_finished_ = true;
        }
        readable_.notify_all();
        writable_.notify_all();
      }

      ReadResult read(char *destination, std::size_t capacity, bool wait_for_data)
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wait_for_data)
        {
          readable_.wait(lock, [this]()
                         { return !chunks_.empty() || cancelled_ || failed_ || finished_; });
        }

        if (chunks_.empty())
        {
          return terminal_read_result_locked();
        }
        if (capacity == 0)
        {
          return ReadResult{ReadStatus::pending, 0};
        }

        const std::string &chunk = chunks_.front();
        const std::size_t available = chunk.size() - front_offset_;
        const std::size_t copied = std::min(capacity, available);
        std::memcpy(destination, chunk.data() + front_offset_, copied);
        front_offset_ += copied;
        buffered_bytes_ -= copied;
        const std::shared_ptr<ConnectionBufferBudget> budget = connection_buffer_budget_;
        if (front_offset_ == chunk.size())
        {
          chunks_.pop_front();
          front_offset_ = 0;
        }
        lock.unlock();
        if (budget && copied > 0)
        {
          budget->release(copied);
        }
        writable_.notify_all();
        return ReadResult{ReadStatus::data, copied};
      }

      bool has_buffered_data() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !chunks_.empty();
      }

      bool terminal() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_ || failed_ || finished_;
      }

      bool cancelled() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_;
      }

      bool failed() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
      }

      bool producer_finished() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return producer_finished_;
      }

      std::string error_message() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return error_message_;
      }

      std::size_t buffered_bytes() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return buffered_bytes_;
      }

      std::size_t produced_bytes() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return produced_bytes_;
      }

      std::optional<std::size_t> known_length() const
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        return expected_bytes_;
      }

    private:
      ReadResult terminal_read_result_locked() const
      {
        if (cancelled_)
        {
          return ReadResult{ReadStatus::cancelled, 0};
        }
        if (failed_)
        {
          return ReadResult{ReadStatus::failed, 0};
        }
        if (finished_)
        {
          return ReadResult{ReadStatus::complete, 0};
        }
        return ReadResult{ReadStatus::pending, 0};
      }

      const std::size_t capacity_bytes_;
      const std::optional<std::size_t> expected_bytes_;
      std::shared_ptr<ConnectionBufferBudget> connection_buffer_budget_;
      mutable std::mutex mutex_;
      std::condition_variable readable_;
      std::condition_variable writable_;
      std::deque<std::string> chunks_;
      std::size_t front_offset_ = 0;
      std::size_t buffered_bytes_ = 0;
      std::size_t produced_bytes_ = 0;
      bool finished_ = false;
      bool failed_ = false;
      bool cancelled_ = false;
      bool producer_finished_ = false;
      std::string error_message_;
    };

    struct Response
    {
      Response() = default;

      Response(
          Status response_status,
          std::vector<Header> response_headers,
          std::string response_body,
          ConnectionBehavior response_connection_behavior = ConnectionBehavior::close)
          : status(std::move(response_status)),
            headers(std::move(response_headers)),
            body(std::move(response_body)),
            connection_behavior(response_connection_behavior)
      {
      }

      Response(
          Status response_status,
          std::vector<Header> response_headers,
          std::vector<std::string> response_body_chunks,
          ConnectionBehavior response_connection_behavior = ConnectionBehavior::close)
          : status(std::move(response_status)),
            headers(std::move(response_headers)),
            connection_behavior(response_connection_behavior),
            body_chunks(std::move(response_body_chunks))
      {
      }

      Status status;
      std::vector<Header> headers;
      std::string body;
      ConnectionBehavior connection_behavior = ConnectionBehavior::close;
      std::vector<std::string> body_chunks;
      std::shared_ptr<ResponseBodyFile> body_file;
      std::shared_ptr<ResponseBodyStream> body_stream;
      bool hijacked = false;
    };

    inline bool response_has_body_chunks(const Response &response)
    {
      return !response.body_chunks.empty();
    }

    inline bool response_has_body_file(const Response &response)
    {
      return response.body_file && response.body_file->file != nullptr;
    }

    inline bool response_has_body_stream(const Response &response)
    {
      return static_cast<bool>(response.body_stream);
    }

    inline bool response_body_stream_has_known_length(const Response &response)
    {
      return response_has_body_stream(response) && response.body_stream->known_length().has_value();
    }

    inline std::size_t response_body_size(const Response &response)
    {
      if (response_has_body_stream(response))
      {
        return response.body_stream->known_length().value_or(0);
      }
      if (response_has_body_file(response))
      {
        return response.body_file->size;
      }
      if (!response_has_body_chunks(response))
      {
        return response.body.size();
      }

      std::size_t size = 0;
      for (const std::string &chunk : response.body_chunks)
      {
        size += chunk.size();
      }
      return size;
    }

    inline bool response_body_empty(const Response &response)
    {
      return response_body_size(response) == 0;
    }
  }
}

#endif
