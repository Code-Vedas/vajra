// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_BOOT_CONTRACT_HPP
#define VAJRA_RUNTIME_BOOT_CONTRACT_HPP

#include "ruby.h"

#include <cstddef>
#include <optional>
#include <string>

namespace Vajra
{
  namespace runtime
  {
    struct BootDiagnostic
    {
      std::string code;
      std::string category;
      std::string message;
    };

    enum class BootStatus
    {
      pending,
      ready,
      failed,
    };

    struct BootContractConfig
    {
      int port;
      std::size_t max_request_head_bytes;
      std::string runtime_role;
    };

    struct BootContractResult
    {
      BootStatus status;
      std::string runtime_role;
      std::optional<BootDiagnostic> diagnostic;
    };

    class BootContract final
    {
    public:
      static void initialize_ids();
      static void set_callback(VALUE callback);
      static BootContractResult run(const BootContractConfig &config);
      static void ensure_ready(const BootContractResult &result);
      static BootDiagnostic diagnostic_for_failure(const BootContractResult &result);
    };
  }
}

#endif
