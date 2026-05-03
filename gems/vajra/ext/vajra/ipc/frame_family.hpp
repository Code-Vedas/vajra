// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_FAMILY_HPP
#define VAJRA_IPC_FRAME_FAMILY_HPP

#include <cstdint>

namespace Vajra
{
  namespace ipc
  {
    enum class FrameFamily : std::uint16_t
    {
      request_execution_input = 0x1001,
      request_body_continuation = 0x1002,
      response_metadata_result = 0x1003,
      response_body_continuation = 0x1004,
      protocol_version_negotiation = 0x2001,
      process_registration_identity = 0x2002,
      readiness_boot_result = 0x2003,
      lifecycle_command = 0x2004,
      lifecycle_state_notification = 0x2005,
      diagnostics_error_reporting = 0x2006,
      telemetry_status_reserved = 0x2007,
    };
  }
}

#endif
