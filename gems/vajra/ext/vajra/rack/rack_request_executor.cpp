// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_request_executor.hpp"

#include "request/rack_env.hpp"
#include "ruby.h"
#include "ruby/thread.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  ID id_installed;
  ID id_call;
  ID id_message;
  ID id_backtrace;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    VALUE result = Qnil;
    VALUE exception = Qnil;
  };

  VALUE rack_execution_module()
  {
    const VALUE vajra_module = rb_const_get(rb_cObject, rb_intern("Vajra"));
    const VALUE internal_module = rb_const_get(vajra_module, rb_intern("Internal"));
    return rb_const_get(internal_module, rb_intern("RackExecution"));
  }

  VALUE ruby_env_entries_from(const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    VALUE ruby_entries = rb_ary_new_capa(static_cast<long>(env_entries.size()));
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      VALUE pair = rb_ary_new_capa(2);
      rb_ary_push(pair, rb_utf8_str_new(entry.key.data(), static_cast<long>(entry.key.size())));
      rb_ary_push(pair, rb_utf8_str_new(entry.value.data(), static_cast<long>(entry.value.size())));
      rb_ary_push(ruby_entries, pair);
    }

    return ruby_entries;
  }

  VALUE protected_execute_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    const VALUE execution_module = rack_execution_module();
    if (!RTEST(rb_funcall(execution_module, id_installed, 0)))
    {
      return Qnil;
    }

    return rb_funcall(execution_module, id_call, 1, ruby_env_entries_from(*context->env_entries));
  }

  void *execute_rack_request_with_gvl(void *data)
  {
    auto *context = static_cast<ExecutionCallContext *>(data);

    int state = 0;
    context->result = rb_protect(protected_execute_rack_request, reinterpret_cast<VALUE>(context), &state);
    if (state != 0)
    {
      context->exception = rb_errinfo();
      rb_set_errinfo(Qnil);
    }

    return nullptr;
  }

  std::string exception_message(VALUE exception)
  {
    VALUE message = rb_funcall(exception, id_message, 0);
    std::string rendered = StringValueCStr(message);
    VALUE backtrace = rb_funcall(exception, id_backtrace, 0);
    if (NIL_P(backtrace) || RARRAY_LEN(backtrace) == 0)
    {
      return rendered;
    }

    VALUE first_frame = rb_ary_entry(backtrace, 0);
    return rendered + " (" + StringValueCStr(first_frame) + ")";
  }

  Vajra::response::Header response_header_from_ruby(VALUE pair)
  {
    if (TYPE(pair) != T_ARRAY || RARRAY_LEN(pair) != 2)
    {
      throw std::runtime_error("Rack execution returned an invalid header entry");
    }

    VALUE name = rb_ary_entry(pair, 0);
    VALUE value = rb_ary_entry(pair, 1);
    return Vajra::response::Header{StringValueCStr(name), StringValueCStr(value)};
  }

  std::string reason_phrase_for_status(int status_code)
  {
    switch (status_code)
    {
      case 200:
        return "OK";
      case 201:
        return "Created";
      case 202:
        return "Accepted";
      case 204:
        return "No Content";
      case 400:
        return "Bad Request";
      case 404:
        return "Not Found";
      case 422:
        return "Unprocessable Entity";
      case 500:
        return "Internal Server Error";
      case 502:
        return "Bad Gateway";
      case 503:
        return "Service Unavailable";
      default:
        return "";
    }
  }

  Vajra::response::Response response_from_ruby(VALUE value)
  {
    if (TYPE(value) != T_ARRAY || RARRAY_LEN(value) != 3)
    {
      throw std::runtime_error("Rack execution returned an invalid normalized response");
    }

    VALUE status = rb_ary_entry(value, 0);
    VALUE headers = rb_ary_entry(value, 1);
    VALUE body = rb_ary_entry(value, 2);

    if (TYPE(headers) != T_ARRAY)
    {
      throw std::runtime_error("Rack execution returned invalid normalized headers");
    }

    std::vector<Vajra::response::Header> response_headers;
    response_headers.reserve(static_cast<std::size_t>(RARRAY_LEN(headers)));
    for (long index = 0; index < RARRAY_LEN(headers); ++index)
    {
      response_headers.push_back(response_header_from_ruby(rb_ary_entry(headers, index)));
    }

    const int status_code = NUM2INT(status);

    return Vajra::response::Response{
        Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
        std::move(response_headers),
        StringValueCStr(body),
        Vajra::response::ConnectionBehavior::close};
  }
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    const request::RequestContext &request_context) const
{
  if (id_installed == 0)
  {
    id_installed = rb_intern("installed?");
    id_call = rb_intern("call");
    id_message = rb_intern("message");
    id_backtrace = rb_intern("backtrace");
  }

  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);

  ExecutionCallContext context{&env_entries, Qnil, Qnil};
  rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);

  if (!NIL_P(context.exception))
  {
    throw std::runtime_error("Rack request execution failed: " + exception_message(context.exception));
  }

  if (NIL_P(context.result))
  {
    return std::nullopt;
  }

  return response_from_ruby(context.result);
}
