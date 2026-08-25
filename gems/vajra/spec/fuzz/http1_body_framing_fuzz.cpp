// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_body_reader.hpp"
#include "request/request_head_error.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::size_t delimiter = input.find('\n');
  const std::string content_length = input.substr(0, delimiter);
  const std::string transfer_encoding = delimiter == std::string::npos ? "" : input.substr(delimiter + 1);
  Vajra::request::ParsedRequest request{{"POST", "/", "HTTP/1.1"}, {}};
  if (!content_length.empty())
  {
    request.headers.push_back({"Content-Length", content_length});
  }
  if (!transfer_encoding.empty())
  {
    request.headers.push_back({"Transfer-Encoding", transfer_encoding});
  }

  try
  {
    Vajra::request::RequestBodyReader reader;
    (void)reader.plan_for(request);
  }
  catch (const Vajra::request::HeadError &)
  {
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
