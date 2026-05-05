// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SPEC_CPP_TEST_SUITES_HPP
#define VAJRA_SPEC_CPP_TEST_SUITES_HPP

namespace VajraSpecCpp
{
  void run_ipc_contract_tests();
  void run_ipc_frame_contract_tests();
  void run_ipc_frame_header_tests();
  void run_ipc_protocol_compatibility_tests();
  void run_server_lifecycle_tests();
  void run_request_head_tests();
  void run_response_tests();
}

#endif
