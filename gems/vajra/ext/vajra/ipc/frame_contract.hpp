// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_CONTRACT_HPP
#define VAJRA_IPC_FRAME_CONTRACT_HPP

#include "channel_kind.hpp"
#include "frame_family.hpp"
#include "protocol_version.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace Vajra
{
  namespace ipc
  {
    bool known_frame_family(FrameFamily family);
    ProtocolVersion first_supported_protocol_version(FrameFamily family);
    ChannelKind owning_channel(FrameFamily family);
    bool valid_on_channel(FrameFamily family, ChannelKind channel);
    bool reserved_family(FrameFamily family);
    std::uint16_t wire_id(FrameFamily family);
    std::optional<FrameFamily> decode_frame_family(std::uint16_t wire_id);
  }
}

#endif
