// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "frame_contract.hpp"

#include <stdexcept>

namespace Vajra
{
  namespace ipc
  {
    bool known_frame_family(FrameFamily family)
    {
      for (const FrameFamily known_family : kFrameFamilies)
      {
        if (known_family == family)
        {
          return true;
        }
      }

      return false;
    }

    ChannelKind owning_channel(FrameFamily family)
    {
      switch (family)
      {
      case FrameFamily::request_execution_input:
      case FrameFamily::request_body_continuation:
      case FrameFamily::response_metadata_result:
      case FrameFamily::response_body_continuation:
        return ChannelKind::request;
      case FrameFamily::protocol_version_negotiation:
      case FrameFamily::process_registration_identity:
      case FrameFamily::readiness_boot_result:
      case FrameFamily::lifecycle_command:
      case FrameFamily::lifecycle_state_notification:
      case FrameFamily::diagnostics_error_reporting:
      case FrameFamily::telemetry_status_reserved:
        return ChannelKind::control;
      }

      throw std::invalid_argument("unknown ipc frame family");
    }

    bool valid_on_channel(FrameFamily family, ChannelKind channel)
    {
      if (!known_frame_family(family))
      {
        return false;
      }

      return owning_channel(family) == channel;
    }

    bool reserved_family(FrameFamily family)
    {
      return family == FrameFamily::telemetry_status_reserved;
    }

    std::uint16_t wire_id(FrameFamily family)
    {
      return static_cast<std::uint16_t>(family);
    }

    std::optional<FrameFamily> decode_frame_family(std::uint16_t encoded_wire_id)
    {
      for (const FrameFamily family : kFrameFamilies)
      {
        if (wire_id(family) == encoded_wire_id)
        {
          return family;
        }
      }

      return std::nullopt;
    }
  }
}
