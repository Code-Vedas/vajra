// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_FAMILY_HPP
#define VAJRA_IPC_FRAME_FAMILY_HPP

#include "channel_kind.hpp"
#include "protocol_version.hpp"

#include <array>
#include <cstdint>

namespace Vajra
{
  namespace ipc
  {
    constexpr std::uint16_t kNeverSupportedProtocolMajor = 0xFFFF;
    constexpr std::uint16_t kNeverSupportedProtocolMinor = 0xFFFF;

    #define VAJRA_IPC_DEFINE_FRAME_FAMILIES(X) \
      X(request_execution_input, 0x1001, ChannelKind::request, 1, 0) \
      X(request_body_continuation, 0x1002, ChannelKind::request, 1, 0) \
      X(response_metadata_result, 0x1003, ChannelKind::request, 1, 0) \
      X(response_body_continuation, 0x1004, ChannelKind::request, 1, 0) \
      X(protocol_version_negotiation, 0x2001, ChannelKind::control, 1, 0) \
      X(process_registration_identity, 0x2002, ChannelKind::control, 1, 0) \
      X(readiness_boot_result, 0x2003, ChannelKind::control, 1, 0) \
      X(lifecycle_command, 0x2004, ChannelKind::control, 1, 0) \
      X(lifecycle_state_notification, 0x2005, ChannelKind::control, 1, 0) \
      X(diagnostics_error_reporting, 0x2006, ChannelKind::control, 1, 0) \
      X(telemetry_status_reserved, 0x2007, ChannelKind::control, kNeverSupportedProtocolMajor, kNeverSupportedProtocolMinor)

    enum class FrameFamily : std::uint16_t
    {
#define VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM(name, wire_id, channel, first_supported_major, first_supported_minor) name = wire_id,
      VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM)
#undef VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM
    };

    struct FrameFamilyMetadata
    {
      FrameFamily family;
      ChannelKind channel;
      ProtocolVersion first_supported_version;
    };

    constexpr std::array<FrameFamily, 11> kFrameFamilies = {
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE(name, wire_id, channel, first_supported_major, first_supported_minor) FrameFamily::name,
        VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE
    };

    constexpr std::array<FrameFamilyMetadata, 11> kFrameFamilyMetadata = {
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA(name, wire_id, channel, first_supported_major, first_supported_minor) FrameFamilyMetadata{FrameFamily::name, channel, ProtocolVersion{first_supported_major, first_supported_minor}},
        VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA
    };

#undef VAJRA_IPC_DEFINE_FRAME_FAMILIES
  }
}

#endif
