// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifdef _WIN32

#include "runtime/native_runtime.hpp"

#include "rack/rack_request_executor.hpp"
#include "rack/ruby_rack_transport.hpp"
#include "runtime/boot_contract.hpp"
#include "runtime/runtime_logging.hpp"
#include "runtime/windows_worker_backend.hpp"
#include "vajra.hpp"

#include "ruby/thread.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace
{
  std::atomic_bool shutting_down{false};

  BOOL WINAPI handle_console_control(DWORD control_type)
  {
    switch (control_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      shutting_down.store(true, std::memory_order_release);
      return TRUE;
    default:
      return FALSE;
    }
  }

  class ConsoleControlGuard final
  {
  public:
    ConsoleControlGuard()
    {
#ifdef VAJRA_TEST_FAULT_INJECTION
      static std::atomic_bool handler_failure_injected{false};
      wchar_t test_fault[64]{};
      const DWORD test_fault_length = GetEnvironmentVariableW(
          L"VAJRA_WINDOWS_TEST_FAULT",
          test_fault,
          static_cast<DWORD>(sizeof(test_fault) / sizeof(test_fault[0])));
      if (test_fault_length > 0 && test_fault_length < sizeof(test_fault) / sizeof(test_fault[0]) &&
          std::wstring(test_fault, test_fault_length) == L"console_handler_failure" &&
          !handler_failure_injected.exchange(true, std::memory_order_acq_rel))
      {
        throw std::runtime_error("injected Windows console control handler failure");
      }
#endif
      if (SetConsoleCtrlHandler(handle_console_control, TRUE) == 0)
      {
        throw std::runtime_error("failed to install Windows console control handler");
      }
    }

    ~ConsoleControlGuard()
    {
      SetConsoleCtrlHandler(handle_console_control, FALSE);
    }
  };

  void *run_server_without_gvl(void *data)
  {
    auto *server = static_cast<Vajra::Server *>(data);
    server->start();
    return nullptr;
  }

  void stop_server_without_gvl(void *data)
  {
    auto *server = static_cast<Vajra::Server *>(data);
    server->stop();
  }

  void *run_windows_supervisor_without_gvl(void *data)
  {
    auto *supervisor = static_cast<Vajra::runtime::WindowsWorkerSupervisor *>(data);
    supervisor->run();
    return nullptr;
  }

  void stop_windows_supervisor_without_gvl(void *data)
  {
    auto *supervisor = static_cast<Vajra::runtime::WindowsWorkerSupervisor *>(data);
    supervisor->request_stop();
  }

  bool start_called_from_ruby_main_thread()
  {
    return rb_equal(rb_thread_current(), rb_thread_main()) == Qtrue;
  }
}

Vajra::runtime::NativeRuntime &Vajra::runtime::NativeRuntime::instance()
{
  static NativeRuntime runtime;
  return runtime;
}

bool Vajra::runtime::NativeRuntime::shutdown_requested()
{
  return shutting_down.load(std::memory_order_acquire);
}

bool Vajra::runtime::NativeRuntime::runtime_running() const
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  return server_instance_ != nullptr || windows_supervisor_ != nullptr || worker_startup_in_progress_;
}

bool Vajra::runtime::NativeRuntime::try_begin_startup()
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  if (server_instance_ != nullptr || windows_supervisor_ != nullptr || worker_startup_in_progress_)
  {
    return false;
  }
  worker_startup_in_progress_ = true;
  stop_requested_ = false;
  runtime_shutdown_started_ = false;
  return true;
}

void Vajra::runtime::NativeRuntime::install_server_instance(std::shared_ptr<Vajra::Server> server)
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  server_instance_ = std::move(server);
  worker_startup_in_progress_ = false;
}

std::shared_ptr<Vajra::Server> Vajra::runtime::NativeRuntime::take_server_instance()
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  return std::exchange(server_instance_, nullptr);
}

void Vajra::runtime::NativeRuntime::begin_runtime_shutdown()
{
  bool shutdown_started = false;
  {
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (!runtime_shutdown_started_)
    {
      runtime_shutdown_started_ = true;
      shutdown_started = true;
    }
    stop_requested_ = true;
  }
  shutting_down.store(true, std::memory_order_release);
  mark_runtime_shutdown_requested();
  mark_worker_lifecycle(0, WorkerLifecycleState::stopping);
  mark_worker_available(0, false);
  if (shutdown_started)
  {
    log_runtime_shutdown_begin();
  }
}

void Vajra::runtime::NativeRuntime::forward_shutdown_to_workers()
{
  stop();
}

void Vajra::runtime::NativeRuntime::stop()
{
  begin_runtime_shutdown();
  std::shared_ptr<Vajra::Server> server;
  std::shared_ptr<WindowsWorkerSupervisor> supervisor;
  {
    std::lock_guard<std::mutex> lock(server_mutex_);
    server = server_instance_;
    supervisor = windows_supervisor_;
  }
  if (server != nullptr)
  {
    server->stop();
  }
  if (supervisor != nullptr)
  {
    supervisor->request_stop();
  }
}

std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>> Vajra::runtime::NativeRuntime::worker_states() const
{
  std::lock_guard<std::mutex> lock(server_mutex_);
  if (windows_supervisor_ != nullptr)
  {
    return windows_supervisor_->worker_states();
  }
  return worker_states_;
}

void Vajra::runtime::NativeRuntime::start(const RuntimeConfig &config)
{
  if (windows_worker_bootstrap_present())
  {
    run_windows_worker_process(config);
    return;
  }
  if (!try_begin_startup())
  {
    std::cout << "Vajra already running" << std::endl;
    return;
  }

  shutting_down.store(false, std::memory_order_release);
  try
  {
    ConsoleControlGuard console_control_guard;
    if (!start_called_from_ruby_main_thread())
    {
      throw std::runtime_error("worker-only Vajra.start must be invoked from the Ruby main thread");
    }
    configure_runtime_logging(
        config.structured_logs,
        config.access_log,
        config.error_log,
        config.access_log_format);
    configure_runtime_tracing(
        config.trace_enabled,
        config.trace_endpoint,
        config.trace_service_name,
        config.trace_enabled && !config.trace_otel_owner,
        config.trace_resource_attributes,
        config.trace_propagators);
    const BootContractResult boot_result = BootContract::run(
        BootContractConfig{config.port, config.max_request_head_bytes, "ruby_master_preload"});
    BootContract::ensure_ready(boot_result);

    if (config.workers > 0)
    {
      if (runtime_state_ != nullptr)
      {
        release_runtime_state(runtime_state_);
      }
      const std::wstring mapping_name = L"Local\\vajra-runtime-" +
                                        std::to_wstring(platform::current_process_id()) + L"-" +
                                        std::to_wstring(GetTickCount64());
      runtime_state_ = allocate_named_runtime_state(mapping_name);
      auto supervisor = std::make_shared<WindowsWorkerSupervisor>(config, runtime_state_);
      {
        std::lock_guard<std::mutex> lock(server_mutex_);
        windows_supervisor_ = supervisor;
        worker_startup_in_progress_ = false;
      }
      start_runtime_logging_worker();
      start_runtime_tracing_worker();
      supervisor->start();
      rb_thread_call_without_gvl(
          run_windows_supervisor_without_gvl,
          supervisor.get(),
          stop_windows_supervisor_without_gvl,
          supervisor.get());
      const auto final_worker_states = supervisor->worker_states();
      const bool terminal_worker_failure = std::any_of(
          final_worker_states.begin(),
          final_worker_states.end(),
          [](const std::shared_ptr<SharedWorkerState> &state)
          {
            return state && state->terminal_replacement_failure.load(std::memory_order_acquire);
          });
      {
        std::lock_guard<std::mutex> lock(server_mutex_);
        windows_supervisor_.reset();
      }
      log_runtime_shutdown_complete();
      stop_runtime_tracing_worker();
      stop_runtime_logging_worker();
      release_runtime_state(runtime_state_);
      runtime_state_ = nullptr;
      if (terminal_worker_failure)
      {
        throw std::runtime_error("Windows worker replacement attempts exhausted");
      }
      return;
    }

    if (runtime_state_ != nullptr)
    {
      release_runtime_state(runtime_state_);
    }
    runtime_state_ = allocate_runtime_state();
    install_master_runtime_state(runtime_state_, 1, config.max_threads, config.socket_queue_capacity);
    install_worker_runtime_state(runtime_state_, 0, platform::current_process_id());

    start_runtime_logging_worker();
    start_runtime_tracing_worker();
    Vajra::rack::ensure_same_process_rack_execution_threads_started();
    mark_worker_lifecycle(0, WorkerLifecycleState::ready);
    mark_worker_health(0, WorkerHealthState::healthy);
    mark_worker_available(0, true);
    if (debug_logging_enabled(config.log_level))
    {
      log_worker_lifecycle_event(
          "worker_ready",
          0,
          platform::current_process_id(),
          WorkerLifecycleState::ready,
          WorkerHealthState::healthy,
          WorkerRecoveryState::none,
          true,
          WorkerExitClassification::none,
          false,
          false,
          0);
    }
    auto rack_executor = std::make_shared<Vajra::rack::RackRequestExecutor>(
        std::shared_ptr<const Vajra::rack::RackExecutionTransport>{},
        Vajra::rack::ControlPlaneConfig{config.stats_path, config.metrics_endpoint});
    auto tls_context = config.tls
                           ? std::make_shared<Vajra::transport::TlsContext>(Vajra::transport::TlsConfig{
                                 config.tls_certificate,
                                 config.tls_private_key,
                                 config.tls_ca_certificate,
                                 config.tls_verify_mode,
                                 config.tls_min_version,
                                 config.alpn_protocols,
                                 config.request_head_timeout_seconds,
                                 config.first_data_timeout_seconds,
                                 static_cast<int>(config.request_timeout_seconds)})
                           : nullptr;
    const Vajra::request::Http2Config http2_config{
        config.http2_max_concurrent_streams,
        config.http2_initial_window_size,
        config.http2_max_frame_size,
        config.http2_header_table_size,
        config.max_request_head_bytes,
        config.max_request_body_bytes,
        config.max_keepalive_requests,
        config.socket_queue_capacity};
    auto server = std::make_shared<Vajra::Server>(
        config.port,
        config.host,
        config.max_request_head_bytes,
        rack_executor,
        "windows_runtime",
        "single_process",
        config.workers,
        "same_process_rack_execution",
        debug_logging_enabled(config.log_level),
        -1,
        config.request_head_timeout_seconds,
        config.first_data_timeout_seconds,
        config.request_body_timeout_seconds,
        config.persistent_timeout_seconds,
        config.max_connections,
        [this]()
        { begin_runtime_shutdown(); },
        config.max_request_body_bytes,
        config.max_keepalive_requests,
        config.max_threads,
        config.http2,
        http2_config,
        std::move(tls_context),
        [host = config.host,
         workers = config.workers,
         min_threads = config.min_threads,
         max_threads = config.max_threads](int bound_port)
        {
          log_runtime_banner_start(host, bound_port, workers, min_threads, max_threads);
          flush_runtime_logs();
        });
    install_server_instance(server);
    rb_thread_call_without_gvl(
        run_server_without_gvl,
        server.get(),
        stop_server_without_gvl,
        server.get());
    take_server_instance();
    mark_worker_available(0, false);
    mark_worker_lifecycle(0, WorkerLifecycleState::exited);
    log_runtime_shutdown_complete();
    stop_runtime_tracing_worker();
    stop_runtime_logging_worker();
    release_runtime_state(runtime_state_);
    runtime_state_ = nullptr;
  }
  catch (...)
  {
    {
      std::lock_guard<std::mutex> lock(server_mutex_);
      server_instance_.reset();
      windows_supervisor_.reset();
      worker_startup_in_progress_ = false;
    }
    if (runtime_state_ != nullptr)
    {
      release_runtime_state(runtime_state_);
      runtime_state_ = nullptr;
    }
    stop_runtime_tracing_worker();
    stop_runtime_logging_worker();
    throw;
  }
}

bool VajraNative::shutdown_requested()
{
  return Vajra::runtime::NativeRuntime::shutdown_requested();
}

void VajraNative::begin_runtime_shutdown()
{
  Vajra::runtime::NativeRuntime::instance().begin_runtime_shutdown();
}

void VajraNative::start(
    std::string host,
    int port,
    int workers,
    std::size_t min_threads,
    std::size_t max_threads,
    std::size_t max_connections,
    std::size_t socket_queue_capacity,
    std::size_t max_request_head_bytes,
    std::size_t max_request_body_bytes,
    std::size_t max_keepalive_requests,
    std::size_t request_timeout_seconds,
    int request_head_timeout_seconds,
    int first_data_timeout_seconds,
    int request_body_timeout_seconds,
    int persistent_timeout_seconds,
    int worker_timeout_seconds,
    bool tls,
    std::string tls_certificate,
    std::string tls_private_key,
    std::string tls_ca_certificate,
    std::string tls_verify_mode,
    std::string tls_min_version,
    std::vector<std::string> alpn_protocols,
    bool http2,
    std::size_t http2_max_concurrent_streams,
    std::size_t http2_initial_window_size,
    std::size_t http2_max_frame_size,
    std::size_t http2_header_table_size,
    std::string log_level,
    std::string access_log,
    std::string error_log,
    bool structured_logs,
    std::string access_log_format,
    std::string stats_path,
    std::string metrics_endpoint,
    bool trace_enabled,
    std::string trace_endpoint,
    std::string trace_service_name,
    bool trace_otel_owner,
    std::string trace_resource_attributes,
    std::string trace_propagators)
{
  Vajra::runtime::NativeRuntime::instance().start(Vajra::runtime::RuntimeConfig{
      std::move(host),
      port,
      workers,
      min_threads,
      max_threads,
      max_connections,
      socket_queue_capacity,
      max_request_head_bytes,
      max_request_body_bytes,
      max_keepalive_requests,
      request_timeout_seconds,
      request_head_timeout_seconds,
      first_data_timeout_seconds,
      request_body_timeout_seconds,
      persistent_timeout_seconds,
      worker_timeout_seconds,
      tls,
      std::move(tls_certificate),
      std::move(tls_private_key),
      std::move(tls_ca_certificate),
      std::move(tls_verify_mode),
      std::move(tls_min_version),
      std::move(alpn_protocols),
      http2,
      http2_max_concurrent_streams,
      http2_initial_window_size,
      http2_max_frame_size,
      http2_header_table_size,
      std::move(log_level),
      std::move(access_log),
      std::move(error_log),
      structured_logs,
      std::move(access_log_format),
      std::move(stats_path),
      std::move(metrics_endpoint),
      trace_enabled,
      std::move(trace_endpoint),
      std::move(trace_service_name),
      trace_otel_owner,
      std::move(trace_resource_attributes),
      std::move(trace_propagators)});
}

void VajraNative::stop()
{
  Vajra::runtime::NativeRuntime::instance().stop();
}

#endif
