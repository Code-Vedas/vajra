// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_REQUEST_RACK_ENV_HPP
#define VAJRA_REQUEST_RACK_ENV_HPP

#include "request_context.hpp"

#include <string>
#include <vector>

namespace Vajra
{
  namespace request
  {
    struct RackEnvEntry
    {
      std::string key;
      std::string value;
    };

    struct RackRequestTarget
    {
      std::string path_info;
      std::string query_string;
    };

    class RackEnvBuilder
    {
    public:
      std::vector<RackEnvEntry> build(const RequestContext &request_context) const;
      RackRequestTarget split_target(const std::string &target) const;
    };
  }
}

#endif
