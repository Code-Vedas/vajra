// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_PLATFORM_PROCESS_HPP
#define VAJRA_PLATFORM_PROCESS_HPP

#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Vajra::platform
{
#ifdef _WIN32
  using ProcessId = DWORD;
  using NativeProcessHandle = HANDLE;
  constexpr ProcessId kInvalidProcessId = 0;
  constexpr NativeProcessHandle kInvalidProcessHandle = nullptr;
#else
  using ProcessId = pid_t;
  using NativeProcessHandle = pid_t;
  constexpr ProcessId kInvalidProcessId = -1;
  constexpr NativeProcessHandle kInvalidProcessHandle = -1;
#endif

  ProcessId current_process_id();
  ProcessId current_parent_process_id();
  std::uint64_t process_id_value(ProcessId process_id);
}

#endif
