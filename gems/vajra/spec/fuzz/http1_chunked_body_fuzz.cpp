// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "fuzz_connection.hpp"
#include "request/request_body_reader.hpp"
#include "request/request_head_error.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace
{
  std::string decoded_input(const std::uint8_t *data, std::size_t size)
  {
    const std::string input(reinterpret_cast<const char *>(data), size);
    if (input.rfind("escaped:", 0) != 0)
    {
      return input;
    }

    std::string decoded;
    decoded.reserve(input.size() - 8);
    for (std::size_t index = 8; index < input.size(); ++index)
    {
      if (input[index] == '\\' && index + 1 < input.size())
      {
        const char escaped = input[++index];
        decoded.push_back(escaped == 'r' ? '\r' : escaped == 'n' ? '\n' : escaped);
      }
      else
      {
        decoded.push_back(input[index]);
      }
    }
    return decoded;
  }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  Vajra::request::ParsedRequest request{
      {"POST", "/", "HTTP/1.1"},
      {{"Transfer-Encoding", "chunked"}}};
  VajraSpecFuzz::BufferedConnection connection;
  try
  {
    Vajra::request::RequestBodyReader reader(64 * 1024, 1024, 1024, 1);
    (void)reader.stream_read(
        connection,
        request,
        [](const char *, std::size_t) {},
        decoded_input(data, size));
  }
  catch (const Vajra::request::HeadError &)
  {
  }
  catch (const Vajra::request::BodyReadIncompleteError &)
  {
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
