// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_request_executor.hpp"

#include "request/http_field_utils.hpp"
#include "request/rack_env.hpp"
#include "request/request_head_error.hpp"
#include "ruby.h"
#include "ruby/encoding.h"
#include "ruby/thread.h"

#include <optional>
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
  ID id_to_a;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  struct InstalledCheckContext
  {
    bool installed = false;
    std::string error_message;
  };

  struct ResponseNormalizationContext
  {
    VALUE result = Qnil;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  std::string exception_message(VALUE exception);

  std::string ruby_string_value(VALUE value)
  {
    VALUE string_value = rb_obj_as_string(value);
    return std::string(RSTRING_PTR(string_value), static_cast<std::size_t>(RSTRING_LEN(string_value)));
  }

  void ensure_ruby_ids()
  {
    if (id_installed != 0)
    {
      return;
    }

    id_installed = rb_intern("installed?");
    id_call = rb_intern("call");
    id_message = rb_intern("message");
    id_backtrace = rb_intern("backtrace");
    id_to_a = rb_intern("to_a");
  }

  VALUE rack_execution_module()
  {
    ensure_ruby_ids();
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
      VALUE key = rb_str_new(entry.key.data(), static_cast<long>(entry.key.size()));
      VALUE value = rb_str_new(entry.value.data(), static_cast<long>(entry.value.size()));
      rb_enc_associate_index(key, rb_ascii8bit_encindex());
      rb_enc_associate_index(value, rb_ascii8bit_encindex());
      rb_ary_push(pair, key);
      rb_ary_push(pair, value);
      rb_ary_push(ruby_entries, pair);
    }

    return ruby_entries;
  }

  VALUE protected_installed_check(VALUE)
  {
    return rb_funcall(rack_execution_module(), id_installed, 0);
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

  void *installed_with_gvl(void *data)
  {
    auto *context = static_cast<InstalledCheckContext *>(data);
    int state = 0;
    VALUE result = rb_protect(protected_installed_check, Qnil, &state);
    if (state != 0)
    {
      context->error_message = exception_message(rb_errinfo());
      rb_set_errinfo(Qnil);
      return nullptr;
    }

    context->installed = RTEST(result);
    return nullptr;
  }

  std::string exception_message(VALUE exception)
  {
    VALUE message = rb_funcall(exception, id_message, 0);
    std::string rendered = ruby_string_value(message);
    VALUE backtrace = rb_funcall(exception, id_backtrace, 0);
    if (NIL_P(backtrace) || RARRAY_LEN(backtrace) == 0)
    {
      return rendered;
    }

    VALUE first_frame = rb_ary_entry(backtrace, 0);
    return rendered + " (" + ruby_string_value(first_frame) + ")";
  }

  Vajra::response::Header response_header_from_ruby(VALUE pair)
  {
    if (TYPE(pair) != T_ARRAY || RARRAY_LEN(pair) != 2)
    {
      throw std::runtime_error("Rack execution returned an invalid header entry");
    }

    VALUE name = rb_ary_entry(pair, 0);
    VALUE value = rb_ary_entry(pair, 1);
    return Vajra::response::Header{ruby_string_value(name), ruby_string_value(value)};
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
    VALUE headers = rb_funcall(rb_ary_entry(value, 1), id_to_a, 0);
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
        ruby_string_value(body),
        Vajra::response::ConnectionBehavior::close};
  }

  VALUE protected_normalize_rack_response(VALUE data)
  {
    auto *context = reinterpret_cast<ResponseNormalizationContext *>(data);
    try
    {
      context->response = response_from_ruby(context->result);
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    return Qnil;
  }

  void *execute_rack_request_with_gvl(void *data)
  {
    auto *context = static_cast<ExecutionCallContext *>(data);

    int state = 0;
    VALUE result = rb_protect(protected_execute_rack_request, reinterpret_cast<VALUE>(context), &state);
    if (state != 0)
    {
      context->error_message = exception_message(rb_errinfo());
      rb_set_errinfo(Qnil);
      return nullptr;
    }

    if (!NIL_P(result))
    {
      ResponseNormalizationContext normalization_context{result, std::nullopt, ""};
      state = 0;
      rb_protect(protected_normalize_rack_response, reinterpret_cast<VALUE>(&normalization_context), &state);
      if (state != 0)
      {
        context->error_message = exception_message(rb_errinfo());
        rb_set_errinfo(Qnil);
        return nullptr;
      }
      if (!normalization_context.error_message.empty())
      {
        context->error_message = normalization_context.error_message;
        return nullptr;
      }

      context->response = std::move(normalization_context.response);
    }

    return nullptr;
  }
  void ensure_bodyless_rack_request(const Vajra::request::RequestContext &request_context)
  {
    for (const Vajra::request::ParsedHeader &header : request_context.request.headers)
    {
      if (!Vajra::request::ascii_case_insensitive_equal(header.name, "Content-Length"))
      {
        continue;
      }

      if (Vajra::request::content_length_is_zero(header.value))
      {
        continue;
      }

      throw Vajra::request::bad_request_error(
          "Rack request execution does not support request bodies until body transport is implemented");
    }
  }
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    const request::RequestContext &request_context) const
{
  InstalledCheckContext installed_check_context{};
  rb_thread_call_with_gvl(installed_with_gvl, &installed_check_context);
  if (!installed_check_context.error_message.empty())
  {
    throw std::runtime_error("Rack execution installation check failed: " + installed_check_context.error_message);
  }

  if (!installed_check_context.installed)
  {
    return std::nullopt;
  }

  ensure_bodyless_rack_request(request_context);
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  ExecutionCallContext context{&env_entries, std::nullopt, ""};
  rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);

  if (!context.error_message.empty())
  {
    throw std::runtime_error("Rack request execution failed: " + context.error_message);
  }

  return context.response;
}
