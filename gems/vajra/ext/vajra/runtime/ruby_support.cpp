// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/ruby_support.hpp"

#include <stdexcept>

VALUE Vajra::runtime::protected_rb_integer(VALUE data)
{
  auto *conversion = reinterpret_cast<ProtectedIntegerConversion *>(data);
  conversion->result = rb_Integer(conversion->value);
  return conversion->result;
}

VALUE Vajra::runtime::protected_rb_obj_as_string(VALUE data)
{
  auto *conversion = reinterpret_cast<ProtectedStringConversion *>(data);
  conversion->result = rb_obj_as_string(conversion->value);
  return conversion->result;
}

VALUE Vajra::runtime::protected_rb_num2long(VALUE data)
{
  auto *value = reinterpret_cast<VALUE *>(data);
  return LONG2NUM(NUM2LONG(*value));
}

VALUE Vajra::runtime::protected_format_ruby_exception_message(VALUE)
{
  VALUE exception = rb_errinfo();
  if (NIL_P(exception))
  {
    return rb_str_new_cstr("Ruby exception");
  }

  return rb_obj_as_string(exception);
}

VALUE Vajra::runtime::protected_rb_hash_lookup(VALUE data)
{
  auto *lookup = reinterpret_cast<VALUE *>(data);
  const VALUE hash = lookup[0];
  const VALUE key = lookup[1];
  return rb_hash_lookup(hash, key);
}

std::string Vajra::runtime::ruby_string_value(VALUE value, const char *error_message)
{
  if (RB_TYPE_P(value, T_STRING) == 0)
  {
    throw std::runtime_error(error_message);
  }

  return std::string(RSTRING_PTR(value), static_cast<std::size_t>(RSTRING_LEN(value)));
}

VALUE Vajra::runtime::protected_ruby_call_value(VALUE (*func)(VALUE), VALUE data, const char *failure_context)
{
  int state = 0;
  const VALUE result = rb_protect(func, data, &state);
  if (state == 0)
  {
    return result;
  }

  VALUE message = Qnil;
  int message_state = 0;
  message = rb_protect(protected_format_ruby_exception_message, Qnil, &message_state);
  rb_set_errinfo(Qnil);

  if (message_state != 0 || NIL_P(message))
  {
    throw std::runtime_error(std::string(failure_context) + ": Ruby exception");
  }

  throw std::runtime_error(std::string(failure_context) + ": " + StringValueCStr(message));
}

long Vajra::runtime::protected_ruby_call_long(VALUE (*func)(VALUE), VALUE data, const char *failure_context)
{
  return NUM2LONG(protected_ruby_call_value(func, data, failure_context));
}
