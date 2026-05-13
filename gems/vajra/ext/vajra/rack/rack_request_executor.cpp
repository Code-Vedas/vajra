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

#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  std::atomic<bool> rack_execution_callback_installed_flag{false};
  std::mutex rack_execution_callback_mutex;
  VALUE rack_execution_callback = Qnil;
  ID id_exception_message;

  struct ExecutionCallContext
  {
    const std::vector<Vajra::request::RackEnvEntry> *env_entries;
    const std::string *request_body;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  struct ResponseNormalizationContext
  {
    VALUE result = Qnil;
    std::optional<Vajra::response::Response> response;
    std::string error_message;
  };

  std::string exception_message(VALUE exception);

  VALUE protected_exception_message(VALUE data)
  {
    auto *exception = reinterpret_cast<VALUE *>(data);
    return rb_funcall(*exception, id_exception_message, 0);
  }

  std::string ruby_string_value(VALUE value)
  {
    if (RB_TYPE_P(value, T_STRING) == 0)
    {
      throw std::runtime_error("Rack execution returned a non-string normalized value");
    }

    return std::string(RSTRING_PTR(value), static_cast<std::size_t>(RSTRING_LEN(value)));
  }

  long ruby_string_length_for(const std::string &value)
  {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()))
    {
      throw std::runtime_error("native request payload exceeds Ruby string length limit");
    }

    return static_cast<long>(value.size());
  }

  VALUE ruby_binary_string_from(const std::string &value)
  {
    VALUE ruby_string = rb_str_new(value.empty() ? "" : value.data(), ruby_string_length_for(value));
    rb_enc_associate_index(ruby_string, rb_ascii8bit_encindex());
    return ruby_string;
  }

  VALUE ruby_env_entries_from(const std::vector<Vajra::request::RackEnvEntry> &env_entries)
  {
    VALUE ruby_entries = rb_ary_new_capa(static_cast<long>(env_entries.size()));
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      VALUE pair = rb_ary_new_capa(2);
      VALUE key = ruby_binary_string_from(entry.key);
      VALUE value = ruby_binary_string_from(entry.value);
      rb_ary_push(pair, key);
      rb_ary_push(pair, value);
      rb_ary_push(ruby_entries, pair);
    }

    return ruby_entries;
  }

  VALUE protected_execute_rack_request(VALUE data)
  {
    auto *context = reinterpret_cast<ExecutionCallContext *>(data);
    VALUE callback = Qnil;
    {
      const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
      callback = rack_execution_callback;
    }

    if (NIL_P(callback))
    {
      return Qnil;
    }

    VALUE env_entries = ruby_env_entries_from(*context->env_entries);
    VALUE request_body = ruby_binary_string_from(*context->request_body);
    VALUE arguments[] = {env_entries, request_body};
    return rb_proc_call(callback, rb_ary_new_from_values(2, arguments));
  }

  std::string exception_message(VALUE exception)
  {
    const std::string class_name = rb_obj_classname(exception);
    int state = 0;
    VALUE message = rb_protect(protected_exception_message, reinterpret_cast<VALUE>(&exception), &state);
    if (state != 0)
    {
      rb_set_errinfo(Qnil);
      return class_name;
    }

    if (RB_TYPE_P(message, T_STRING) == 0)
    {
      return class_name;
    }

    return class_name + ": " + ruby_string_value(message);
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
      case 100:
        return "Continue";
      case 101:
        return "Switching Protocols";
      case 102:
        return "Processing";
      case 103:
        return "Early Hints";
      case 200:
        return "OK";
      case 201:
        return "Created";
      case 202:
        return "Accepted";
      case 203:
        return "Non-Authoritative Information";
      case 204:
        return "No Content";
      case 205:
        return "Reset Content";
      case 206:
        return "Partial Content";
      case 207:
        return "Multi-Status";
      case 208:
        return "Already Reported";
      case 226:
        return "IM Used";
      case 300:
        return "Multiple Choices";
      case 301:
        return "Moved Permanently";
      case 302:
        return "Found";
      case 303:
        return "See Other";
      case 304:
        return "Not Modified";
      case 305:
        return "Use Proxy";
      case 307:
        return "Temporary Redirect";
      case 308:
        return "Permanent Redirect";
      case 400:
        return "Bad Request";
      case 401:
        return "Unauthorized";
      case 402:
        return "Payment Required";
      case 403:
        return "Forbidden";
      case 404:
        return "Not Found";
      case 405:
        return "Method Not Allowed";
      case 406:
        return "Not Acceptable";
      case 407:
        return "Proxy Authentication Required";
      case 408:
        return "Request Timeout";
      case 409:
        return "Conflict";
      case 410:
        return "Gone";
      case 411:
        return "Length Required";
      case 412:
        return "Precondition Failed";
      case 413:
        return "Content Too Large";
      case 414:
        return "URI Too Long";
      case 415:
        return "Unsupported Media Type";
      case 416:
        return "Range Not Satisfiable";
      case 417:
        return "Expectation Failed";
      case 418:
        return "I'm a teapot";
      case 421:
        return "Misdirected Request";
      case 422:
        return "Unprocessable Entity";
      case 423:
        return "Locked";
      case 424:
        return "Failed Dependency";
      case 425:
        return "Too Early";
      case 426:
        return "Upgrade Required";
      case 428:
        return "Precondition Required";
      case 429:
        return "Too Many Requests";
      case 431:
        return "Request Header Fields Too Large";
      case 451:
        return "Unavailable For Legal Reasons";
      case 500:
        return "Internal Server Error";
      case 501:
        return "Not Implemented";
      case 502:
        return "Bad Gateway";
      case 503:
        return "Service Unavailable";
      case 504:
        return "Gateway Timeout";
      case 505:
        return "HTTP Version Not Supported";
      case 506:
        return "Variant Also Negotiates";
      case 507:
        return "Insufficient Storage";
      case 508:
        return "Loop Detected";
      case 510:
        return "Not Extended";
      case 511:
        return "Network Authentication Required";
      default:
        return "Status";
    }
  }

  int status_code_from_ruby(VALUE status)
  {
    if (RB_INTEGER_TYPE_P(status) == 0)
    {
      throw std::runtime_error("Rack execution returned a non-integer HTTP status code");
    }

    if (RB_FIXNUM_P(status) == 0)
    {
      throw std::runtime_error("Rack execution returned an unrepresentable HTTP status code");
    }

    const long status_code = FIX2LONG(status);
    if (status_code < 100 || status_code > 599)
    {
      throw std::runtime_error("Rack execution returned an out-of-range HTTP status code");
    }

    return static_cast<int>(status_code);
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

    const int status_code = status_code_from_ruby(status);

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
}

void Vajra::rack::initialize_rack_execution_bridge()
{
  rb_global_variable(&rack_execution_callback);
  id_exception_message = rb_intern("message");
}

void Vajra::rack::set_rack_execution_callback(VALUE callback)
{
  {
    const std::lock_guard<std::mutex> callback_lock(rack_execution_callback_mutex);
    rack_execution_callback = callback;
  }
  rack_execution_callback_installed_flag.store(!NIL_P(callback), std::memory_order_release);
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    const request::RequestContext &request_context) const
{
  if (!rack_execution_callback_installed_flag.load(std::memory_order_acquire))
  {
    return std::nullopt;
  }

  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  ExecutionCallContext context{&env_entries, &request_context.request_body, std::nullopt, ""};
  rb_thread_call_with_gvl(execute_rack_request_with_gvl, &context);

  if (!context.error_message.empty())
  {
    throw std::runtime_error("Rack request execution failed: " + context.error_message);
  }

  return context.response;
}
