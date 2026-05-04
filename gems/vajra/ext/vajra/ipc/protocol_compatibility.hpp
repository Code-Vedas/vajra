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
      unsupported_local_version,
      unsupported_remote_version,
      incompatible_major,
      remote_newer_minor,
    };

    CompatibilityResult check_compatibility(ProtocolVersion local, ProtocolVersion remote);
    bool supported_protocol_version(ProtocolVersion version);
    // This models post-negotiation activation. Negotiation frames themselves
    // are allowed to carry unsupported advertised versions structurally.
    bool frame_family_active_for_protocol_version(FrameFamily family, ProtocolVersion version);
  }
}

#endif
