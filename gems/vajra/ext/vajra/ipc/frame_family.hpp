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
      X(request_execution_input, 0x1001, ChannelKind::request, false) \
      X(request_body_continuation, 0x1002, ChannelKind::request, false) \
      X(response_metadata_result, 0x1003, ChannelKind::request, false) \
      X(response_body_continuation, 0x1004, ChannelKind::request, false) \
      X(protocol_version_negotiation, 0x2001, ChannelKind::control, false) \
      X(process_registration_identity, 0x2002, ChannelKind::control, false) \
      X(readiness_boot_result, 0x2003, ChannelKind::control, false) \
      X(lifecycle_command, 0x2004, ChannelKind::control, false) \
      X(lifecycle_state_notification, 0x2005, ChannelKind::control, false) \
      X(diagnostics_error_reporting, 0x2006, ChannelKind::control, false) \
      X(telemetry_status_reserved, 0x2007, ChannelKind::control, true)

    #define VAJRA_IPC_DEFINE_ACTIVE_FRAME_FAMILY_VERSIONS(X) \
      X(request_execution_input, 1, 0) \
      X(request_body_continuation, 1, 0) \
      X(response_metadata_result, 1, 0) \
      X(response_body_continuation, 1, 0) \
      X(protocol_version_negotiation, 1, 0) \
      X(process_registration_identity, 1, 0) \
      X(readiness_boot_result, 1, 0) \
      X(lifecycle_command, 1, 0) \
      X(lifecycle_state_notification, 1, 0) \
      X(diagnostics_error_reporting, 1, 0)

    enum class FrameFamily : std::uint16_t
    {
#define VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM(name, wire_id, channel, reserved) name = wire_id,
      VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM)
#undef VAJRA_IPC_DEFINE_FRAME_FAMILY_ENUM
    };

    struct FrameFamilyMetadata
    {
      FrameFamily family;
      ChannelKind channel;
      bool reserved;
    };

    struct FrameFamilyVersionSupport
    {
      FrameFamily family;
      ProtocolVersion version;
    };

    constexpr std::size_t kFrameFamilyCount =
        0
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_COUNT(name, wire_id, channel, reserved) + 1
        VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_COUNT)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_COUNT
        ;

    constexpr std::size_t kFrameFamilyVersionSupportCount =
        0
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT_COUNT(name, major, minor) + 1
        VAJRA_IPC_DEFINE_ACTIVE_FRAME_FAMILY_VERSIONS(VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT_COUNT)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT_COUNT
        ;

    constexpr std::array<FrameFamily, kFrameFamilyCount> kFrameFamilies = {
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE(name, wire_id, channel, reserved) FrameFamily::name,
        VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_VALUE
    };

    constexpr std::array<FrameFamilyMetadata, kFrameFamilyCount> kFrameFamilyMetadata = {
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA(name, wire_id, channel, reserved) FrameFamilyMetadata{FrameFamily::name, channel, reserved},
        VAJRA_IPC_DEFINE_FRAME_FAMILIES(VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_METADATA
    };

    constexpr std::array<FrameFamilyVersionSupport, kFrameFamilyVersionSupportCount> kFrameFamilyVersionSupport = {
        #define VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT(name, major, minor) FrameFamilyVersionSupport{FrameFamily::name, ProtocolVersion{major, minor}},
        VAJRA_IPC_DEFINE_ACTIVE_FRAME_FAMILY_VERSIONS(VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT)
        #undef VAJRA_IPC_DEFINE_FRAME_FAMILY_VERSION_SUPPORT
    };

#undef VAJRA_IPC_DEFINE_FRAME_FAMILIES
#undef VAJRA_IPC_DEFINE_ACTIVE_FRAME_FAMILY_VERSIONS
  }
}

#endif
