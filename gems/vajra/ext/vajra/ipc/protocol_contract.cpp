// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "protocol_contract.hpp"

#include <array>
#include <stdexcept>

namespace Vajra
{
  namespace ipc
  {
    namespace
    {
      struct CompatibilityEntry
      {
        ProtocolVersion local;
        ProtocolVersion remote;
      };

      constexpr std::array<ProtocolVersion, 1> kSupportedProtocolVersions = {{
          kProtocolVersion1_0,
      }};

      constexpr std::array<CompatibilityEntry, 0> kExplicitCrossVersionCompatibility = {};

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

      bool supported_protocol_version_entry(ProtocolVersion version)
      {
        for (const ProtocolVersion supported_version : kSupportedProtocolVersions)
        {
          if (supported_version == version)
          {
            return true;
          }
        }

        return false;
      }

      bool explicitly_compatible_protocol_pair(ProtocolVersion local, ProtocolVersion remote)
      {
        for (const CompatibilityEntry &entry : kExplicitCrossVersionCompatibility)
        {
          if (entry.local == local && entry.remote == remote)
          {
            return true;
          }
        }

        return false;
      }

      bool version_has_active_frame_family(ProtocolVersion version)
      {
        for (const FrameFamilyVersionSupport &support : kFrameFamilyVersionSupport)
        {
          if (support.version == version)
          {
            return true;
          }
        }

        return false;
      }

      bool negotiation_frame_family(FrameFamily family)
      {
        return family == FrameFamily::protocol_version_negotiation;
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

      std::optional<ProtocolVersion> first_supported;
      for (const FrameFamilyVersionSupport &support : kFrameFamilyVersionSupport)
      {
        if (support.family != family)
        {
          continue;
        }

        if (!first_supported.has_value() || support.version.major < first_supported->major ||
            (support.version.major == first_supported->major &&
             support.version.minor < first_supported->minor))
        {
          first_supported = support.version;
        }
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
      if (!known_frame_family(family))
      {
        throw std::invalid_argument("unknown ipc frame family");
      }

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

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote)
    {
      if (!supported_protocol_version(local))
      {
        return CompatibilityResult::unsupported_local_version;
      }

      if (local == remote)
      {
        return CompatibilityResult::compatible;
      }

      if (explicitly_compatible_protocol_pair(local, remote))
      {
        return CompatibilityResult::compatible;
      }

      if (local.major != remote.major)
      {
        return CompatibilityResult::incompatible_major;
      }

      if (remote.minor > local.minor)
      {
        return CompatibilityResult::remote_newer_minor;
      }

      return CompatibilityResult::unsupported_remote_version;
    }

    bool supported_protocol_version(ProtocolVersion version)
    {
      return supported_protocol_version_entry(version) &&
             version_has_active_frame_family(version);
    }

    bool frame_family_active_for_protocol_version(FrameFamily family, ProtocolVersion version)
    {
      if (!known_frame_family(family) || reserved_family(family) || !supported_protocol_version(version))
      {
        return false;
      }

      for (const FrameFamilyVersionSupport &support : kFrameFamilyVersionSupport)
      {
        if (support.family == family && support.version == version)
        {
          return true;
        }
      }

      return false;
    }

    FrameValidationError validate_outbound_frame(
        ChannelKind channel,
        FrameFamily family,
        ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return FrameValidationError::unknown_frame_family;
      }

      if (!valid_on_channel(family, channel))
      {
        return FrameValidationError::channel_family_mismatch;
      }

      if (reserved_family(family))
      {
        return FrameValidationError::reserved_frame_family;
      }

      if (!supported_protocol_version(version))
      {
        return FrameValidationError::unsupported_protocol_version;
      }

      if (negotiation_frame_family(family))
      {
        return FrameValidationError::none;
      }

      if (!frame_family_active_for_protocol_version(family, version))
      {
        return FrameValidationError::unavailable_frame_family;
      }

      return FrameValidationError::none;
    }

    FrameValidationError validate_inbound_frame(
        ChannelKind channel,
        FrameFamily family,
        ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return FrameValidationError::unknown_frame_family;
      }

      if (!valid_on_channel(family, channel))
      {
        return FrameValidationError::channel_family_mismatch;
      }

      if (negotiation_frame_family(family))
      {
        return FrameValidationError::none;
      }

      if (reserved_family(family))
      {
        return FrameValidationError::reserved_frame_family;
      }

      if (!supported_protocol_version(version))
      {
        return FrameValidationError::unsupported_protocol_version;
      }

      if (!frame_family_active_for_protocol_version(family, version))
      {
        return FrameValidationError::unavailable_frame_family;
      }

      return FrameValidationError::none;
    }
  }
}
