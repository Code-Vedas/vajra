// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "protocol_compatibility.hpp"

#include <array>

namespace Vajra
{
  namespace ipc
  {
    namespace
    {
      constexpr std::array<ProtocolVersion, 1> kSupportedProtocolVersions = {{
          kProtocolVersion1_0,
      }};

      struct CompatibilityEntry
      {
        ProtocolVersion local;
        ProtocolVersion remote;
      };

      constexpr std::array<CompatibilityEntry, 0> kExplicitCrossVersionCompatibility = {};

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
    }

    bool supported_protocol_version(ProtocolVersion version)
    {
      return supported_protocol_version_entry(version) &&
             version_has_active_frame_family(version);
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

      if (!supported_protocol_version(remote))
      {
        return CompatibilityResult::unsupported_remote_version;
      }

      return CompatibilityResult::unsupported_remote_version;
    }

    bool frame_family_active_for_protocol_version(FrameFamily family, ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return false;
      }

      if (!supported_protocol_version(version))
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
  }
}
