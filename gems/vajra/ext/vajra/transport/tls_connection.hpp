// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_TRANSPORT_TLS_CONNECTION_HPP
#define VAJRA_TRANSPORT_TLS_CONNECTION_HPP

#include "connection.hpp"

#include <chrono>
#include <memory>
#include <openssl/ssl.h>
#include <string>
#include <vector>

namespace Vajra
{
  namespace transport
  {
    struct TlsConfig
    {
      std::string certificate;
      std::string private_key;
      std::string ca_certificate;
      std::string verify_mode;
      std::string min_version;
      std::vector<std::string> alpn_protocols;
      int handshake_timeout_seconds = 5;
      int read_timeout_seconds = 30;
      int write_timeout_seconds = 25;
    };

    struct SslContextDeleter
    {
      void operator()(SSL_CTX *context) const;
    };

    struct SslConnectionDeleter
    {
      void operator()(SSL *ssl) const;
    };

    class TlsContext final
    {
    public:
      explicit TlsContext(const TlsConfig &config);
      ~TlsContext() = default;

      TlsContext(const TlsContext &) = delete;
      TlsContext &operator=(const TlsContext &) = delete;

      SSL_CTX *get() const;
      int handshake_timeout_seconds() const;
      int read_timeout_seconds() const;
      int write_timeout_seconds() const;

    private:
      std::unique_ptr<SSL_CTX, SslContextDeleter> context_;
      std::vector<unsigned char> alpn_wire_;
      int handshake_timeout_seconds_;
      int read_timeout_seconds_;
      int write_timeout_seconds_;
    };

    class TlsConnection final : public Connection
    {
    public:
      TlsConnection(int client_fd, const TlsContext &context);
      ~TlsConnection() override;

      TlsConnection(const TlsConnection &) = delete;
      TlsConnection &operator=(const TlsConnection &) = delete;

      void handshake();
      int fd() const override;
      bool wait_readable(int timeout_seconds) override;
      ssize_t read(char *buffer, std::size_t length) override;
      ssize_t write(const char *buffer, std::size_t length) override;
      std::string protocol() const override;
      bool tls() const override;
      std::unique_ptr<SSL, SslConnectionDeleter> release_ssl();
      int read_timeout_seconds() const;
      int write_timeout_seconds() const;

    private:
      bool wait_for_events(short events, int timeout_seconds);
      bool wait_for_events_milliseconds(short events, int timeout_milliseconds);
      bool wait_for_ssl_error(int ssl_error, int timeout_seconds);
      bool wait_for_ssl_error_milliseconds(int ssl_error, int timeout_milliseconds);
      [[noreturn]] void raise_ssl_error(const char *operation, int ssl_result) const;

      int client_fd_;
      std::unique_ptr<SSL, SslConnectionDeleter> ssl_;
      std::string negotiated_protocol_ = "http/1.1";
      bool handshake_complete_ = false;
      int handshake_timeout_seconds_;
      int read_timeout_seconds_;
      int write_timeout_seconds_;
    };
  }
}

#endif
