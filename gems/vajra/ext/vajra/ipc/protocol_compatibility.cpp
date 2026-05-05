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
      struct CompatibilityEntry
      {
        ProtocolVersion local;
        ProtocolVersion remote;
      };

      constexpr std::array<CompatibilityEntry, 1> kCompatibleProtocolPairs = {{
          {kProtocolVersion1_0, kProtocolVersion1_0},
      }};

      bool explicitly_compatible_protocol_pair(ProtocolVersion local, ProtocolVersion remote)
      {
        for (const CompatibilityEntry &entry : kCompatibleProtocolPairs)
        {
          if (entry.local == local && entry.remote == remote)
          {
            return true;
          }
        }

        return false;
      }

      bool protocol_version_present_in_contract(ProtocolVersion version)
      {
        for (const CompatibilityEntry &entry : kCompatibleProtocolPairs)
        {
          if (entry.local == version || entry.remote == version)
          {
            return true;
          }
        }

        return false;
      }
    }

    bool supported_protocol_version(ProtocolVersion version)
    {
      return protocol_version_present_in_contract(version);
    }

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote)
    {
      if (!supported_protocol_version(local))
      {
        return CompatibilityResult::unsupported_local_version;
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

      const ProtocolVersion activation_version = first_supported_protocol_version(family);
      if (activation_version.major == kNeverSupportedProtocolMajor &&
          activation_version.minor == kNeverSupportedProtocolMinor)
      {
        return false;
      }

      if (version.major != activation_version.major)
      {
        return false;
      }

      return version.minor >= activation_version.minor;
    }
  }
}
