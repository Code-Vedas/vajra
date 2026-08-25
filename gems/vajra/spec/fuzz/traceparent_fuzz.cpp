// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/traceparent.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
  const std::string traceparent(reinterpret_cast<const char *>(data), size);
  (void)Vajra::runtime::traceparent_part(traceparent, 1);
  (void)Vajra::runtime::traceparent_part(traceparent, 2);
  (void)Vajra::runtime::traceparent_part(traceparent, 3);
  return 0;
}
