// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "test_suites.hpp"

namespace VajraSpecCpp
{
  void run_ipc_contract_tests()
  {
    run_ipc_frame_contract_tests();
    run_ipc_frame_header_tests();
    run_ipc_protocol_compatibility_tests();
  }
}
