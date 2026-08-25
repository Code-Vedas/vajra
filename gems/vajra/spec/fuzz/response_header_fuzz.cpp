// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response/response_serializer.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::size_t delimiter = input.find('\n');
  const std::string name = input.substr(0, delimiter);
  const std::string value = delimiter == std::string::npos ? "" : input.substr(delimiter + 1);
  const Vajra::response::Response response{
      {200, "OK"},
      {{name, value}},
      ""};
  try
  {
    Vajra::response::ResponseSerializer serializer;
    serializer.validate(response);
  }
  catch (const Vajra::response::SerializationError &)
  {
  }
  catch (const std::exception &)
  {
  }
  return 0;
}
