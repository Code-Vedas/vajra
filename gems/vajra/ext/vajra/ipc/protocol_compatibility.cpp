// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "protocol_compatibility.hpp"

namespace Vajra
{
  namespace ipc
  {
    bool supported_protocol_version(ProtocolVersion version)
    {
      return version == kProtocolVersion1_0;
    }

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote)
    {
      if (!supported_protocol_version(local))
      {
        return CompatibilityResult::unsupported_local_version;
      }

      if (!supported_protocol_version(remote))
      {
        if (local.major != remote.major)
        {
          return CompatibilityResult::incompatible_major;
        }

        if (remote.minor < local.minor)
        {
          return CompatibilityResult::remote_older_minor;
        }

        return CompatibilityResult::remote_newer_minor;
      }

      if (local.major != remote.major)
      {
        return CompatibilityResult::incompatible_major;
      }

      if (remote.minor < local.minor)
      {
        return CompatibilityResult::remote_older_minor;
      }

      if (remote.minor > local.minor)
      {
        return CompatibilityResult::remote_newer_minor;
      }

      return CompatibilityResult::compatible;
    }

    bool frame_family_available(FrameFamily family, ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return false;
      }

      if (!supported_protocol_version(version))
      {
        return false;
      }

      switch (family)
      {
#define VAJRA_IPC_FRAME_AVAILABILITY_CASE(name, wire_id, channel, available_in_v1_0) \
      case FrameFamily::name:                                                          \
        return available_in_v1_0;
        VAJRA_IPC_FRAME_FAMILY_REGISTRY(VAJRA_IPC_FRAME_AVAILABILITY_CASE)
#undef VAJRA_IPC_FRAME_AVAILABILITY_CASE
      }

      return false;
    }
  }
}
