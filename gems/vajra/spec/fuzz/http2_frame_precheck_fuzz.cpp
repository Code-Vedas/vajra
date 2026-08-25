// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "fuzz_connection.hpp"
#include "request/http2_session.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace
{
  class NoopRequestExecutor final : public Vajra::request::RequestExecutor
  {
  public:
    std::optional<Vajra::response::Response> execute(const Vajra::request::RequestContext &) const override
    {
      return std::nullopt;
    }
  };

  int hex_value(char character)
  {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  }

  std::string decoded_input(const std::uint8_t *data, std::size_t size)
  {
    const std::string input(reinterpret_cast<const char *>(data), size);
    if (input.rfind("hex:", 0) != 0 || ((input.size() - 4) % 2) != 0)
    {
      return input;
    }
    std::string decoded;
    decoded.reserve((input.size() - 4) / 2);
    for (std::size_t index = 4; index < input.size(); index += 2)
    {
      const int high = hex_value(input[index]);
      const int low = hex_value(input[index + 1]);
      if (high < 0 || low < 0)
      {
        return input;
      }
      decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  constexpr const char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  const std::string frames = decoded_input(data, size);
  VajraSpecFuzz::BufferedConnection connection;
  try
  {
    Vajra::request::Http2Session session(
        connection,
        {"127.0.0.1", 12345, "fuzz.test", 443, "https"},
        {},
        std::make_shared<NoopRequestExecutor>(),
        nullptr,
        std::string(kClientPreface) + frames);
    session.run();
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
