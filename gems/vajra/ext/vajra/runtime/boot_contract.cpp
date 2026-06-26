// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "runtime/boot_contract.hpp"

#include "runtime/ruby_support.hpp"

#include <mutex>
#include <stdexcept>

namespace
{
  ID id_port;
  ID id_max_request_head_bytes;
  ID id_runtime_role;
  std::mutex boot_callback_mutex;
  VALUE boot_callback = Qnil;

  VALUE ruby_boot_request_from(const Vajra::runtime::BootContractConfig &config)
  {
    VALUE boot_request = rb_hash_new();
    rb_hash_aset(boot_request, ID2SYM(id_port), INT2NUM(config.port));
    rb_hash_aset(boot_request, ID2SYM(id_max_request_head_bytes), SIZET2NUM(config.max_request_head_bytes));
    rb_hash_aset(
        boot_request,
        ID2SYM(id_runtime_role),
        rb_str_new(config.runtime_role.data(), static_cast<long>(config.runtime_role.size())));
    return boot_request;
  }

  VALUE protected_execute_boot_callback(VALUE data)
  {
    auto *config = reinterpret_cast<Vajra::runtime::BootContractConfig *>(data);
    VALUE callback = Qnil;
    {
      const std::lock_guard<std::mutex> callback_lock(boot_callback_mutex);
      callback = boot_callback;
    }

    if (NIL_P(callback))
    {
      rb_raise(
          rb_eRuntimeError,
          "Ruby boot contract callback is not installed. Require \"vajra\" or call Vajra::Internal::Boot.install! before starting Vajra.");
    }

    VALUE boot_request = ruby_boot_request_from(*config);
    VALUE arguments[] = {boot_request};
    return rb_proc_call(callback, rb_ary_new_from_values(1, arguments));
  }

  Vajra::runtime::BootStatus boot_status_from_ruby(VALUE status)
  {
    const std::string status_value =
        Vajra::runtime::ruby_string_value(status, "Ruby boot contract returned a non-string status");
    if (status_value == "pending")
    {
      return Vajra::runtime::BootStatus::pending;
    }
    if (status_value == "ready")
    {
      return Vajra::runtime::BootStatus::ready;
    }
    if (status_value == "failed")
    {
      return Vajra::runtime::BootStatus::failed;
    }

    throw std::runtime_error("Ruby boot contract returned an unsupported status: " + status_value);
  }

  std::optional<Vajra::runtime::BootDiagnostic> boot_diagnostic_from_ruby(VALUE diagnostic)
  {
    if (NIL_P(diagnostic))
    {
      return std::nullopt;
    }

    if (TYPE(diagnostic) != T_ARRAY || RARRAY_LEN(diagnostic) != 3)
    {
      throw std::runtime_error("Ruby boot contract returned an invalid diagnostic");
    }

    return Vajra::runtime::BootDiagnostic{
        Vajra::runtime::ruby_string_value(
            rb_ary_entry(diagnostic, 0),
            "Ruby boot contract returned a non-string diagnostic code"),
        Vajra::runtime::ruby_string_value(
            rb_ary_entry(diagnostic, 1),
            "Ruby boot contract returned a non-string diagnostic category"),
        Vajra::runtime::ruby_string_value(
            rb_ary_entry(diagnostic, 2),
            "Ruby boot contract returned a non-string diagnostic message")};
  }

  Vajra::runtime::BootContractResult boot_result_from_ruby(VALUE result)
  {
    if (TYPE(result) != T_ARRAY || RARRAY_LEN(result) != 3)
    {
      throw std::runtime_error("Ruby boot contract returned an invalid result");
    }

    return Vajra::runtime::BootContractResult{
        boot_status_from_ruby(rb_ary_entry(result, 0)),
        Vajra::runtime::ruby_string_value(
            rb_ary_entry(result, 1),
            "Ruby boot contract returned a non-string runtime role"),
        boot_diagnostic_from_ruby(rb_ary_entry(result, 2))};
  }
}

void Vajra::runtime::BootContract::initialize_ids()
{
  id_port = rb_intern("port");
  id_max_request_head_bytes = rb_intern("max_request_head_bytes");
  id_runtime_role = rb_intern("runtime_role");
  rb_global_variable(&boot_callback);
}

void Vajra::runtime::BootContract::set_callback(VALUE callback)
{
  const std::lock_guard<std::mutex> callback_lock(boot_callback_mutex);
  boot_callback = callback;
}

Vajra::runtime::BootContractResult Vajra::runtime::BootContract::run(const BootContractConfig &config)
{
  int state = 0;
  VALUE result = rb_protect(
      protected_execute_boot_callback,
      reinterpret_cast<VALUE>(const_cast<BootContractConfig *>(&config)),
      &state);
  if (state == 0)
  {
    return boot_result_from_ruby(result);
  }

  VALUE message = Qnil;
  int message_state = 0;
  message = rb_protect(Vajra::runtime::protected_format_ruby_exception_message, Qnil, &message_state);
  rb_set_errinfo(Qnil);

  if (message_state != 0 || NIL_P(message))
  {
    throw std::runtime_error("Ruby boot contract execution failed: Ruby exception");
  }

  throw std::runtime_error("Ruby boot contract execution failed: " + std::string(StringValueCStr(message)));
}

void Vajra::runtime::BootContract::ensure_ready(const BootContractResult &result)
{
  switch (result.status)
  {
  case BootStatus::ready:
    return;
  case BootStatus::pending:
    throw std::runtime_error("Ruby boot contract did not reach ready state");
  case BootStatus::failed:
    if (!result.diagnostic.has_value())
    {
      throw std::runtime_error("Ruby boot failed without diagnostic details");
    }

    throw std::runtime_error(
        "Ruby boot failed (" + result.diagnostic->code + "/" + result.diagnostic->category + "): " +
        result.diagnostic->message);
  }

  throw std::runtime_error("Ruby boot contract returned an unknown state");
}

Vajra::runtime::BootDiagnostic Vajra::runtime::BootContract::diagnostic_for_failure(const BootContractResult &result)
{
  switch (result.status)
  {
  case BootStatus::failed:
    if (result.diagnostic.has_value())
    {
      return *result.diagnostic;
    }
    return BootDiagnostic{
        "missing_boot_diagnostic",
        "contract",
        "Ruby boot failed without diagnostic details"};
  case BootStatus::pending:
    return BootDiagnostic{
        "boot_not_ready",
        "contract",
        "Ruby boot contract did not reach ready state"};
  case BootStatus::ready:
    return BootDiagnostic{
        "unexpected_ready_state",
        "contract",
        "Ruby worker bootstrap reported ready when a failure diagnostic was requested"};
  }

  return BootDiagnostic{
      "unknown_boot_state",
      "contract",
      "Ruby boot contract returned an unknown state"};
}
