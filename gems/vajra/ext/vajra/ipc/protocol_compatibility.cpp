// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "protocol_compatibility.hpp"

namespace Vajra
{
  namespace ipc
  {
    bool supported_protocol_version(ProtocolVersion version)
    {
      return version == kProtocolVersion1_0;
    }

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote)
    {
      if (local.major != remote.major)
      {
        return CompatibilityResult::incompatible_major;
      }

      if (local.minor != remote.minor)
      {
        return CompatibilityResult::unsupported_minor;
      }

      return CompatibilityResult::compatible;
    }

    bool frame_family_available(FrameFamily family, ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return false;
      }

      if (!supported_protocol_version(version))
      {
        return false;
      }

      switch (family)
      {
      case FrameFamily::request_execution_input:
      case FrameFamily::request_body_continuation:
      case FrameFamily::response_metadata_result:
      case FrameFamily::response_body_continuation:
      case FrameFamily::protocol_version_negotiation:
      case FrameFamily::process_registration_identity:
      case FrameFamily::readiness_boot_result:
      case FrameFamily::lifecycle_command:
      case FrameFamily::lifecycle_state_notification:
      case FrameFamily::diagnostics_error_reporting:
        return true;
      case FrameFamily::telemetry_status_reserved:
        return false;
      }

      return false;
    }
  }
}
