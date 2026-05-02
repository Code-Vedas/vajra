// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_LISTENER_SOCKET_HPP
#define VAJRA_LISTENER_SOCKET_HPP

namespace Vajra
{
  namespace listener
  {
    struct SocketBinding
    {
      int fd;
      int port;
    };

    class Socket
    {
    public:
      SocketBinding open(int port) const;
    };
  }
}

#endif
