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
    namespace
    {
      const FrameFamilyMetadata *find_frame_family_metadata(FrameFamily family)
      {
        for (const FrameFamilyMetadata &metadata : kFrameFamilyMetadata)
        {
          if (metadata.family == family)
          {
            return &metadata;
          }
        }

        return nullptr;
      }
    }

    bool known_frame_family(FrameFamily family)
    {
      return find_frame_family_metadata(family) != nullptr;
    }

    std::optional<ProtocolVersion> first_supported_protocol_version(FrameFamily family)
    {
      if (!known_frame_family(family))
      {
        throw std::invalid_argument("unknown ipc frame family");
      }

      ProtocolVersion first_supported = {
          kNeverSupportedProtocolMajor,
          kNeverSupportedProtocolMinor,
      };

      for (const FrameFamilyVersionSupport &support : kFrameFamilyVersionSupport)
      {
        if (support.family != family)
        {
          continue;
        }

        if (first_supported.major == kNeverSupportedProtocolMajor ||
            support.version.major < first_supported.major ||
            (support.version.major == first_supported.major && support.version.minor < first_supported.minor))
        {
          first_supported = support.version;
        }
      }

      if (first_supported.major == kNeverSupportedProtocolMajor &&
          first_supported.minor == kNeverSupportedProtocolMinor)
      {
        return std::nullopt;
      }

      return first_supported;
    }

    ChannelKind owning_channel(FrameFamily family)
    {
      if (const FrameFamilyMetadata *metadata = find_frame_family_metadata(family))
      {
        return metadata->channel;
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
      if (const FrameFamilyMetadata *metadata = find_frame_family_metadata(family))
      {
        return metadata->reserved;
      }

      return false;
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
