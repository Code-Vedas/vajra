// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "platform/socket.hpp"
#include "test_support.hpp"
#include "transport/tls_connection.hpp"

#include <limits>
#include <stdexcept>

namespace VajraSpecCpp
{
  void run_platform_socket_tests()
  {
    try
    {
      (void)Vajra::platform::openssl_socket_descriptor(Vajra::platform::kInvalidSocket);
      fail("OpenSSL descriptor conversion accepted an invalid socket");
    }
    catch (const std::invalid_argument &)
    {
    }

    const auto sockets = connected_socket_pair();
    SocketGuard client(sockets[0]);
    SocketGuard server(sockets[1]);
    const int descriptor = Vajra::platform::openssl_socket_descriptor(client.get());
    if (descriptor < 0)
    {
      fail("OpenSSL descriptor conversion rejected a representable socket");
    }

#ifdef _WIN32
    const auto oversized = static_cast<Vajra::platform::SocketHandle>(
        static_cast<unsigned long long>(std::numeric_limits<int>::max()) + 1ULL);
    BIO *wide_bio = Vajra::transport::new_socket_bio(oversized);
    expect_true(
        Vajra::transport::socket_bio_handle(wide_bio) == oversized,
        "Windows socket BIO truncated a pointer-sized socket handle");
    BIO_free(wide_bio);

    expect_true(
        Vajra::platform::set_socket_nonblocking(client.get(), true),
        "failed to make Windows socket non-blocking for BIO retry test");
    BIO *socket_bio = Vajra::transport::new_socket_bio(client.get());
    char byte = 0;
    expect_true(BIO_read(socket_bio, &byte, 1) < 0, "empty non-blocking socket BIO unexpectedly read data");
    expect_true(BIO_should_retry(socket_bio) != 0, "socket BIO did not mark would-block as retryable");
    expect_true(BIO_should_read(socket_bio) != 0, "socket BIO did not request a read retry");
    BIO_free(socket_bio);
    expect_true(
        Vajra::platform::socket_open(client.get()),
        "freeing the non-owning socket BIO closed its socket");
#endif
  }
}
