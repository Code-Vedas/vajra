// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_FAMILY_HPP
#define VAJRA_IPC_FRAME_FAMILY_HPP

#include "channel_kind.hpp"

#include <array>
#include <cstdint>

namespace Vajra
{
  namespace ipc
  {
    #define VAJRA_IPC_FRAME_FAMILY_REGISTRY(X) \
      X(request_execution_input, 0x1001, ChannelKind::request, true) \
      X(request_body_continuation, 0x1002, ChannelKind::request, true) \
      X(response_metadata_result, 0x1003, ChannelKind::request, true) \
      X(response_body_continuation, 0x1004, ChannelKind::request, true) \
      X(protocol_version_negotiation, 0x2001, ChannelKind::control, true) \
      X(process_registration_identity, 0x2002, ChannelKind::control, true) \
      X(readiness_boot_result, 0x2003, ChannelKind::control, true) \
      X(lifecycle_command, 0x2004, ChannelKind::control, true) \
      X(lifecycle_state_notification, 0x2005, ChannelKind::control, true) \
      X(diagnostics_error_reporting, 0x2006, ChannelKind::control, true) \
      X(telemetry_status_reserved, 0x2007, ChannelKind::control, false)

    enum class FrameFamily : std::uint16_t
    {
#define VAJRA_IPC_DEFINE_FRAME_FAMILY(name, wire_id, channel, available_in_v1_0) name = wire_id,
      VAJRA_IPC_FRAME_FAMILY_REGISTRY(VAJRA_IPC_DEFINE_FRAME_FAMILY)
#undef VAJRA_IPC_DEFINE_FRAME_FAMILY
    };

    constexpr std::array<FrameFamily, 11> kFrameFamilies = {
#define VAJRA_IPC_FRAME_FAMILY_VALUE(name, wire_id, channel, available_in_v1_0) FrameFamily::name,
        VAJRA_IPC_FRAME_FAMILY_REGISTRY(VAJRA_IPC_FRAME_FAMILY_VALUE)
#undef VAJRA_IPC_FRAME_FAMILY_VALUE
    };
  }
}

#endif
