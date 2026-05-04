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

    ProtocolVersion first_supported_protocol_version(FrameFamily family)
    {
      switch (family)
      {
#define VAJRA_IPC_FRAME_PROTOCOL_CASE(name, wire_id, channel, first_supported_major, first_supported_minor) \
      case FrameFamily::name:                                                                                \
        return ProtocolVersion{first_supported_major, first_supported_minor};
        VAJRA_IPC_FRAME_FAMILY_REGISTRY(VAJRA_IPC_FRAME_PROTOCOL_CASE)
#undef VAJRA_IPC_FRAME_PROTOCOL_CASE
      }

      throw std::invalid_argument("unknown ipc frame family");
    }

    ChannelKind owning_channel(FrameFamily family)
    {
      switch (family)
      {
#define VAJRA_IPC_FRAME_CHANNEL_CASE(name, wire_id, channel, first_supported_major, first_supported_minor) \
      case FrameFamily::name:                                                                                \
        return channel;
        VAJRA_IPC_FRAME_FAMILY_REGISTRY(VAJRA_IPC_FRAME_CHANNEL_CASE)
#undef VAJRA_IPC_FRAME_CHANNEL_CASE
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
      if (!known_frame_family(family))
      {
        return false;
      }

      const ProtocolVersion activation_version = first_supported_protocol_version(family);
      return activation_version.major == kNeverSupportedProtocolMajor &&
             activation_version.minor == kNeverSupportedProtocolMinor;
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
