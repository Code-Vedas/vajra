// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_PROTOCOL_CONTRACT_HPP
#define VAJRA_IPC_PROTOCOL_CONTRACT_HPP

#include "channel_kind.hpp"
#include "frame_family.hpp"
#include "protocol_version.hpp"

#include <cstdint>
#include <optional>

namespace Vajra
{
  namespace ipc
  {
    enum class CompatibilityResult
    {
      compatible,
      unsupported_local_version,
      unsupported_remote_version,
      incompatible_major,
      remote_newer_minor,
    };

    enum class FrameValidationError
    {
      none,
      unknown_frame_family,
      channel_family_mismatch,
      reserved_frame_family,
      unsupported_protocol_version,
      unavailable_frame_family,
    };

    bool known_frame_family(FrameFamily family);
    std::optional<ProtocolVersion> first_supported_protocol_version(FrameFamily family);
    ChannelKind owning_channel(FrameFamily family);
    bool valid_on_channel(FrameFamily family, ChannelKind channel);
    bool reserved_family(FrameFamily family);
    std::uint16_t wire_id(FrameFamily family);
    std::optional<FrameFamily> decode_frame_family(std::uint16_t wire_id);

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote);
    bool supported_protocol_version(ProtocolVersion version);
    bool frame_family_active_for_protocol_version(FrameFamily family, ProtocolVersion version);

    FrameValidationError validate_outbound_frame(
        ChannelKind channel,
        FrameFamily family,
        ProtocolVersion version);
    FrameValidationError validate_inbound_frame(
        ChannelKind channel,
        FrameFamily family,
        ProtocolVersion version);
  }
}

#endif
