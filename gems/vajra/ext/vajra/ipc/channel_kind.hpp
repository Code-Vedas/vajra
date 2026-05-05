// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_IPC_CHANNEL_KIND_HPP
#define VAJRA_IPC_CHANNEL_KIND_HPP

#include <cstdint>

namespace Vajra
{
  namespace ipc
  {
    enum class ChannelKind : std::uint8_t
    {
      request = 1,
      control = 2,
    };
  }
}

#endif
