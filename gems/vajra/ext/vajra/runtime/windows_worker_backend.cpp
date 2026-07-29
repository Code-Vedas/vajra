// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifdef _WIN32

#include "runtime/windows_worker_backend.hpp"

#include "listener/listener_socket.hpp"
#include "platform/process.hpp"
#include "platform/socket.hpp"
#include "rack/rack_request_executor.hpp"
#include "rack/ruby_rack_transport.hpp"
#include "runtime/boot_contract.hpp"
#include "runtime/native_runtime.hpp"
#include "runtime/runtime_logging.hpp"
#include "server.hpp"
#include "vajra.hpp"

#include "ruby/thread.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <windows.h>

namespace
{
  constexpr std::uint32_t kFrameMagic = 0x56574a52;
  constexpr std::uint16_t kFrameVersion = 1;
  constexpr std::size_t kMaximumFramePayload = 1024 * 1024;
  constexpr DWORD kPipeBufferBytes = 64 * 1024;
  constexpr wchar_t kWorkerPipeEnvironment[] = L"VAJRA_WINDOWS_WORKER_PIPE";
  constexpr wchar_t kRuntimeMappingEnvironment[] = L"VAJRA_WINDOWS_RUNTIME_MAPPING_HANDLE";
  constexpr wchar_t kParentShutdownEnvironment[] = L"VAJRA_WINDOWS_PARENT_SHUTDOWN_HANDLE";
#ifdef VAJRA_TEST_FAULT_INJECTION
  constexpr wchar_t kTestFaultEnvironment[] = L"VAJRA_WINDOWS_TEST_FAULT";
#endif

  enum class FrameKind : std::uint16_t
  {
    bootstrap = 1,
    ready = 2,
    socket_dispatch = 3,
    socket_ack = 4,
    shutdown = 5,
    stopped = 6,
    failure = 7,
  };

#pragma pack(push, 1)
  struct FrameHeader
  {
    std::uint32_t magic = kFrameMagic;
    std::uint16_t version = kFrameVersion;
    FrameKind kind = FrameKind::failure;
    std::uint32_t payload_length = 0;
    std::uint32_t worker_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
  };

  struct SocketDispatchPayload
  {
    WSAPROTOCOL_INFOW protocol_info{};
  };

  struct SocketAckPayload
  {
    std::uint8_t accepted = 0;
    std::int32_t error_code = 0;
  };
#pragma pack(pop)

  class Handle final
  {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle &operator=(Handle &&other) noexcept
    {
      if (this != &other)
      {
        reset(std::exchange(other.value_, nullptr));
      }
      return *this;
    }
    HANDLE get() const { return value_; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr)
    {
      if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
      {
        CloseHandle(value_);
      }
      value_ = value;
    }
    explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_ = nullptr;
  };

  std::runtime_error windows_error(const std::string &operation, DWORD error = GetLastError())
  {
    return std::runtime_error(operation + " failed with Windows error " + std::to_string(error));
  }

  std::wstring environment_value(const wchar_t *name)
  {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0)
    {
      return L"";
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written == 0 || written >= length)
    {
      return L"";
    }
    value.resize(written);
    return value;
  }

  std::uint64_t unsigned_value(const std::wstring &value, const char *name)
  {
    if (value.empty())
    {
      throw std::runtime_error(std::string("missing Windows worker bootstrap value: ") + name);
    }
    wchar_t *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != L'\0')
    {
      throw std::runtime_error(std::string("invalid Windows worker bootstrap value: ") + name);
    }
    return static_cast<std::uint64_t>(parsed);
  }

  std::wstring unique_name(const wchar_t *kind, std::size_t index, std::uint64_t generation)
  {
    std::wostringstream name;
    name << L"Local\\vajra-" << kind << L'-' << GetCurrentProcessId() << L'-'
         << GetTickCount64() << L'-' << index << L'-' << generation;
    return name.str();
  }

  std::wstring pipe_path(const std::wstring &name)
  {
    return L"\\\\.\\pipe\\" + name;
  }

  void cancel_and_drain_overlapped(HANDLE object, OVERLAPPED &overlapped)
  {
    if (CancelIoEx(object, &overlapped) == 0 && GetLastError() != ERROR_NOT_FOUND)
    {
      return;
    }
    DWORD ignored = 0;
    (void)GetOverlappedResult(object, &overlapped, &ignored, TRUE);
  }

  bool wait_overlapped(HANDLE object, OVERLAPPED &overlapped, DWORD timeout, DWORD *transferred)
  {
    const DWORD wait_status = WaitForSingleObject(overlapped.hEvent, timeout);
    if (wait_status != WAIT_OBJECT_0)
    {
      cancel_and_drain_overlapped(object, overlapped);
      return false;
    }
    return GetOverlappedResult(object, &overlapped, transferred, FALSE) != 0;
  }

  bool connect_named_pipe(HANDLE pipe, HANDLE process, DWORD timeout)
  {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
    {
      return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped) != 0)
    {
      return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
    {
      return true;
    }
    if (error != ERROR_IO_PENDING)
    {
      return false;
    }
    HANDLE objects[] = {event.get(), process};
    const DWORD wait_status = WaitForMultipleObjects(2, objects, FALSE, timeout);
    if (wait_status != WAIT_OBJECT_0)
    {
      cancel_and_drain_overlapped(pipe, overlapped);
      return false;
    }
    DWORD transferred = 0;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != 0;
  }

  bool write_message(HANDLE pipe, const void *data, DWORD length, DWORD timeout)
  {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
    {
      return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    if (WriteFile(pipe, data, length, &transferred, &overlapped) != 0)
    {
      return transferred == length;
    }
    if (GetLastError() != ERROR_IO_PENDING || !wait_overlapped(pipe, overlapped, timeout, &transferred))
    {
      return false;
    }
    return transferred == length;
  }

  bool read_message(HANDLE pipe, void *data, DWORD length, DWORD timeout)
  {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
    {
      return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    if (ReadFile(pipe, data, length, &transferred, &overlapped) != 0)
    {
      return transferred == length;
    }
    if (GetLastError() != ERROR_IO_PENDING || !wait_overlapped(pipe, overlapped, timeout, &transferred))
    {
      return false;
    }
    return transferred == length;
  }

  bool write_frame(
      HANDLE pipe,
      FrameKind kind,
      std::size_t worker_index,
      std::uint64_t generation,
      std::uint64_t sequence,
      const std::vector<std::uint8_t> &payload,
      DWORD timeout)
  {
    if (payload.size() > kMaximumFramePayload || payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
      return false;
    }
    const FrameHeader header{
        kFrameMagic,
        kFrameVersion,
        kind,
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(worker_index),
        generation,
        sequence};
    if (!write_message(pipe, &header, sizeof(header), timeout))
    {
      return false;
    }
    return payload.empty() || write_message(pipe, payload.data(), static_cast<DWORD>(payload.size()), timeout);
  }

  bool read_frame(HANDLE pipe, FrameHeader &header, std::vector<std::uint8_t> &payload, DWORD timeout)
  {
    if (!read_message(pipe, &header, sizeof(header), timeout) ||
        header.magic != kFrameMagic || header.version != kFrameVersion ||
        header.payload_length > kMaximumFramePayload)
    {
      return false;
    }
    payload.resize(header.payload_length);
    return payload.empty() || read_message(pipe, payload.data(), header.payload_length, timeout);
  }

  bool child_write_message(HANDLE pipe, const void *data, DWORD length)
  {
    DWORD transferred = 0;
    return WriteFile(pipe, data, length, &transferred, nullptr) != 0 && transferred == length;
  }

  bool child_read_message(HANDLE pipe, void *data, DWORD length)
  {
    DWORD transferred = 0;
    return ReadFile(pipe, data, length, &transferred, nullptr) != 0 && transferred == length;
  }

  bool child_write_frame(
      HANDLE pipe,
      FrameKind kind,
      std::size_t worker_index,
      std::uint64_t generation,
      std::uint64_t sequence,
      const std::vector<std::uint8_t> &payload)
  {
    if (payload.size() > kMaximumFramePayload || payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
      return false;
    }
    const FrameHeader header{
        kFrameMagic,
        kFrameVersion,
        kind,
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(worker_index),
        generation,
        sequence};
    return child_write_message(pipe, &header, sizeof(header)) &&
           (payload.empty() || child_write_message(pipe, payload.data(), static_cast<DWORD>(payload.size())));
  }

  bool child_read_frame(HANDLE pipe, FrameHeader &header, std::vector<std::uint8_t> &payload)
  {
    if (!child_read_message(pipe, &header, sizeof(header)) ||
        header.magic != kFrameMagic || header.version != kFrameVersion ||
        header.payload_length > kMaximumFramePayload)
    {
      return false;
    }
    payload.resize(header.payload_length);
    return payload.empty() || child_read_message(pipe, payload.data(), header.payload_length);
  }

  class BinaryWriter final
  {
  public:
    template <typename Value>
    void scalar(Value value)
    {
      static_assert(std::is_trivially_copyable_v<Value>);
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
      data_.insert(data_.end(), bytes, bytes + sizeof(value));
    }

    void string(const std::string &value)
    {
      if (value.size() > std::numeric_limits<std::uint32_t>::max())
      {
        throw std::runtime_error("Windows worker bootstrap string is too large");
      }
      scalar(static_cast<std::uint32_t>(value.size()));
      data_.insert(data_.end(), value.begin(), value.end());
    }

    void strings(const std::vector<std::string> &values)
    {
      scalar(static_cast<std::uint32_t>(values.size()));
      for (const std::string &value : values)
      {
        string(value);
      }
    }

    std::vector<std::uint8_t> finish() { return std::move(data_); }

  private:
    std::vector<std::uint8_t> data_;
  };

  class BinaryReader final
  {
  public:
    explicit BinaryReader(const std::vector<std::uint8_t> &data) : data_(data) {}

    template <typename Value>
    Value scalar()
    {
      static_assert(std::is_trivially_copyable_v<Value>);
      require(sizeof(Value));
      Value value{};
      std::memcpy(&value, data_.data() + offset_, sizeof(Value));
      offset_ += sizeof(Value);
      return value;
    }

    std::string string()
    {
      const std::uint32_t length = scalar<std::uint32_t>();
      require(length);
      std::string value(reinterpret_cast<const char *>(data_.data() + offset_), length);
      offset_ += length;
      return value;
    }

    std::vector<std::string> strings()
    {
      const std::uint32_t count = scalar<std::uint32_t>();
      if (count > 1024)
      {
        throw std::runtime_error("Windows worker bootstrap string list is too large");
      }
      std::vector<std::string> values;
      values.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index)
      {
        values.push_back(string());
      }
      return values;
    }

    void ensure_complete() const
    {
      if (offset_ != data_.size())
      {
        throw std::runtime_error("Windows worker bootstrap contains trailing bytes");
      }
    }

  private:
    void require(std::size_t length)
    {
      if (length > data_.size() - std::min(offset_, data_.size()))
      {
        throw std::runtime_error("truncated Windows worker bootstrap payload");
      }
    }
    const std::vector<std::uint8_t> &data_;
    std::size_t offset_ = 0;
  };

  std::vector<std::uint8_t> serialize_config(const Vajra::runtime::RuntimeConfig &config)
  {
    BinaryWriter writer;
    writer.string(config.host);
    writer.scalar(config.port);
    writer.scalar(config.workers);
    writer.scalar<std::uint64_t>(config.min_threads);
    writer.scalar<std::uint64_t>(config.max_threads);
    writer.scalar<std::uint64_t>(config.max_connections);
    writer.scalar<std::uint64_t>(config.socket_queue_capacity);
    writer.scalar<std::uint64_t>(config.max_request_head_bytes);
    writer.scalar<std::uint64_t>(config.max_request_body_bytes);
    writer.scalar<std::uint64_t>(config.max_keepalive_requests);
    writer.scalar<std::uint64_t>(config.request_timeout_seconds);
    writer.scalar(config.request_head_timeout_seconds);
    writer.scalar(config.first_data_timeout_seconds);
    writer.scalar(config.request_body_timeout_seconds);
    writer.scalar(config.persistent_timeout_seconds);
    writer.scalar(config.worker_timeout_seconds);
    writer.scalar<std::uint8_t>(config.tls ? 1 : 0);
    writer.string(config.tls_certificate);
    writer.string(config.tls_private_key);
    writer.string(config.tls_ca_certificate);
    writer.string(config.tls_verify_mode);
    writer.string(config.tls_min_version);
    writer.strings(config.alpn_protocols);
    writer.scalar<std::uint8_t>(config.http2 ? 1 : 0);
    writer.scalar<std::uint64_t>(config.http2_max_concurrent_streams);
    writer.scalar<std::uint64_t>(config.http2_initial_window_size);
    writer.scalar<std::uint64_t>(config.http2_max_frame_size);
    writer.scalar<std::uint64_t>(config.http2_header_table_size);
    writer.string(config.log_level);
    writer.string(config.access_log);
    writer.string(config.error_log);
    writer.scalar<std::uint8_t>(config.structured_logs ? 1 : 0);
    writer.string(config.access_log_format);
    writer.string(config.stats_path);
    writer.string(config.metrics_endpoint);
    writer.scalar<std::uint8_t>(config.trace_enabled ? 1 : 0);
    writer.string(config.trace_endpoint);
    writer.string(config.trace_service_name);
    writer.scalar<std::uint8_t>(config.trace_otel_owner ? 1 : 0);
    writer.string(config.trace_resource_attributes);
    writer.string(config.trace_propagators);
    return writer.finish();
  }

  Vajra::runtime::RuntimeConfig deserialize_config(const std::vector<std::uint8_t> &payload)
  {
    BinaryReader reader(payload);
    Vajra::runtime::RuntimeConfig config{
        reader.string(),
        reader.scalar<int>(),
        reader.scalar<int>(),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        reader.scalar<int>(),
        reader.scalar<int>(),
        reader.scalar<int>(),
        reader.scalar<int>(),
        reader.scalar<int>(),
        reader.scalar<std::uint8_t>() != 0,
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.strings(),
        reader.scalar<std::uint8_t>() != 0,
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        static_cast<std::size_t>(reader.scalar<std::uint64_t>()),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.scalar<std::uint8_t>() != 0,
        reader.string(),
        reader.string(),
        reader.string(),
        reader.scalar<std::uint8_t>() != 0,
        reader.string(),
        reader.string(),
        reader.scalar<std::uint8_t>() != 0,
        reader.string(),
        reader.string()};
    reader.ensure_complete();
    return config;
  }

  std::shared_ptr<Vajra::Server> build_dispatch_server(
      const Vajra::runtime::RuntimeConfig &config,
      std::size_t worker_index)
  {
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
    return std::make_shared<Vajra::Server>(
        config.port,
        config.host,
        config.max_request_head_bytes,
        rack_executor,
        "windows_worker_" + std::to_string(worker_index),
        "worker_process",
        config.workers,
        "same_process_rack_execution",
        Vajra::runtime::debug_logging_enabled(config.log_level),
        Vajra::platform::kInvalidSocket,
        config.request_head_timeout_seconds,
        config.first_data_timeout_seconds,
        config.request_body_timeout_seconds,
        config.persistent_timeout_seconds,
        config.max_connections,
        std::function<void()>{},
        config.max_request_body_bytes,
        config.max_keepalive_requests,
        config.max_threads,
        config.http2,
        http2_config,
        std::move(tls_context));
  }

  std::vector<wchar_t> environment_block_with(
      const std::vector<std::pair<std::wstring, std::wstring>> &updates)
  {
    LPWCH current = GetEnvironmentStringsW();
    if (current == nullptr)
    {
      throw windows_error("GetEnvironmentStringsW");
    }
    std::vector<std::wstring> entries;
    for (const wchar_t *cursor = current; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1)
    {
      const std::wstring entry(cursor);
      bool replaced = false;
      for (const auto &[name, value] : updates)
      {
        const std::wstring prefix = name + L"=";
        if (entry.size() >= prefix.size() && _wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0)
        {
          replaced = true;
          break;
        }
      }
      if (!replaced)
      {
        entries.push_back(entry);
      }
    }
    FreeEnvironmentStringsW(current);
    for (const auto &[name, value] : updates)
    {
      entries.push_back(name + L"=" + value);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring &left, const std::wstring &right)
              { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const std::wstring &entry : entries)
    {
      block.insert(block.end(), entry.begin(), entry.end());
      block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
  }

  std::vector<std::uint8_t> bytes_of(const void *data, std::size_t length)
  {
    const auto *begin = static_cast<const std::uint8_t *>(data);
    return std::vector<std::uint8_t>(begin, begin + length);
  }
}

struct Vajra::runtime::WindowsWorkerSupervisor::Implementation
{
  struct Worker
  {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t replacement_failures = 0;
    Handle pipe;
    Handle process;
    platform::ProcessId process_id = platform::kInvalidProcessId;
    std::shared_ptr<SharedWorkerState> state;
    bool ready = false;
    bool replacement_pending = false;
  };

  RuntimeConfig config;
  RuntimeState *runtime_state = nullptr;
  listener::SocketBinding listener_binding{platform::kInvalidSocket, -1};
  Handle job;
  Handle parent_shutdown_event;
  Handle supervisor_stop_event;
  mutable std::mutex mutex;
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::shared_ptr<SharedWorkerState>> public_states;
  std::atomic_bool stop_requested{false};
  std::once_flag cleanup_once;
  std::size_t next_worker = 0;
  std::deque<platform::SocketHandle> pending_clients;
#ifdef VAJRA_TEST_FAULT_INJECTION
  std::wstring test_fault = environment_value(kTestFaultEnvironment);
  bool test_fault_injected = false;
  std::uint64_t successful_dispatches = 0;
#endif

  explicit Implementation(RuntimeConfig value, RuntimeState *state)
      : config(std::move(value)), runtime_state(state)
  {
  }

  DWORD io_timeout() const
  {
    const int seconds = std::max(1, config.worker_timeout_seconds);
    return static_cast<DWORD>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(seconds) * 1000,
        std::numeric_limits<DWORD>::max()));
  }

  void initialize_job()
  {
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job)
    {
      throw windows_error("CreateJobObjectW");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0)
    {
      throw windows_error("SetInformationJobObject");
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    parent_shutdown_event.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
    if (!parent_shutdown_event)
    {
      throw windows_error("CreateEventW");
    }
    supervisor_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!supervisor_stop_event)
    {
      throw windows_error("CreateEventW");
    }
  }

  bool spawn_worker(Worker &worker)
  {
    worker.generation += 1;
#ifdef VAJRA_TEST_FAULT_INJECTION
    if (test_fault == L"replacement_exhaustion" && worker.generation > 1)
    {
      return false;
    }
#endif
    worker.sequence = 0;
    worker.ready = false;
    const std::wstring pipe_name = unique_name(L"worker", worker.index, worker.generation);
    const std::wstring full_pipe_path = pipe_path(pipe_name);
    worker.pipe.reset(CreateNamedPipeW(
        full_pipe_path.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        kPipeBufferBytes,
        kPipeBufferBytes,
        0,
        nullptr));
    if (!worker.pipe)
    {
      return false;
    }

    const HANDLE mapping = runtime_state_mapping_handle(runtime_state);
    if (mapping == nullptr)
    {
      throw std::runtime_error("Windows runtime state mapping handle is unavailable");
    }
    auto duplicate_inheritable = [](DWORD standard_handle) -> Handle
    {
      const HANDLE source = GetStdHandle(standard_handle);
      if (source == nullptr || source == INVALID_HANDLE_VALUE)
      {
        return Handle{};
      }
      HANDLE duplicate = nullptr;
      if (DuplicateHandle(
              GetCurrentProcess(),
              source,
              GetCurrentProcess(),
              &duplicate,
              0,
              TRUE,
              DUPLICATE_SAME_ACCESS) == 0)
      {
        return Handle{};
      }
      return Handle(duplicate);
    };
    SECURITY_ATTRIBUTES standard_handle_attributes{};
    standard_handle_attributes.nLength = sizeof(standard_handle_attributes);
    standard_handle_attributes.bInheritHandle = TRUE;
    Handle child_stdin(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &standard_handle_attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!child_stdin)
    {
      return false;
    }
    Handle child_stdout = duplicate_inheritable(STD_OUTPUT_HANDLE);
    Handle child_stderr = duplicate_inheritable(STD_ERROR_HANDLE);
    std::vector<HANDLE> inherited_handles{mapping, parent_shutdown_event.get()};
    for (const Handle *standard : {&child_stdin, &child_stdout, &child_stderr})
    {
      if (*standard)
      {
        inherited_handles.push_back(standard->get());
      }
    }
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<std::uint8_t> attribute_storage(attribute_bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes) == 0)
    {
      return false;
    }
    struct AttributeGuard
    {
      LPPROC_THREAD_ATTRIBUTE_LIST value;
      ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    if (UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            inherited_handles.size() * sizeof(HANDLE),
            nullptr,
            nullptr) == 0)
    {
      return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    if (child_stdin && child_stdout && child_stderr)
    {
      startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdInput = child_stdin.get();
      startup.StartupInfo.hStdOutput = child_stdout.get();
      startup.StartupInfo.hStdError = child_stderr.get();
    }
    PROCESS_INFORMATION process{};
    std::wstring command_line(GetCommandLineW());
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    std::array<wchar_t, MAX_PATH + 1> temporary_directory{};
    const DWORD temporary_length = GetTempPathW(
        static_cast<DWORD>(temporary_directory.size()),
        temporary_directory.data());
    if (temporary_length == 0 || temporary_length >= temporary_directory.size())
    {
      return false;
    }
    const std::wstring worker_pidfile =
        std::wstring(temporary_directory.data(), temporary_length) +
        L"vajra-worker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(worker.index) + L"-" + std::to_wstring(worker.generation) + L".pid";
    const std::vector<wchar_t> environment = environment_block_with({{kWorkerPipeEnvironment, full_pipe_path},
                                                                     {kRuntimeMappingEnvironment, std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping))},
                                                                     {kParentShutdownEnvironment, std::to_wstring(reinterpret_cast<std::uintptr_t>(parent_shutdown_event.get()))},
                                                                     {L"PIDFILE", worker_pidfile}});
    const DWORD creation_flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (CreateProcessW(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            creation_flags,
            const_cast<wchar_t *>(environment.data()),
            nullptr,
            &startup.StartupInfo,
            &process) == 0)
    {
      return false;
    }
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    if (AssignProcessToJobObject(job.get(), process_handle.get()) == 0 || ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1))
    {
      TerminateProcess(process_handle.get(), 1);
      return false;
    }
    const DWORD connect_timeout = std::max<DWORD>(io_timeout(), 30'000);
    if (!connect_named_pipe(worker.pipe.get(), process_handle.get(), connect_timeout))
    {
      DWORD exit_code = STILL_ACTIVE;
      GetExitCodeProcess(process_handle.get(), &exit_code);
      TerminateProcess(process_handle.get(), 1);
      throw std::runtime_error(
          "Windows worker failed to connect its bootstrap pipe: index=" + std::to_string(worker.index) +
          " exit_code=" + std::to_string(exit_code) +
          " windows_error=" + std::to_string(GetLastError()));
    }

    worker.process_id = process.dwProcessId;
    worker.process = std::move(process_handle);
    if (!write_frame(
            worker.pipe.get(),
            FrameKind::bootstrap,
            worker.index,
            worker.generation,
            0,
            serialize_config(config),
            io_timeout()))
    {
      TerminateProcess(worker.process.get(), 1);
      return false;
    }
    FrameHeader ready_header{};
    std::vector<std::uint8_t> ready_payload;
    if (!read_frame(worker.pipe.get(), ready_header, ready_payload, io_timeout()) ||
        ready_header.kind != FrameKind::ready || ready_header.worker_index != worker.index ||
        ready_header.generation != worker.generation || !ready_payload.empty())
    {
      TerminateProcess(worker.process.get(), 1);
      return false;
    }

    worker.state = std::make_shared<SharedWorkerState>(worker.index, worker.process_id, std::vector<int>{});
    worker.state->lifecycle_state.store(WorkerLifecycleState::ready, std::memory_order_release);
    worker.state->health_state.store(WorkerHealthState::healthy, std::memory_order_release);
    worker.state->available.store(true, std::memory_order_release);
    worker.state->channel_generation.store(worker.generation, std::memory_order_release);
    worker.state->last_progress_nanoseconds.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_release);
    install_worker_runtime_state(runtime_state, worker.index, worker.process_id);
    detach_worker_runtime_state();
    mark_worker_lifecycle(worker.index, WorkerLifecycleState::ready);
    mark_worker_health(worker.index, WorkerHealthState::healthy);
    mark_worker_available(worker.index, true);
    worker.ready = true;
    return true;
  }

  void start()
  {
    initialize_job();
    listener::Socket listener_socket;
    listener_binding = listener_socket.open(config.host, config.port);
    config.port = listener_binding.port;
    install_master_runtime_state(runtime_state, config.workers, config.max_threads, config.socket_queue_capacity);

    workers.reserve(static_cast<std::size_t>(config.workers));
    public_states.reserve(static_cast<std::size_t>(config.workers));
    std::vector<double> worker_boot_seconds;
    worker_boot_seconds.reserve(static_cast<std::size_t>(config.workers));
    for (int index = 0; index < config.workers; ++index)
    {
      const auto boot_started = std::chrono::steady_clock::now();
      auto worker = std::make_unique<Worker>();
      worker->index = static_cast<std::size_t>(index);
      if (!spawn_worker(*worker))
      {
        throw std::runtime_error("Windows worker failed to become ready: index=" + std::to_string(index));
      }
      const double elapsed_boot_seconds = std::chrono::duration<double>(
                                              std::chrono::steady_clock::now() - boot_started)
                                              .count();
      worker_boot_seconds.push_back(elapsed_boot_seconds);
      public_states.push_back(worker->state);
      workers.push_back(std::move(worker));
    }
    log_runtime_banner_start(
        config.host,
        listener_binding.port,
        config.workers,
        config.min_threads,
        config.max_threads);
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
      const Worker &worker = *workers[index];
      log_worker_booted(
          static_cast<int>(index),
          worker.process_id,
          Vajra::platform::current_process_id(),
          worker_boot_seconds[index]);
      if (debug_logging_enabled(config.log_level))
      {
        log_worker_lifecycle_event(
            "worker_registered",
            worker.index,
            worker.process_id,
            WorkerLifecycleState::booting,
            WorkerHealthState::healthy,
            WorkerRecoveryState::none,
            false,
            WorkerExitClassification::none,
            false,
            false,
            0);
        log_worker_lifecycle_event(
            "worker_ready",
            worker.index,
            worker.process_id,
            WorkerLifecycleState::ready,
            WorkerHealthState::healthy,
            WorkerRecoveryState::none,
            true,
            WorkerExitClassification::none,
            false,
            false,
            0);
      }
    }
    flush_runtime_logs();
  }

  bool replace_worker(Worker &worker)
  {
    const bool observe_unexpected_exit = !worker.replacement_pending;
    const std::uint64_t replacement_attempts = worker.state
                                                   ? worker.state->replacement_attempt_count.load(std::memory_order_acquire) + 1
                                                   : 1;
    const std::uint64_t replacement_successes = worker.state
                                                    ? worker.state->replacement_success_count.load(std::memory_order_acquire)
                                                    : 0;
    const std::uint64_t replacement_failures = worker.state
                                                   ? worker.state->replacement_failure_count.load(std::memory_order_acquire)
                                                   : 0;
    const std::uint64_t unexpected_exits = worker.state
                                               ? worker.state->unexpected_exit_count.load(std::memory_order_acquire) +
                                                     (observe_unexpected_exit ? 1 : 0)
                                               : (observe_unexpected_exit ? 1 : 0);
    const std::int64_t unexpected_exit_time = observe_unexpected_exit
                                                  ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        std::chrono::steady_clock::now().time_since_epoch())
                                                        .count()
                                                  : (worker.state ? worker.state->last_unexpected_exit_nanoseconds.load(
                                                                        std::memory_order_acquire)
                                                                  : 0);
    worker.replacement_pending = true;
    worker.ready = false;
    mark_worker_available(worker.index, false);
    mark_worker_lifecycle(worker.index, WorkerLifecycleState::exited);
    if (worker.state)
    {
      worker.state->available.store(false, std::memory_order_release);
      worker.state->lifecycle_state.store(WorkerLifecycleState::exited, std::memory_order_release);
      worker.state->replacement_attempt_count.store(replacement_attempts, std::memory_order_release);
      worker.state->unexpected_exit_count.store(unexpected_exits, std::memory_order_release);
      worker.state->last_unexpected_exit_nanoseconds.store(unexpected_exit_time, std::memory_order_release);
    }
    mark_worker_unexpected_exit(worker.index, unexpected_exits, unexpected_exit_time);
    worker.pipe.reset();
    if (worker.process && WaitForSingleObject(worker.process.get(), 0) == WAIT_TIMEOUT)
    {
      TerminateProcess(worker.process.get(), 1);
      WaitForSingleObject(worker.process.get(), 5000);
      if (worker.state)
      {
        const std::uint64_t escalations = worker.state->timeout_escalation_count.fetch_add(
                                              1,
                                              std::memory_order_acq_rel) +
                                          1;
        mark_worker_timeout_escalations(worker.index, escalations);
      }
    }
    worker.process.reset();
    if (stop_requested.load(std::memory_order_acquire))
    {
      return false;
    }
    if (!spawn_worker(worker))
    {
      worker.replacement_failures += 1;
      if (worker.state)
      {
        worker.state->replacement_attempt_count.store(replacement_attempts, std::memory_order_release);
        worker.state->replacement_success_count.store(replacement_successes, std::memory_order_release);
        worker.state->replacement_failure_count.store(
            replacement_failures + 1,
            std::memory_order_release);
      }
      if (worker.replacement_failures >= 3)
      {
        if (worker.state)
        {
          worker.state->terminal_replacement_failure.store(true, std::memory_order_release);
          worker.state->recovery_state.store(WorkerRecoveryState::terminal_failure, std::memory_order_release);
        }
        mark_worker_terminal_replacement_failure(worker.index, true);
        if (debug_logging_enabled(config.log_level))
        {
          log_worker_lifecycle_event(
              "worker_replacement_terminal_failure",
              worker.index,
              worker.process_id,
              WorkerLifecycleState::exited,
              WorkerHealthState::wedged,
              WorkerRecoveryState::terminal_failure,
              false,
              WorkerExitClassification::unexpected_exit,
              true,
              false,
              static_cast<int>(replacement_attempts));
        }
        stop_requested.store(true, std::memory_order_release);
      }
      return false;
    }
    worker.replacement_failures = 0;
    worker.replacement_pending = false;
    if (worker.state)
    {
      worker.state->replacement_attempt_count.store(replacement_attempts, std::memory_order_release);
      worker.state->replacement_success_count.store(replacement_successes + 1, std::memory_order_release);
      worker.state->replacement_failure_count.store(replacement_failures, std::memory_order_release);
      worker.state->unexpected_exit_count.store(unexpected_exits, std::memory_order_release);
      worker.state->last_unexpected_exit_nanoseconds.store(unexpected_exit_time, std::memory_order_release);
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      public_states[worker.index] = worker.state;
    }
    mark_worker_replacement_counters(
        worker.index,
        replacement_attempts,
        replacement_successes + 1,
        replacement_failures);
    if (debug_logging_enabled(config.log_level))
    {
      log_worker_lifecycle_event(
          "worker_replacement_ready",
          worker.index,
          worker.process_id,
          WorkerLifecycleState::ready,
          WorkerHealthState::healthy,
          WorkerRecoveryState::none,
          true,
          WorkerExitClassification::none,
          false,
          false,
          static_cast<int>(replacement_attempts));
    }
    return true;
  }

  Worker *next_ready_worker()
  {
    if (workers.empty())
    {
      return nullptr;
    }
    for (std::size_t attempt = 0; attempt < workers.size(); ++attempt)
    {
      Worker &worker = *workers[next_worker++ % workers.size()];
      if (worker.ready && WaitForSingleObject(worker.process.get(), 0) == WAIT_TIMEOUT)
      {
        return &worker;
      }
      if (worker.ready)
      {
        replace_worker(worker);
      }
    }
    return nullptr;
  }

  bool dispatch_socket(Worker &worker, platform::SocketHandle client)
  {
    SocketDispatchPayload dispatch{};
    if (WSADuplicateSocketW(client, worker.process_id, &dispatch.protocol_info) != 0)
    {
      return false;
    }
    const std::uint64_t sequence = ++worker.sequence;
#ifdef VAJRA_TEST_FAULT_INJECTION
    const bool duplicate_ready = test_fault == L"duplicate_dispatch" && successful_dispatches > 0;
    const bool frame_fault = test_fault == L"malformed_frame" || test_fault == L"oversized_frame" ||
                             test_fault == L"unknown_frame" || test_fault == L"stale_generation" ||
                             test_fault == L"duplicate_dispatch" || test_fault == L"partial_socket_transfer";
    const bool inject_frame = !test_fault_injected && frame_fault &&
                              (test_fault != L"duplicate_dispatch" || duplicate_ready);
    if (inject_frame)
    {
      FrameHeader header{
          kFrameMagic,
          kFrameVersion,
          FrameKind::socket_dispatch,
          static_cast<std::uint32_t>(sizeof(dispatch)),
          static_cast<std::uint32_t>(worker.index),
          worker.generation,
          sequence};
      if (test_fault == L"malformed_frame")
      {
        header.magic = 0;
      }
      else if (test_fault == L"oversized_frame")
      {
        header.payload_length = static_cast<std::uint32_t>(kMaximumFramePayload + 1);
      }
      else if (test_fault == L"unknown_frame")
      {
        header.kind = static_cast<FrameKind>(std::numeric_limits<std::uint16_t>::max());
      }
      else if (test_fault == L"stale_generation")
      {
        header.generation = worker.generation == 0 ? 1 : worker.generation - 1;
      }
      else if (test_fault == L"duplicate_dispatch")
      {
        header.sequence = sequence - 1;
      }
      else if (test_fault == L"partial_socket_transfer")
      {
        // Keep the valid header and truncate only its protocol-info payload.
      }
      else
      {
        return false;
      }
      test_fault_injected = true;
      const DWORD dispatch_bytes = test_fault == L"partial_socket_transfer"
                                       ? static_cast<DWORD>(sizeof(dispatch) / 2)
                                       : static_cast<DWORD>(sizeof(dispatch));
      if (!write_message(worker.pipe.get(), &header, sizeof(header), io_timeout()) ||
          (test_fault != L"oversized_frame" &&
           !write_message(worker.pipe.get(), &dispatch, dispatch_bytes, io_timeout())))
      {
        replace_worker(worker);
        return false;
      }
    }
    else
#endif
        if (!write_frame(
                worker.pipe.get(),
                FrameKind::socket_dispatch,
                worker.index,
                worker.generation,
                sequence,
                bytes_of(&dispatch, sizeof(dispatch)),
                io_timeout()))
    {
      replace_worker(worker);
      return false;
    }
    FrameHeader ack_header{};
    std::vector<std::uint8_t> ack_payload;
    if (!read_frame(worker.pipe.get(), ack_header, ack_payload, io_timeout()) ||
        ack_header.kind != FrameKind::socket_ack || ack_header.worker_index != worker.index ||
        ack_header.generation != worker.generation || ack_header.sequence != sequence ||
        ack_payload.size() != sizeof(SocketAckPayload))
    {
      replace_worker(worker);
      return false;
    }
    SocketAckPayload ack{};
    std::memcpy(&ack, ack_payload.data(), sizeof(ack));
    if (ack.accepted != 0)
    {
#ifdef VAJRA_TEST_FAULT_INJECTION
      successful_dispatches += 1;
#endif
      return true;
    }
    return false;
  }

  void run()
  {
    while (!stop_requested.load(std::memory_order_acquire))
    {
      if (VajraNative::shutdown_requested())
      {
        VajraNative::begin_runtime_shutdown();
        request_stop();
        break;
      }
      if (!pending_clients.empty())
      {
        Worker *worker = next_ready_worker();
        if (worker != nullptr)
        {
          const platform::SocketHandle client = pending_clients.front();
          pending_clients.pop_front();
          (void)dispatch_socket(*worker, client);
          platform::close_socket(client);
          continue;
        }
      }
      const std::size_t queue_capacity = std::max<std::size_t>(1, config.socket_queue_capacity);
      const bool queue_has_capacity = pending_clients.size() < queue_capacity;
      if (!queue_has_capacity || !platform::wait_socket(listener_binding.fd, platform::WaitEvent::read, 100))
      {
        for (auto &worker : workers)
        {
          if (worker->ready && WaitForSingleObject(worker->process.get(), 0) == WAIT_OBJECT_0)
          {
            replace_worker(*worker);
          }
          else if (!worker->ready && worker->replacement_pending &&
                   (!worker->state || !worker->state->terminal_replacement_failure.load(std::memory_order_acquire)))
          {
            replace_worker(*worker);
          }
        }
        if (!queue_has_capacity)
        {
          WaitForSingleObject(supervisor_stop_event.get(), 1);
        }
        continue;
      }
      sockaddr_storage address{};
      socklen_t address_length = sizeof(address);
      const platform::SocketHandle client = platform::accept_socket(
          listener_binding.fd,
          reinterpret_cast<sockaddr *>(&address),
          &address_length);
      if (!platform::socket_valid(client))
      {
        continue;
      }
      platform::set_socket_inheritable(client, false);
      pending_clients.push_back(client);
    }
    cleanup();
  }

  void request_stop()
  {
    stop_requested.store(true, std::memory_order_release);
    if (supervisor_stop_event)
    {
      SetEvent(supervisor_stop_event.get());
    }
  }

  void cleanup()
  {
    request_stop();
    std::call_once(cleanup_once, [this]()
                   {
                     mark_runtime_shutdown_requested();
                     if (platform::socket_valid(listener_binding.fd))
                     {
                       platform::shutdown_socket(listener_binding.fd);
                       platform::close_socket(listener_binding.fd);
                       listener_binding.fd = platform::kInvalidSocket;
                     }
                     while (!pending_clients.empty())
                     {
                       platform::close_socket(pending_clients.front());
                       pending_clients.pop_front();
                     }
                     for (auto &worker : workers)
                     {
                       worker->ready = false;
                       mark_worker_available(worker->index, false);
                       mark_worker_lifecycle(worker->index, WorkerLifecycleState::stopping);
                       (void)write_frame(
                           worker->pipe.get(),
                           FrameKind::shutdown,
                           worker->index,
                           worker->generation,
                           ++worker->sequence,
                           {},
                           io_timeout());
                     }
                     const auto deadline = std::chrono::steady_clock::now() +
                                           std::chrono::seconds(std::max(1, config.worker_timeout_seconds));
                     for (auto &worker : workers)
                     {
                       const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  deadline - std::chrono::steady_clock::now())
                                                  .count();
                       const DWORD wait_time = remaining <= 0 ? 0 : static_cast<DWORD>(remaining);
                       if (worker->process && WaitForSingleObject(worker->process.get(), wait_time) != WAIT_OBJECT_0)
                       {
                         TerminateProcess(worker->process.get(), 1);
                         WaitForSingleObject(worker->process.get(), 5000);
                         if (worker->state)
                         {
                           worker->state->timeout_escalation_count.fetch_add(1, std::memory_order_acq_rel);
                         }
                       }
                       mark_worker_lifecycle(worker->index, WorkerLifecycleState::exited);
                       worker->pipe.reset();
                       worker->process.reset();
                     }
                     if (parent_shutdown_event)
                     {
                       SetEvent(parent_shutdown_event.get());
                     } });
  }
};

bool Vajra::runtime::windows_worker_bootstrap_present()
{
  return !environment_value(kWorkerPipeEnvironment).empty();
}

void Vajra::runtime::run_windows_worker_process(const RuntimeConfig &invocation_config)
{
  (void)invocation_config;
  platform::ensure_socket_runtime();
  const std::wstring pipe = environment_value(kWorkerPipeEnvironment);
  const std::uint64_t mapping_value = unsigned_value(
      environment_value(kRuntimeMappingEnvironment),
      "runtime mapping handle");
  const std::uint64_t shutdown_value = unsigned_value(
      environment_value(kParentShutdownEnvironment),
      "parent shutdown handle");
  Handle control(CreateFileW(
      pipe.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      0,
      nullptr));
  if (!control)
  {
    throw windows_error("worker CreateFileW(named pipe)");
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (SetNamedPipeHandleState(control.get(), &mode, nullptr, nullptr) == 0)
  {
    throw windows_error("SetNamedPipeHandleState");
  }

  FrameHeader bootstrap_header{};
  std::vector<std::uint8_t> bootstrap_payload;
  if (!child_read_frame(control.get(), bootstrap_header, bootstrap_payload) ||
      bootstrap_header.kind != FrameKind::bootstrap)
  {
    throw std::runtime_error("invalid Windows worker bootstrap frame");
  }
  const RuntimeConfig config = deserialize_config(bootstrap_payload);
#ifdef VAJRA_TEST_FAULT_INJECTION
  const std::wstring test_fault = environment_value(kTestFaultEnvironment);
  if (test_fault == L"bootstrap_failure")
  {
    throw std::runtime_error("injected Windows worker bootstrap failure");
  }
  if (test_fault == L"readiness_timeout")
  {
    Sleep(static_cast<DWORD>(std::max(2, config.worker_timeout_seconds + 1)) * 1000);
  }
#endif
  const BootContractResult boot_result = BootContract::run(
      BootContractConfig{
          config.port,
          config.max_request_head_bytes,
          "ruby_worker_bootstrap"});
  BootContract::ensure_ready(boot_result);
  auto *runtime_state = attach_runtime_state(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(mapping_value)));
  struct RuntimeStateGuard
  {
    RuntimeState *state;
    ~RuntimeStateGuard() { release_runtime_state(state); }
  } state_guard{runtime_state};
  const std::size_t worker_index = bootstrap_header.worker_index;
  const std::uint64_t generation = bootstrap_header.generation;
  install_worker_runtime_state(runtime_state, worker_index, platform::current_process_id());
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
  start_runtime_logging_worker();
  start_runtime_tracing_worker();
  Vajra::rack::ensure_same_process_rack_execution_threads_started();
  auto server = build_dispatch_server(config, worker_index);
  server->start_dispatch_worker();
  mark_worker_lifecycle(worker_index, WorkerLifecycleState::ready);
  mark_worker_health(worker_index, WorkerHealthState::healthy);
  mark_worker_available(worker_index, true);
  if (!child_write_frame(control.get(), FrameKind::ready, worker_index, generation, 0, {}))
  {
    throw std::runtime_error("failed to acknowledge Windows worker readiness");
  }

  struct WorkerLoopContext
  {
    HANDLE control;
    HANDLE parent_shutdown;
    std::size_t worker_index;
    std::uint64_t generation;
    std::uint64_t last_dispatch_sequence;
    int worker_timeout_seconds;
#ifdef VAJRA_TEST_FAULT_INJECTION
    std::wstring test_fault;
#endif
    std::shared_ptr<Vajra::Server> server;
    std::exception_ptr failure;
  } loop_context{
      control.get(),
      reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(shutdown_value)),
      worker_index,
      generation,
      0,
      config.worker_timeout_seconds,
#ifdef VAJRA_TEST_FAULT_INJECTION
      test_fault,
#endif
      server,
      nullptr};
  rb_thread_call_without_gvl(
      [](void *value) -> void *
      {
        auto &context = *static_cast<WorkerLoopContext *>(value);
        try
        {
          for (;;)
          {
            if (WaitForSingleObject(context.parent_shutdown, 0) == WAIT_OBJECT_0)
            {
              break;
            }
            FrameHeader header{};
            std::vector<std::uint8_t> payload;
            if (!child_read_frame(context.control, header, payload))
            {
              break;
            }
            if (header.worker_index != context.worker_index || header.generation != context.generation)
            {
              break;
            }
            if (header.kind == FrameKind::shutdown)
            {
#ifdef VAJRA_TEST_FAULT_INJECTION
              if (context.test_fault == L"drain_timeout")
              {
                Sleep(static_cast<DWORD>(std::max(2, context.worker_timeout_seconds + 1)) * 1000);
              }
#endif
              break;
            }
            if (header.kind != FrameKind::socket_dispatch || payload.size() != sizeof(SocketDispatchPayload) ||
                header.sequence == 0 || header.sequence <= context.last_dispatch_sequence)
            {
              child_write_frame(context.control, FrameKind::failure, context.worker_index, context.generation, header.sequence, {});
              break;
            }
            context.last_dispatch_sequence = header.sequence;
#ifdef VAJRA_TEST_FAULT_INJECTION
            if (context.generation == 1 && context.test_fault == L"dispatch_timeout")
            {
              Sleep(static_cast<DWORD>(std::max(2, context.worker_timeout_seconds + 1)) * 1000);
            }
            if (context.generation == 1 &&
                (context.test_fault == L"worker_crash" || context.test_fault == L"replacement_exhaustion"))
            {
              ExitProcess(86);
            }
#endif
            SocketDispatchPayload dispatch{};
            std::memcpy(&dispatch, payload.data(), sizeof(dispatch));
            const platform::SocketHandle client = WSASocketW(
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                FROM_PROTOCOL_INFO,
                &dispatch.protocol_info,
                0,
                WSA_FLAG_OVERLAPPED);
            SocketAckPayload ack{};
            if (platform::socket_valid(client))
            {
              platform::set_socket_inheritable(client, false);
              ack.accepted = context.server->dispatch_client(client) ? 1 : 0;
              if (ack.accepted == 0)
              {
                ack.error_code = platform::socket_last_error();
              }
            }
            else
            {
              ack.error_code = platform::socket_last_error();
            }
            if (!child_write_frame(
                    context.control,
                    FrameKind::socket_ack,
                    context.worker_index,
                    context.generation,
                    header.sequence,
                    bytes_of(&ack, sizeof(ack))))
            {
              break;
            }
          }

          mark_worker_available(context.worker_index, false);
          mark_worker_lifecycle(context.worker_index, WorkerLifecycleState::stopping);
          mark_runtime_shutdown_requested();
          NativeRuntime::instance().begin_runtime_shutdown();
          context.server->finish_dispatch_worker();
          mark_worker_lifecycle(context.worker_index, WorkerLifecycleState::exited);
          child_write_frame(context.control, FrameKind::stopped, context.worker_index, context.generation, 0, {});
        }
        catch (...)
        {
          context.failure = std::current_exception();
        }
        return nullptr;
      },
      &loop_context,
      nullptr,
      nullptr);
  if (loop_context.failure)
  {
    std::rethrow_exception(loop_context.failure);
  }
  stop_runtime_tracing_worker();
  stop_runtime_logging_worker();
}

Vajra::runtime::WindowsWorkerSupervisor::WindowsWorkerSupervisor(RuntimeConfig config, RuntimeState *runtime_state)
    : implementation_(std::make_unique<Implementation>(std::move(config), runtime_state))
{
}

Vajra::runtime::WindowsWorkerSupervisor::~WindowsWorkerSupervisor()
{
  implementation_->cleanup();
}

void Vajra::runtime::WindowsWorkerSupervisor::start()
{
  implementation_->start();
}

void Vajra::runtime::WindowsWorkerSupervisor::run()
{
  implementation_->run();
}

void Vajra::runtime::WindowsWorkerSupervisor::request_stop()
{
  implementation_->request_stop();
}

std::vector<std::shared_ptr<Vajra::runtime::SharedWorkerState>>
Vajra::runtime::WindowsWorkerSupervisor::worker_states() const
{
  const std::lock_guard<std::mutex> lock(implementation_->mutex);
  return implementation_->public_states;
}

#endif
