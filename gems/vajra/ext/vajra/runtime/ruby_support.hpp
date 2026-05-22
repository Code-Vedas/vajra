// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RUNTIME_RUBY_SUPPORT_HPP
#define VAJRA_RUNTIME_RUBY_SUPPORT_HPP

#include "ruby.h"

#include <string>

namespace Vajra
{
  namespace runtime
  {
    struct ProtectedIntegerConversion
    {
      VALUE value;
      VALUE result;
    };

    struct ProtectedStringConversion
    {
      VALUE value;
      VALUE result;
    };

    VALUE protected_rb_integer(VALUE data);
    VALUE protected_rb_obj_as_string(VALUE data);
    VALUE protected_rb_num2long(VALUE data);
    VALUE protected_format_ruby_exception_message(VALUE);
    VALUE protected_rb_hash_lookup(VALUE data);

    std::string ruby_string_value(VALUE value, const char *error_message);
    VALUE protected_ruby_call_value(VALUE (*func)(VALUE), VALUE data, const char *failure_context);
    long protected_ruby_call_long(VALUE (*func)(VALUE), VALUE data, const char *failure_context);
  }
}

#endif
