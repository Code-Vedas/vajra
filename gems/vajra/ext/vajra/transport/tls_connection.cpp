// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "tls_connection.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>

namespace
{
  std::string openssl_error_string()
  {
    const unsigned long error = ERR_get_error();
    if (error == 0)
    {
      return "no OpenSSL error available";
    }

    char buffer[256];
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return buffer;
  }

  int tls_version_constant(const std::string &version)
  {
    if (version == "TLSv1_2")
    {
      return TLS1_2_VERSION;
    }
    if (version == "TLSv1_3")
    {
      return TLS1_3_VERSION;
    }

    throw std::runtime_error("unsupported TLS minimum version: " + version);
  }

  std::vector<unsigned char> encode_alpn_wire(const std::vector<std::string> &protocols)
  {
    std::vector<unsigned char> encoded;
    for (const std::string &protocol : protocols)
    {
      if (protocol.empty() || protocol.size() > 255)
      {
        throw std::runtime_error("invalid ALPN protocol length");
      }
      encoded.push_back(static_cast<unsigned char>(protocol.size()));
      encoded.insert(encoded.end(), protocol.begin(), protocol.end());
    }
    return encoded;
  }

  int select_alpn(
      SSL *,
      const unsigned char **out,
      unsigned char *outlen,
      const unsigned char *in,
      unsigned int inlen,
      void *arg)
  {
    const auto *wire = static_cast<const std::vector<unsigned char> *>(arg);
    if (wire == nullptr || wire->empty())
    {
      return SSL_TLSEXT_ERR_NOACK;
    }

    if (SSL_select_next_proto(
            const_cast<unsigned char **>(out),
            outlen,
            wire->data(),
            static_cast<unsigned int>(wire->size()),
            in,
            inlen) != OPENSSL_NPN_NEGOTIATED)
    {
      return SSL_TLSEXT_ERR_NOACK;
    }

    return SSL_TLSEXT_ERR_OK;
  }
}

void Vajra::transport::SslContextDeleter::operator()(SSL_CTX *context) const
{
  SSL_CTX_free(context);
}

void Vajra::transport::SslConnectionDeleter::operator()(SSL *ssl) const
{
  SSL_free(ssl);
}

Vajra::transport::TlsContext::TlsContext(const TlsConfig &config)
    : context_(SSL_CTX_new(TLS_server_method())),
      alpn_wire_(encode_alpn_wire(config.alpn_protocols)),
      handshake_timeout_seconds_(config.handshake_timeout_seconds),
      read_timeout_seconds_(config.read_timeout_seconds),
      write_timeout_seconds_(config.write_timeout_seconds)
{
  if (context_ == nullptr)
  {
    throw std::runtime_error("unable to create TLS context: " + openssl_error_string());
  }

  SSL_CTX_set_min_proto_version(context_.get(), tls_version_constant(config.min_version));
  if (SSL_CTX_use_certificate_chain_file(context_.get(), config.certificate.c_str()) != 1)
  {
    throw std::runtime_error("unable to load TLS certificate: " + openssl_error_string());
  }
  if (SSL_CTX_use_PrivateKey_file(context_.get(), config.private_key.c_str(), SSL_FILETYPE_PEM) != 1)
  {
    throw std::runtime_error("unable to load TLS private key: " + openssl_error_string());
  }
  if (SSL_CTX_check_private_key(context_.get()) != 1)
  {
    throw std::runtime_error("TLS private key does not match certificate: " + openssl_error_string());
  }

  if (!config.ca_certificate.empty())
  {
    if (SSL_CTX_load_verify_locations(context_.get(), config.ca_certificate.c_str(), nullptr) != 1)
    {
      throw std::runtime_error("unable to load TLS CA certificate: " + openssl_error_string());
    }
  }
  SSL_CTX_set_verify(
      context_.get(),
      config.verify_mode == "peer" ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_NONE,
      nullptr);
  SSL_CTX_set_alpn_select_cb(context_.get(), select_alpn, &alpn_wire_);
}

SSL_CTX *Vajra::transport::TlsContext::get() const
{
  return context_.get();
}

int Vajra::transport::TlsContext::handshake_timeout_seconds() const
{
  return handshake_timeout_seconds_;
}

int Vajra::transport::TlsContext::read_timeout_seconds() const
{
  return read_timeout_seconds_;
}

int Vajra::transport::TlsContext::write_timeout_seconds() const
{
  return write_timeout_seconds_;
}

Vajra::transport::TlsConnection::TlsConnection(int client_fd, const TlsContext &context)
    : client_fd_(client_fd),
      ssl_(SSL_new(context.get())),
      handshake_timeout_seconds_(context.handshake_timeout_seconds()),
      read_timeout_seconds_(context.read_timeout_seconds()),
      write_timeout_seconds_(context.write_timeout_seconds())
{
  if (ssl_ == nullptr)
  {
    throw std::runtime_error("unable to create TLS connection: " + openssl_error_string());
  }
  if (SSL_set_fd(ssl_.get(), client_fd_) != 1)
  {
    throw std::runtime_error("unable to attach TLS connection to socket: " + openssl_error_string());
  }
}

Vajra::transport::TlsConnection::~TlsConnection()
{
  if (ssl_ != nullptr && handshake_complete_)
  {
    SSL_shutdown(ssl_.get());
  }
}

void Vajra::transport::TlsConnection::handshake()
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(handshake_timeout_seconds_);
  for (;;)
  {
    const int result = SSL_accept(ssl_.get());
    if (result == 1)
    {
      handshake_complete_ = true;
      const unsigned char *protocol = nullptr;
      unsigned int protocol_length = 0;
      SSL_get0_alpn_selected(ssl_.get(), &protocol, &protocol_length);
      negotiated_protocol_ = protocol_length == 0
                                 ? "http/1.1"
                                 : std::string(reinterpret_cast<const char *>(protocol), protocol_length);
      return;
    }

    const int ssl_error = SSL_get_error(ssl_.get(), result);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0)
    {
      raise_ssl_error("TLS handshake", result);
    }
    if (wait_for_ssl_error_milliseconds(ssl_error, static_cast<int>(remaining)))
    {
      continue;
    }
    raise_ssl_error("TLS handshake", result);
  }
}

int Vajra::transport::TlsConnection::fd() const
{
  return client_fd_;
}

bool Vajra::transport::TlsConnection::wait_readable(int timeout_seconds)
{
  if (SSL_pending(ssl_.get()) > 0)
  {
    return true;
  }
  return wait_for_events(POLLIN | POLLHUP | POLLERR, timeout_seconds);
}

ssize_t Vajra::transport::TlsConnection::read(char *buffer, std::size_t length)
{
  if (ssl_ == nullptr)
  {
    errno = EBADF;
    return -1;
  }
  for (;;)
  {
    const int result = SSL_read(ssl_.get(), buffer, static_cast<int>(length));
    if (result > 0)
    {
      return result;
    }

    const int ssl_error = SSL_get_error(ssl_.get(), result);
    if (ssl_error == SSL_ERROR_ZERO_RETURN)
    {
      return 0;
    }
    if (wait_for_ssl_error(ssl_error, read_timeout_seconds_))
    {
      continue;
    }
    errno = ECONNRESET;
    return -1;
  }
}

ssize_t Vajra::transport::TlsConnection::write(const char *buffer, std::size_t length)
{
  if (ssl_ == nullptr)
  {
    errno = EBADF;
    return -1;
  }
  for (;;)
  {
    const int result = SSL_write(ssl_.get(), buffer, static_cast<int>(length));
    if (result > 0)
    {
      return result;
    }

    const int ssl_error = SSL_get_error(ssl_.get(), result);
    if (wait_for_ssl_error(ssl_error, write_timeout_seconds_))
    {
      continue;
    }
    errno = ECONNRESET;
    return -1;
  }
}

std::string Vajra::transport::TlsConnection::protocol() const
{
  return negotiated_protocol_;
}

bool Vajra::transport::TlsConnection::tls() const
{
  return true;
}

std::unique_ptr<SSL, Vajra::transport::SslConnectionDeleter> Vajra::transport::TlsConnection::release_ssl()
{
  handshake_complete_ = false;
  return std::move(ssl_);
}

int Vajra::transport::TlsConnection::read_timeout_seconds() const
{
  return read_timeout_seconds_;
}

int Vajra::transport::TlsConnection::write_timeout_seconds() const
{
  return write_timeout_seconds_;
}

bool Vajra::transport::TlsConnection::wait_for_events(short events, int timeout_seconds)
{
  const int timeout_milliseconds = timeout_seconds <= 0 ? 0 : timeout_seconds * 1000;
  return wait_for_events_milliseconds(events, timeout_milliseconds);
}

bool Vajra::transport::TlsConnection::wait_for_events_milliseconds(short events, int timeout_milliseconds)
{
  pollfd descriptor{client_fd_, events, 0};
  for (;;)
  {
    const int poll_result = poll(&descriptor, 1, timeout_milliseconds);
    if (poll_result > 0)
    {
      return (descriptor.revents & events) != 0;
    }
    if (poll_result == 0)
    {
      return false;
    }
    if (errno != EINTR)
    {
      return false;
    }
  }
}

bool Vajra::transport::TlsConnection::wait_for_ssl_error(int ssl_error, int timeout_seconds)
{
  const int timeout_milliseconds = timeout_seconds <= 0 ? 0 : timeout_seconds * 1000;
  return wait_for_ssl_error_milliseconds(ssl_error, timeout_milliseconds);
}

bool Vajra::transport::TlsConnection::wait_for_ssl_error_milliseconds(int ssl_error, int timeout_milliseconds)
{
  if (ssl_error == SSL_ERROR_WANT_READ)
  {
    return wait_for_events_milliseconds(POLLIN | POLLHUP | POLLERR, timeout_milliseconds);
  }
  if (ssl_error == SSL_ERROR_WANT_WRITE)
  {
    return wait_for_events_milliseconds(POLLOUT | POLLHUP | POLLERR, timeout_milliseconds);
  }
  return false;
}

void Vajra::transport::TlsConnection::raise_ssl_error(const char *operation, int ssl_result) const
{
  std::ostringstream message;
  message << operation << " failed: ssl_result=" << ssl_result << " error=" << openssl_error_string();
  throw std::runtime_error(message.str());
}
