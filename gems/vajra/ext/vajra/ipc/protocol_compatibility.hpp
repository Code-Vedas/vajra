// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_PROTOCOL_COMPATIBILITY_HPP
#define VAJRA_IPC_PROTOCOL_COMPATIBILITY_HPP

#include "frame_contract.hpp"
#include "protocol_version.hpp"

namespace Vajra
{
  namespace ipc
  {
    enum class CompatibilityResult
    {
      compatible,
      incompatible_major,
      unsupported_minor,
    };

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote);
    bool supported_protocol_version(ProtocolVersion version);
    bool frame_family_available(FrameFamily family, ProtocolVersion version);
  }
}

#endif
