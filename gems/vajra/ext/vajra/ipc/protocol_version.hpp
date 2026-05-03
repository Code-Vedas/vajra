// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_PROTOCOL_VERSION_HPP
#define VAJRA_IPC_PROTOCOL_VERSION_HPP

#include <cstdint>

namespace Vajra
{
  namespace ipc
  {
    struct ProtocolVersion
    {
      std::uint16_t major;
      std::uint16_t minor;
    };

    constexpr ProtocolVersion kProtocolVersion1_0{1, 0};

    constexpr bool operator==(ProtocolVersion left, ProtocolVersion right)
    {
      return left.major == right.major && left.minor == right.minor;
    }

    constexpr bool operator!=(ProtocolVersion left, ProtocolVersion right)
    {
      return !(left == right);
    }
  }
}

#endif
