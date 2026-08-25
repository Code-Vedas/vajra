// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "fuzz_connection.hpp"
#include "request/request_processor.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  const std::string settings(reinterpret_cast<const char *>(data), size);
  const std::string request =
      "GET / HTTP/1.1\r\n"
      "Host: fuzz.test\r\n"
      "Upgrade: h2c\r\n"
      "Connection: Upgrade, HTTP2-Settings\r\n"
      "HTTP2-Settings: " + settings + "\r\n\r\n";
  VajraSpecFuzz::BufferedConnection connection;
  try
  {
    const Vajra::request::RequestProcessor processor(Vajra::request::kDefaultMaxRequestHeadBytes, nullptr);
    (void)processor.handle_one(
        connection,
        {"127.0.0.1", 12345, "fuzz.test", 80, "http"},
        request);
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
