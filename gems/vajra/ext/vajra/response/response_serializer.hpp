// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RESPONSE_SERIALIZER_HPP
#define VAJRA_RESPONSE_SERIALIZER_HPP

#include "response.hpp"

#include <stdexcept>
#include <string>

namespace Vajra
{
  namespace response
  {
    class SerializationError : public std::runtime_error
    {
    public:
      explicit SerializationError(const std::string &message) : std::runtime_error(message) {}
    };

    class ResponseSerializer
    {
    public:
      std::string serialize(const Response &response) const;

    private:
      void validate_response(const Response &response) const;
      void validate_status(const Status &status) const;
      void validate_header(const Header &header) const;
    };
  }
}

#endif
