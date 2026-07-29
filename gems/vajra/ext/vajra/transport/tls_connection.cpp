// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "tls_connection.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <openssl/err.h>
#include <sstream>
#include <stdexcept>

namespace
{
#ifdef _WIN32
  struct SocketBioState
  {
    Vajra::platform::SocketHandle socket;
  };

  int socket_bio_create(BIO *bio)
  {
    BIO_set_init(bio, 0);
    BIO_set_data(bio, nullptr);
    BIO_set_shutdown(bio, 0);
    return 1;
  }

  int socket_bio_destroy(BIO *bio)
  {
    if (bio == nullptr)
    {
      return 0;
    }
    delete static_cast<SocketBioState *>(BIO_get_data(bio));
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
  }

  int socket_bio_read(BIO *bio, char *buffer, int length)
  {
    BIO_clear_retry_flags(bio);
    if (buffer == nullptr || length <= 0)
    {
      return 0;
    }
    const auto *state = static_cast<const SocketBioState *>(BIO_get_data(bio));
    const auto result = Vajra::platform::receive_socket(state->socket, buffer, static_cast<std::size_t>(length));
    if (result < 0)
    {
      const int error_number = errno;
      if (Vajra::platform::socket_error_interrupted(error_number) ||
          Vajra::platform::socket_error_would_block(error_number))
      {
        BIO_set_retry_read(bio);
      }
    }
    return static_cast<int>(result);
  }

  int socket_bio_write(BIO *bio, const char *buffer, int length)
  {
    BIO_clear_retry_flags(bio);
    if (buffer == nullptr || length <= 0)
    {
      return 0;
    }
    const auto *state = static_cast<const SocketBioState *>(BIO_get_data(bio));
    const auto result = Vajra::platform::send_socket(state->socket, buffer, static_cast<std::size_t>(length));
    if (result < 0)
    {
      const int error_number = errno;
      if (Vajra::platform::socket_error_interrupted(error_number) ||
          Vajra::platform::socket_error_would_block(error_number))
      {
        BIO_set_retry_write(bio);
      }
    }
    return static_cast<int>(result);
  }

  long socket_bio_ctrl(BIO *bio, int command, long value, void *)
  {
    switch (command)
    {
    case BIO_CTRL_FLUSH:
      return 1;
    case BIO_CTRL_GET_CLOSE:
      return BIO_get_shutdown(bio);
    case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(bio, static_cast<int>(value));
      return 1;
    case BIO_CTRL_PENDING:
    case BIO_CTRL_WPENDING:
      return 0;
    default:
      return 0;
    }
  }

  BIO_METHOD *socket_bio_method()
  {
    static BIO_METHOD *method = []()
    {
      BIO_METHOD *value = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Vajra Windows socket");
      if (value == nullptr || BIO_meth_set_create(value, socket_bio_create) != 1 ||
          BIO_meth_set_destroy(value, socket_bio_destroy) != 1 ||
          BIO_meth_set_read(value, socket_bio_read) != 1 ||
          BIO_meth_set_write(value, socket_bio_write) != 1 ||
          BIO_meth_set_ctrl(value, socket_bio_ctrl) != 1)
      {
        BIO_meth_free(value);
        throw std::runtime_error("unable to create Windows socket BIO method");
      }
      return value;
    }();
    return method;
  }
#endif

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

#ifdef _WIN32
BIO *Vajra::transport::new_socket_bio(platform::SocketHandle socket)
{
  if (!platform::socket_valid(socket))
  {
    throw std::invalid_argument("cannot attach an invalid socket to OpenSSL");
  }
  BIO *bio = BIO_new(socket_bio_method());
  if (bio == nullptr)
  {
    throw std::runtime_error("unable to allocate Windows socket BIO: " + openssl_error_string());
  }
  try
  {
    BIO_set_data(bio, new SocketBioState{socket});
    BIO_set_init(bio, 1);
    BIO_set_shutdown(bio, 0);
    return bio;
  }
  catch (...)
  {
    BIO_free(bio);
    throw;
  }
}

Vajra::platform::SocketHandle Vajra::transport::socket_bio_handle(BIO *bio)
{
  const auto *state = bio == nullptr ? nullptr : static_cast<const SocketBioState *>(BIO_get_data(bio));
  return state == nullptr ? platform::kInvalidSocket : state->socket;
}
#endif

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

Vajra::transport::TlsConnection::TlsConnection(platform::SocketHandle client_fd, const TlsContext &context)
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
#ifdef _WIN32
  BIO *bio = new_socket_bio(client_fd_);
  SSL_set_bio(ssl_.get(), bio, bio);
#else
  if (SSL_set_fd(ssl_.get(), platform::openssl_socket_descriptor(client_fd_)) != 1)
  {
    throw std::runtime_error("unable to attach TLS connection to socket: " + openssl_error_string());
  }
#endif
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

Vajra::platform::SocketHandle Vajra::transport::TlsConnection::fd() const
{
  return client_fd_;
}

bool Vajra::transport::TlsConnection::wait_readable(int timeout_seconds)
{
  if (SSL_pending(ssl_.get()) > 0)
  {
    return true;
  }
  return wait_for_event(platform::WaitEvent::read, timeout_seconds);
}

Vajra::platform::SignedSize Vajra::transport::TlsConnection::read(char *buffer, std::size_t length)
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

Vajra::platform::SignedSize Vajra::transport::TlsConnection::write(const char *buffer, std::size_t length)
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

bool Vajra::transport::TlsConnection::wait_for_event(platform::WaitEvent event, int timeout_seconds)
{
  const int timeout_milliseconds = timeout_seconds <= 0 ? 0 : timeout_seconds * 1000;
  return wait_for_event_milliseconds(event, timeout_milliseconds);
}

bool Vajra::transport::TlsConnection::wait_for_event_milliseconds(
    platform::WaitEvent event,
    int timeout_milliseconds)
{
  return platform::wait_socket(client_fd_, event, timeout_milliseconds);
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
    return platform::wait_socket(client_fd_, platform::WaitEvent::read, timeout_milliseconds);
  }
  if (ssl_error == SSL_ERROR_WANT_WRITE)
  {
    return platform::wait_socket(client_fd_, platform::WaitEvent::write, timeout_milliseconds);
  }
  return false;
}

void Vajra::transport::TlsConnection::raise_ssl_error(const char *operation, int ssl_result) const
{
  std::ostringstream message;
  message << operation << " failed: ssl_result=" << ssl_result << " error=" << openssl_error_string();
  throw std::runtime_error(message.str());
}
