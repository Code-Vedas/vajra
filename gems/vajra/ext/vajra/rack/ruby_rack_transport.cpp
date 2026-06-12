// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/ruby_rack_transport.hpp"

#include "rack/http2_stream.hpp"
#include "rack/native_input.hpp"
#include "rack/rack_execution_profiler.hpp"
#include "rack/ruby_execution_bridge.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/runtime_state.hpp"
#include "runtime/traceparent.hpp"
#include "ruby/thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace
{
  std::atomic<bool> rack_execution_callback_installed_flag{false};
  std::mutex rack_execution_callback_mutex;
  VALUE rack_execution_callback = Qnil;
  std::atomic<bool> rack_execution_app_installed_flag{false};
  std::mutex rack_execution_app_mutex;
  VALUE rack_execution_app = Qnil;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    std::string *request_body;
    VALUE rack_input = Qnil;
    int client_fd = -1;
    std::shared_ptr<Vajra::rack::NativeInputState> input_state;
    std::shared_ptr<Vajra::rack::NativeHijackState> hijack_state;
    std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream;
    std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
    bool use_native_app = false;
  };

  struct ResponseNormalizationContext
  {
    VALUE result = Qnil;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  struct DirectRackObservabilityFields
  {
    std::string method;
    std::string target;
    std::string protocol;
    std::string host;
    std::string traceparent;
  };

  DirectRackObservabilityFields direct_rack_observability_fields(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    DirectRackObservabilityFields fields;
    std::string path;
    std::string query;
    std::string server_name;
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      if (entry.key == "REQUEST_METHOD")
      {
        fields.method = entry.value;
      }
      else if (entry.key == "PATH_INFO")
      {
        path = entry.value;
      }
      else if (entry.key == "QUERY_STRING")
      {
        query = entry.value;
      }
      else if (entry.key == "SERVER_PROTOCOL")
      {
        fields.protocol = entry.value;
      }
      else if (entry.key == "HTTP_HOST")
      {
        fields.host = entry.value;
      }
      else if (entry.key == "SERVER_NAME")
      {
        server_name = entry.value;
      }
      else if (entry.key == "HTTP_TRACEPARENT")
      {
        fields.traceparent = entry.value;
      }
    }
    fields.target = std::move(path);
    if (!query.empty())
    {
      fields.target += "?" + query;
    }
    if (fields.host.empty())
    {
      fields.host = std::move(server_name);
    }
    return fields;
  }

  Vajra::runtime::RequestSpanEvent direct_rack_observability_event(
      const DirectRackObservabilityFields &fields,
      const Vajra::response::Response &response,
      std::chrono::steady_clock::time_point started_at)
  {
    Vajra::runtime::RequestSpanEvent event;
    event.method = fields.method;
    event.target = fields.target;
    event.status_code = response.status.code;
    event.duration_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count();
    event.protocol = fields.protocol;
    event.host = fields.host;
    event.outcome = "completed";
    event.response_sent = true;
    event.connection_outcome = response.connection_behavior == Vajra::response::ConnectionBehavior::close ? "close" : "keepalive";
    event.worker_index = static_cast<int>(Vajra::runtime::current_worker_index());
    event.worker_pid = getpid();
    event.trace_id = Vajra::runtime::traceparent_part(fields.traceparent, 1);
    event.span_id = Vajra::runtime::traceparent_part(fields.traceparent, 2);
    return event;
  }

  VALUE rack_execution_module()
  {
    VALUE vajra_module = rb_const_get(rb_cObject, rb_intern("Vajra"));
    VALUE internal_module = rb_const_get(vajra_module, rb_intern("Internal"));
    return rb_const_get(internal_module, rb_intern("RackExecution"));
  }

  VALUE call_native_rack_app(VALUE app, VALUE env)
  {
    VALUE arguments[] = {app, env};
    return rb_funcallv(rack_execution_module(), rb_intern("call_native"), 2, arguments);
  }

  VALUE protected_execute_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    try
    {
      VALUE callback = Qnil;
      {
        const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
        callback = rack_execution_callback;
      }

      if (NIL_P(callback))
      {
        return Qnil;
      }

      VALUE env_entries = Vajra::rack::RubyExecutionBridge::env_entries_array_from(*context->env_entries);
      VALUE request_body = NIL_P(context->rack_input)
                               ? Vajra::rack::RubyExecutionBridge::binary_string_from(*context->request_body)
                               : context->rack_input;
      VALUE arguments[] = {env_entries, request_body};
      return rb_funcallv(callback, rb_intern("call"), 2, arguments);
    }
    catch (const std::exception &error)
    {
      rb_raise(rb_eRuntimeError, "%s", error.what());
    }
    catch (...)
    {
      rb_raise(rb_eRuntimeError, "Rack request execution failed with an unknown native error");
    }
  }

  VALUE protected_execute_native_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    const auto started_at = std::chrono::steady_clock::now();
    try
    {
      VALUE app = Qnil;
      {
        const std::lock_guard<std::mutex> app_lock(rack_execution_app_mutex);
        app = rack_execution_app;
      }

      if (NIL_P(app))
      {
        return Qnil;
      }

      context->hijack_state.reset();
      VALUE env = NIL_P(context->rack_input)
                      ? Vajra::rack::RubyExecutionBridge::rack_env_from(
                            *context->env_entries,
                            std::move(*context->request_body),
                            context->client_fd,
                            &context->hijack_state,
                            context->http2_stream,
                            context->native_hijack_transport)
                      : Vajra::rack::RubyExecutionBridge::rack_env_from(
                            *context->env_entries,
                            context->rack_input,
                            context->client_fd,
                            context->input_state,
                            &context->hijack_state,
                            context->http2_stream,
                            context->native_hijack_transport);
      const bool close_rack_input_after_app = context->input_state == nullptr;
      VALUE result = call_native_rack_app(app, env);
      try
      {
        if (Vajra::rack::RubyExecutionBridge::native_hijack_called(context->hijack_state))
        {
          Vajra::response::Response response;
          response.status = Vajra::response::Status{101, "Switching Protocols"};
          response.connection_behavior = Vajra::response::ConnectionBehavior::close;
          response.hijacked = true;
          context->response = std::move(response);
          if (close_rack_input_after_app)
          {
            Vajra::rack::RubyExecutionBridge::close_rack_input(env);
          }
          return Qnil;
        }
        Vajra::rack::RubyExecutionBridge::commit_native_hijack(context->hijack_state);
        context->response = Vajra::rack::RackResponseHandler::response_from_rack_result(result);
        if (close_rack_input_after_app)
        {
          Vajra::rack::RubyExecutionBridge::close_rack_input(env);
        }
      }
      catch (...)
      {
        if (close_rack_input_after_app)
        {
          Vajra::rack::RubyExecutionBridge::close_rack_input(env);
        }
        throw;
      }
      if (context->response &&
          Vajra::runtime::runtime_request_span_observability_enabled() &&
          !Vajra::runtime::runtime_tracing_active_context_required())
      {
        const DirectRackObservabilityFields observability_fields = direct_rack_observability_fields(*context->env_entries);
        if (Vajra::runtime::runtime_trace_sampled(observability_fields.traceparent))
        {
          Vajra::runtime::emit_runtime_request_span_event(
              direct_rack_observability_event(observability_fields, *context->response, started_at));
        }
      }
      return Qnil;
    }
    catch (const Vajra::rack::RubyJumpTag &jump)
    {
      rb_jump_tag(jump.state());
      return Qnil;
    }
    catch (const std::exception &error)
    {
      rb_raise(rb_eRuntimeError, "%s", error.what());
    }
    catch (...)
    {
      rb_raise(rb_eRuntimeError, "Rack native request execution failed with an unknown native error");
    }
  }

  VALUE protected_normalize_rack_response(VALUE data)
  {
    auto *context = reinterpret_cast<ResponseNormalizationContext *>(data);
    try
    {
      context->response = Vajra::rack::RackResponseHandler::response_from_normalized_result(context->result);
    }
    catch (const Vajra::rack::RubyJumpTag &jump)
    {
      rb_jump_tag(jump.state());
      return Qnil;
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    return Qnil;
  }

  void *execute_rack_request_with_gvl(void *data)
  {
    const auto started_at = std::chrono::steady_clock::now();
    auto &profiling_counters = Vajra::rack::runtime_profiling_counters();
    Vajra::rack::ScopedProfilingSample profiling_sample(
        profiling_counters.ruby_execution_count,
        profiling_counters.ruby_execution_nanoseconds);
    Vajra::runtime::note_worker_execution_started();
    const auto profiling_cleanup = []()
    {
      Vajra::runtime::note_worker_execution_finished();
    };
    auto *context = static_cast<ExecutionCallContext *>(data);

    int state = 0;
    VALUE result = rb_protect(
        context->use_native_app ? protected_execute_native_rack_request : protected_execute_rack_request,
        reinterpret_cast<VALUE>(context),
        &state);
    if (state != 0)
    {
      context->error_message = Vajra::rack::RubyExecutionBridge::exception_message(rb_errinfo());
      rb_set_errinfo(Qnil);
      profiling_cleanup();
      Vajra::runtime::note_worker_rack_execution_time(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - started_at)
              .count());
      return nullptr;
    }

    if (!context->use_native_app && !NIL_P(result))
    {
      ResponseNormalizationContext normalization_context{result, std::nullopt, ""};
      state = 0;
      rb_protect(protected_normalize_rack_response, reinterpret_cast<VALUE>(&normalization_context), &state);
      if (state != 0)
      {
        context->error_message = Vajra::rack::RubyExecutionBridge::exception_message(rb_errinfo());
        rb_set_errinfo(Qnil);
        profiling_cleanup();
        Vajra::runtime::note_worker_rack_execution_time(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        return nullptr;
      }
      if (!normalization_context.error_message.empty())
      {
        context->error_message = normalization_context.error_message;
        profiling_cleanup();
        Vajra::runtime::note_worker_rack_execution_time(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count());
        return nullptr;
      }

      context->response = std::move(normalization_context.response);
    }

    profiling_cleanup();
    Vajra::runtime::note_worker_rack_execution_time(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
    return nullptr;
  }

  std::optional<Vajra::response::Response> execute_rack_request(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries,
      std::string request_body,
      int client_fd,
      std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream,
      std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport,
      bool acquire_gvl)
  {
    if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
    {
      return std::nullopt;
    }

    ExecutionCallContext context{&env_entries, &request_body, Qnil, client_fd, nullptr, nullptr, std::move(http2_stream), std::move(native_hijack_transport), std::nullopt, ""};
    context.use_native_app = rack_execution_app_installed_flag.load(std::memory_order_acquire);
    if (acquire_gvl)
    {
      rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);
    }
    else
    {
      execute_rack_request_with_gvl(&context);
    }

    if (!context.error_message.empty())
    {
      throw std::runtime_error("Rack request execution failed: " + context.error_message);
    }

    return context.response;
  }

  std::optional<Vajra::response::Response> execute_rack_request(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries,
      VALUE rack_input,
      int client_fd,
      std::shared_ptr<Vajra::rack::NativeInputState> input_state,
      std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream,
      std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport,
      bool acquire_gvl)
  {
    if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
    {
      return std::nullopt;
    }

    std::string empty_body;
    ExecutionCallContext context{&env_entries, &empty_body, rack_input, client_fd, std::move(input_state), nullptr, std::move(http2_stream), std::move(native_hijack_transport), std::nullopt, ""};
    context.use_native_app = rack_execution_app_installed_flag.load(std::memory_order_acquire);
    if (!NIL_P(context.rack_input))
    {
      rb_gc_register_address(&context.rack_input);
    }
    if (acquire_gvl)
    {
      rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);
    }
    else
    {
      execute_rack_request_with_gvl(&context);
    }

    if (!context.error_message.empty())
    {
      if (!NIL_P(context.rack_input))
      {
        rb_gc_unregister_address(&context.rack_input);
      }
      throw std::runtime_error("Rack request execution failed: " + context.error_message);
    }

    if (!NIL_P(context.rack_input))
    {
      rb_gc_unregister_address(&context.rack_input);
    }
    return context.response;
  }

  class SameProcessRackTask final
  {
  public:
    SameProcessRackTask(
        std::vector<Vajra::request::RackEnvEntry> env_entries,
        std::shared_ptr<Vajra::rack::NativeInputState> input_state,
        int client_fd,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport)
        : env_entries_(std::move(env_entries)),
          input_state_(std::move(input_state)),
          client_fd_(client_fd),
          native_hijack_transport_(std::move(native_hijack_transport))
    {
    }

    void execute()
    {
      try
      {
        Vajra::rack::NativeInputHandle input = Vajra::rack::create_native_input(input_state_);
        complete_with_response(execute_rack_request(
            env_entries_,
            input.value,
            client_fd_,
            input_state_,
            nullptr,
            native_hijack_transport_,
            false));
      }
      catch (const std::exception &error)
      {
        complete_with_error(error.what());
      }
      catch (...)
      {
        complete_with_error("Rack request execution failed with an unknown native error");
      }
    }

    void cancel(const std::string &message)
    {
      complete_with_error(message);
    }

    void wait()
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this]()
                      { return completed_; });
    }

    bool completed() const
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return completed_;
    }

    std::optional<Vajra::response::Response> take_response()
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return std::move(response_);
    }

    std::string error_message() const
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return error_message_;
    }

  private:
    void complete_with_response(std::optional<Vajra::response::Response> response)
    {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
        completed_ = true;
      }
      condition_.notify_all();
    }

    void complete_with_error(std::string message)
    {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        error_message_ = std::move(message);
        completed_ = true;
      }
      condition_.notify_all();
    }

    std::vector<Vajra::request::RackEnvEntry> env_entries_;
    std::shared_ptr<Vajra::rack::NativeInputState> input_state_;
    int client_fd_ = -1;
    std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Vajra::response::Response> response_;
    std::string error_message_;
    bool completed_ = false;
  };

  class SameProcessDirectRackTask final
  {
  public:
    SameProcessDirectRackTask(
        std::vector<Vajra::request::RackEnvEntry> env_entries,
        std::string request_body,
        int client_fd,
        std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream = nullptr,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport = nullptr,
        Vajra::request::RequestExecutor::CompletionCallback callback = nullptr)
        : env_entries_(std::move(env_entries)),
          request_body_(std::move(request_body)),
          client_fd_(client_fd),
          http2_stream_(std::move(http2_stream)),
          native_hijack_transport_(std::move(native_hijack_transport)),
          callback_(std::move(callback)),
          enqueued_at_(std::chrono::steady_clock::now())
    {
    }

    void execute()
    {
      try
      {
        complete_with_response(execute_rack_request(
            env_entries_,
            request_body_,
            client_fd_,
            http2_stream_,
            native_hijack_transport_,
            false));
      }
      catch (const std::exception &error)
      {
        complete_with_error(error.what());
      }
      catch (...)
      {
        complete_with_error("Rack request execution failed with an unknown native error");
      }
    }

    void cancel(const std::string &message)
    {
      complete_with_error(message);
    }

    void wait()
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this]()
                      { return completed_; });
    }

    std::optional<Vajra::response::Response> take_response()
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return std::move(response_);
    }

    std::string error_message() const
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return error_message_;
    }

  private:
    void complete_with_response(std::optional<Vajra::response::Response> response)
    {
      if (callback_)
      {
        callback_(
            std::move(response),
            "",
            elapsed_queue_wait_nanoseconds());
        return;
      }
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
        completed_ = true;
      }
      condition_.notify_all();
    }

    void complete_with_error(std::string message)
    {
      if (callback_)
      {
        callback_(
            std::nullopt,
            std::move(message),
            elapsed_queue_wait_nanoseconds());
        return;
      }
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        error_message_ = std::move(message);
        completed_ = true;
      }
      condition_.notify_all();
    }

    std::int64_t elapsed_queue_wait_nanoseconds() const
    {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - enqueued_at_)
          .count();
    }

    std::vector<Vajra::request::RackEnvEntry> env_entries_;
    std::string request_body_;
    int client_fd_ = -1;
    std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream_;
    std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport_;
    Vajra::request::RequestExecutor::CompletionCallback callback_;
    std::chrono::steady_clock::time_point enqueued_at_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Vajra::response::Response> response_;
    std::string error_message_;
    bool completed_ = false;
  };

  class SameProcessRackExecutionPool final
  {
  public:
    void configure(std::size_t thread_count)
    {
      if (thread_count == 0)
      {
        thread_count = 1;
      }

      std::lock_guard<std::mutex> lock(mutex_);
      desired_thread_count_ = thread_count;
    }

    void ensure_started()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = false;
      while (threads_.size() < desired_thread_count_)
      {
        VALUE thread = rb_thread_create(worker_thread, this);
        threads_.push_back(thread);
      }
    }

    void restart_after_fork()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      threads_.clear();
      tasks_.clear();
      active_task_count_ = 0;
      closed_ = false;
      while (threads_.size() < desired_thread_count_)
      {
        VALUE thread = rb_thread_create(worker_thread, this);
        threads_.push_back(thread);
      }
    }

    void shutdown()
    {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        threads_.clear();
      }
      condition_.notify_all();
    }

    bool wait_until_idle(std::chrono::milliseconds timeout)
    {
      std::unique_lock<std::mutex> lock(mutex_);
      return condition_.wait_for(lock, timeout, [this]()
                                 { return tasks_.empty() && active_task_count_ == 0; });
    }

    bool running()
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      return !closed_;
    }

    template <typename Task>
    void enqueue(const std::shared_ptr<Task> &task)
    {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
        {
          task->cancel("same-process Rack execution pool is closed");
          return;
        }
        tasks_.push_back(queued_task_for(task));
      }
      condition_.notify_one();
    }

  private:
    struct QueuedTask
    {
      std::shared_ptr<void> task;
      void (*execute)(const std::shared_ptr<void> &) = nullptr;
      void (*cancel)(const std::shared_ptr<void> &, const std::string &) = nullptr;
    };

    template <typename Task>
    static QueuedTask queued_task_for(const std::shared_ptr<Task> &task)
    {
      return QueuedTask{
          task,
          [](const std::shared_ptr<void> &data)
          {
            std::static_pointer_cast<Task>(data)->execute();
          },
          [](const std::shared_ptr<void> &data, const std::string &message)
          {
            std::static_pointer_cast<Task>(data)->cancel(message);
          }};
    }

    struct WaitContext
    {
      SameProcessRackExecutionPool *pool;
      QueuedTask task;
    };

    static void *wait_for_task_without_gvl(void *data)
    {
      auto *context = static_cast<WaitContext *>(data);
      std::unique_lock<std::mutex> lock(context->pool->mutex_);
      context->pool->condition_.wait(lock, [context]()
                                     { return context->pool->closed_ || !context->pool->tasks_.empty(); });
      if (context->pool->tasks_.empty())
      {
        return nullptr;
      }
      context->task = std::move(context->pool->tasks_.front());
      context->pool->tasks_.pop_front();
      ++context->pool->active_task_count_;
      return nullptr;
    }

    static VALUE worker_thread(void *data)
    {
      auto *pool = static_cast<SameProcessRackExecutionPool *>(data);
      for (;;)
      {
        WaitContext context{pool, QueuedTask{}};
        rb_thread_call_without_gvl(wait_for_task_without_gvl, &context, RUBY_UBF_IO, nullptr);
        if (!context.task.task)
        {
          break;
        }
        try
        {
          context.task.execute(context.task.task);
        }
        catch (...)
        {
          context.task.cancel(context.task.task, "Rack request execution failed with an unknown native error");
        }
        pool->task_finished();
      }
      return Qnil;
    }

    void task_finished()
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_task_count_ > 0)
        {
          --active_task_count_;
        }
      }
      condition_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedTask> tasks_;
    std::vector<VALUE> threads_;
    bool closed_ = true;
    std::size_t desired_thread_count_ = 1;
    std::size_t active_task_count_ = 0;
  };

  SameProcessRackExecutionPool &same_process_execution_pool()
  {
    static SameProcessRackExecutionPool pool;
    return pool;
  }

  class SameProcessRackExecutionSession final : public Vajra::rack::RackExecutionSession
  {
  public:
    SameProcessRackExecutionSession(
        std::vector<Vajra::request::RackEnvEntry> env_entries,
        int client_fd,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport)
        : input_state_(Vajra::rack::create_native_input_state()),
          task_(std::make_shared<SameProcessRackTask>(
              std::move(env_entries),
              input_state_,
              client_fd,
              std::move(native_hijack_transport)))
    {
      same_process_execution_pool().enqueue(task_);
    }

    ~SameProcessRackExecutionSession() override
    {
      if (!body_finished_)
      {
        fail_noexcept("request body stream closed before completion");
      }
      wait_noexcept();
      close_noexcept();
    }

    Vajra::rack::NativeInputState *native_input_state() override
    {
      return input_state_.get();
    }

    std::shared_ptr<Vajra::rack::NativeInputState> native_input_state_owner() override
    {
      return input_state_;
    }

    void append_request_body_bytes(const char *data, std::size_t length) override
    {
      if (task_->completed())
      {
        return;
      }
      Vajra::rack::native_input_append(input_state_.get(), data, length);
    }

    bool try_append_request_body_bytes(const char *data, std::size_t length) override
    {
      if (task_->completed())
      {
        return true;
      }
      return Vajra::rack::native_input_try_append(input_state_.get(), data, length);
    }

    void finish_request_body() override
    {
      if (!body_finished_)
      {
        Vajra::rack::native_input_finish(input_state_.get());
        body_finished_ = true;
      }
    }

    void fail_request_body(const std::string &message) noexcept override
    {
      fail_noexcept(message);
      body_finished_ = true;
    }

    std::optional<Vajra::response::Response> finish() override
    {
      finish_request_body();
      task_->wait();
      finished_ = true;
      const std::string error_message = task_->error_message();
      std::optional<Vajra::response::Response> response = task_->take_response();
      close_noexcept();
      if (!error_message.empty())
      {
        throw std::runtime_error(error_message);
      }
      return response;
    }

  private:
    void wait_noexcept() noexcept
    {
      try
      {
        task_->wait();
      }
      catch (...)
      {
      }
    }

    void fail_noexcept(const std::string &message) noexcept
    {
      try
      {
        Vajra::rack::native_input_fail(input_state_.get(), message);
      }
      catch (...)
      {
      }
    }

    void close_noexcept() noexcept
    {
      try
      {
        Vajra::rack::native_input_close(input_state_.get());
      }
      catch (...)
      {
      }
    }

    std::shared_ptr<Vajra::rack::NativeInputState> input_state_;
    std::shared_ptr<SameProcessRackTask> task_;
    bool body_finished_ = false;
    bool finished_ = false;
  };

  class SameProcessRackExecutionTransport final : public Vajra::rack::RackExecutionTransport
  {
  public:
    bool async_execution_supported() const override
    {
      return true;
    }

    bool async_completion_supported() const override
    {
      return true;
    }

    std::unique_ptr<Vajra::rack::RackExecutionSession> start(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        int client_fd,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport = nullptr) const override
    {
      return std::make_unique<SameProcessRackExecutionSession>(
          env_entries,
          client_fd,
          std::move(native_hijack_transport));
    }

    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        const std::string &request_body,
        int client_fd,
        std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream = nullptr,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport = nullptr) const override
    {
      if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }
      if (!same_process_execution_pool().running())
      {
        return std::nullopt;
      }
      auto task = std::make_shared<SameProcessDirectRackTask>(
          env_entries,
          request_body,
          client_fd,
          std::move(http2_stream),
          std::move(native_hijack_transport));
      same_process_execution_pool().enqueue(task);
      task->wait();
      const std::string error_message = task->error_message();
      if (!error_message.empty())
      {
        if (error_message == "same-process Rack execution pool is closed")
        {
          return std::nullopt;
        }
        throw std::runtime_error(error_message);
      }
      return task->take_response();
    }

    std::optional<Vajra::response::Response> execute(
        const std::vector<Vajra::request::RackEnvEntry> &env_entries,
        std::string &&request_body,
        int client_fd,
        std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream = nullptr,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport = nullptr) const override
    {
      if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
      {
        return std::nullopt;
      }
      if (!same_process_execution_pool().running())
      {
        return std::nullopt;
      }
      auto task = std::make_shared<SameProcessDirectRackTask>(
          env_entries,
          std::move(request_body),
          client_fd,
          std::move(http2_stream),
          std::move(native_hijack_transport));
      same_process_execution_pool().enqueue(task);
      task->wait();
      const std::string error_message = task->error_message();
      if (!error_message.empty())
      {
        if (error_message == "same-process Rack execution pool is closed")
        {
          return std::nullopt;
        }
        throw std::runtime_error(error_message);
      }
      return task->take_response();
    }

    bool execute_async(
        std::vector<Vajra::request::RackEnvEntry> env_entries,
        std::string request_body,
        int client_fd,
        std::shared_ptr<Vajra::rack::Http2StreamState> http2_stream,
        std::shared_ptr<Vajra::rack::NativeHijackTransport> native_hijack_transport,
        Vajra::request::RequestExecutor::CompletionCallback callback) const override
    {
      if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
      {
        return false;
      }
      if (!same_process_execution_pool().running())
      {
        return false;
      }
      auto task = std::make_shared<SameProcessDirectRackTask>(
          std::move(env_entries),
          std::move(request_body),
          client_fd,
          std::move(http2_stream),
          std::move(native_hijack_transport),
          std::move(callback));
      same_process_execution_pool().enqueue(task);
      return true;
    }
  };
}

void Vajra::rack::initialize_rack_execution_bridge()
{
  rb_global_variable(&rack_execution_callback);
  rb_global_variable(&rack_execution_app);
  initialize_native_input();
  initialize_http2_stream();
  RubyExecutionBridge::initialize();
}

void Vajra::rack::set_rack_execution_callback(VALUE callback)
{
  {
    const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
    rack_execution_callback = callback;
  }
  rack_execution_callback_installed_flag.store(!NIL_P(callback), std::memory_order_release);
}

void Vajra::rack::set_rack_execution_app(VALUE app)
{
  {
    const std::lock_guard<std::mutex> app_lock(rack_execution_app_mutex);
    rack_execution_app = app;
  }
  rack_execution_app_installed_flag.store(!NIL_P(app), std::memory_order_release);
  if (NIL_P(app))
  {
    shutdown_same_process_rack_execution_threads();
  }
  else
  {
    same_process_execution_pool().ensure_started();
  }
}

std::shared_ptr<const Vajra::rack::RackExecutionTransport> Vajra::rack::same_process_rack_execution_transport()
{
  return std::make_shared<SameProcessRackExecutionTransport>();
}

void Vajra::rack::configure_same_process_rack_execution_threads(std::size_t max_threads)
{
  same_process_execution_pool().configure(max_threads);
  if (rack_execution_callback_installed_flag.load(std::memory_order_acquire))
  {
    same_process_execution_pool().ensure_started();
  }
}

void Vajra::rack::ensure_same_process_rack_execution_threads_started()
{
  if (rack_execution_callback_installed_flag.load(std::memory_order_acquire))
  {
    same_process_execution_pool().restart_after_fork();
  }
}

bool Vajra::rack::wait_for_same_process_rack_execution_idle(std::chrono::milliseconds timeout)
{
  return same_process_execution_pool().wait_until_idle(timeout);
}

void Vajra::rack::shutdown_same_process_rack_execution_threads()
{
  same_process_execution_pool().shutdown();
}

std::optional<Vajra::response::Response> Vajra::rack::execute_current_thread_rack_request(
    const std::vector<Vajra::request::RackEnvEntry> &env_entries,
    const std::string &request_body,
    int client_fd)
{
  return execute_rack_request(env_entries, request_body, client_fd, nullptr, nullptr, false);
}

std::optional<Vajra::response::Response> Vajra::rack::execute_current_thread_rack_request(
    const std::vector<Vajra::request::RackEnvEntry> &env_entries,
    VALUE rack_input,
    int client_fd,
    std::shared_ptr<Vajra::rack::NativeInputState> input_state)
{
  return execute_rack_request(env_entries, rack_input, client_fd, std::move(input_state), nullptr, nullptr, false);
}
