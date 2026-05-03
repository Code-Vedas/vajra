// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_CONTRACT_HPP
#define VAJRA_IPC_FRAME_CONTRACT_HPP

#include "channel_kind.hpp"
#include "frame_family.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace Vajra
{
  namespace ipc
  {
    constexpr std::array<FrameFamily, 11> kFrameFamilies = {
        FrameFamily::request_execution_input,
        FrameFamily::request_body_continuation,
        FrameFamily::response_metadata_result,
        FrameFamily::response_body_continuation,
        FrameFamily::protocol_version_negotiation,
        FrameFamily::process_registration_identity,
        FrameFamily::readiness_boot_result,
        FrameFamily::lifecycle_command,
        FrameFamily::lifecycle_state_notification,
        FrameFamily::diagnostics_error_reporting,
        FrameFamily::telemetry_status_reserved,
    };

    ChannelKind owning_channel(FrameFamily family);
    bool valid_on_channel(FrameFamily family, ChannelKind channel);
    bool reserved_family(FrameFamily family);
    std::uint16_t wire_id(FrameFamily family);
    std::optional<FrameFamily> decode_frame_family(std::uint16_t wire_id);
  }
}

#endif
