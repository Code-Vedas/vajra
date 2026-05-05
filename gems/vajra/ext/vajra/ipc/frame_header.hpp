// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_FRAME_HEADER_HPP
#define VAJRA_IPC_FRAME_HEADER_HPP

#include "channel_kind.hpp"
#include "frame_contract.hpp"
#include "protocol_version.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Vajra
{
  namespace ipc
  {
    struct FrameHeader
    {
      ChannelKind channel;
      FrameFamily family;
      ProtocolVersion version;
      std::uint32_t payload_length;
    };

    enum class HeaderDecodeError
    {
      none,
      reserved_bits_set,
      unknown_channel_kind,
      unknown_frame_family,
      channel_family_mismatch,
      unsupported_protocol_version,
      reserved_frame_family,
      unavailable_frame_family,
    };

    constexpr std::size_t kFrameHeaderSize = 12;

    std::array<std::uint8_t, kFrameHeaderSize> encode_frame_header(const FrameHeader &header);
    std::optional<FrameHeader> decode_frame_header(
        const std::array<std::uint8_t, kFrameHeaderSize> &encoded_header,
        HeaderDecodeError &error);
  }
}

#endif
