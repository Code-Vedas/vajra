// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "protocol_compatibility.hpp"

namespace Vajra
{
  namespace ipc
  {
    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote)
    {
      if (local.major != remote.major)
      {
        return CompatibilityResult::incompatible_major;
      }

      if (local.minor != remote.minor)
      {
        return CompatibilityResult::unsupported_minor;
      }

      return CompatibilityResult::compatible;
    }

    bool frame_family_available(FrameFamily family, ProtocolVersion version)
    {
      if (!known_frame_family(family))
      {
        return false;
      }

      if (check_compatibility(kProtocolVersion1_0, version) != CompatibilityResult::compatible)
      {
        return false;
      }

      return !reserved_family(family);
    }
  }
}
