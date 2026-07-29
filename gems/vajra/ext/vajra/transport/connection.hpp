// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_TRANSPORT_CONNECTION_HPP
#define VAJRA_TRANSPORT_CONNECTION_HPP

#include "platform/socket.hpp"

#include <cstddef>
#include <string>

namespace Vajra
{
  namespace transport
  {
    class Connection
    {
    public:
      virtual ~Connection() = default;

      virtual platform::SocketHandle fd() const = 0;
      virtual bool wait_readable(int timeout_seconds) = 0;
      virtual platform::SignedSize read(char *buffer, std::size_t length) = 0;
      virtual platform::SignedSize write(const char *buffer, std::size_t length) = 0;
      virtual std::string protocol() const = 0;
      virtual bool tls() const = 0;
    };

    class PlainConnection final : public Connection
    {
    public:
      explicit PlainConnection(platform::SocketHandle client_fd);

      platform::SocketHandle fd() const override;
      bool wait_readable(int timeout_seconds) override;
      platform::SignedSize read(char *buffer, std::size_t length) override;
      platform::SignedSize write(const char *buffer, std::size_t length) override;
      std::string protocol() const override;
      bool tls() const override;

    private:
      platform::SocketHandle client_fd_;
    };
  }
}

#endif
