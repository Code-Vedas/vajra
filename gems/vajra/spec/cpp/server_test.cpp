// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "test_suites.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

namespace VajraNative
{
  bool shutdown_requested()
  {
    return false;
  }
}

int main()
{
  try
  {
    VajraSpecCpp::run_ipc_contract_tests();
    VajraSpecCpp::run_server_lifecycle_tests();
    VajraSpecCpp::run_request_head_tests();
    VajraSpecCpp::run_response_tests();
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
