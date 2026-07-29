// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "platform/process.hpp"

#ifdef _WIN32
#include <tlhelp32.h>
#endif

Vajra::platform::ProcessId Vajra::platform::current_process_id()
{
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return getpid();
#endif
}

Vajra::platform::ProcessId Vajra::platform::current_parent_process_id()
{
#ifdef _WIN32
  const DWORD current_id = GetCurrentProcessId();
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
  {
    return kInvalidProcessId;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  ProcessId parent_id = kInvalidProcessId;
  if (Process32FirstW(snapshot, &entry) != FALSE)
  {
    do
    {
      if (entry.th32ProcessID == current_id)
      {
        parent_id = entry.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry) != FALSE);
  }
  CloseHandle(snapshot);
  return parent_id;
#else
  return getppid();
#endif
}

std::uint64_t Vajra::platform::process_id_value(ProcessId process_id)
{
  return static_cast<std::uint64_t>(process_id);
}
