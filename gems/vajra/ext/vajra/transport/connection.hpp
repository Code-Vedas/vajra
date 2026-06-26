// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_TRANSPORT_CONNECTION_HPP
#define VAJRA_TRANSPORT_CONNECTION_HPP

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

      virtual int fd() const = 0;
      virtual bool wait_readable(int timeout_seconds) = 0;
      virtual ssize_t read(char *buffer, std::size_t length) = 0;
      virtual ssize_t write(const char *buffer, std::size_t length) = 0;
      virtual std::string protocol() const = 0;
      virtual bool tls() const = 0;
    };

    class PlainConnection final : public Connection
    {
    public:
      explicit PlainConnection(int client_fd);

      int fd() const override;
      bool wait_readable(int timeout_seconds) override;
      ssize_t read(char *buffer, std::size_t length) override;
      ssize_t write(const char *buffer, std::size_t length) override;
      std::string protocol() const override;
      bool tls() const override;

    private:
      int client_fd_;
    };
  }
}

#endif
