// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  try
  {
    Vajra::request::RequestHeadParser parser;
    (void)parser.parse(std::string(reinterpret_cast<const char *>(data), size));
  }
  catch (const Vajra::request::HeadError &)
  {
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
