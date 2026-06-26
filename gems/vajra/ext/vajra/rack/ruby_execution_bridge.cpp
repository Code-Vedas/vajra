// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/ruby_execution_bridge.hpp"

#include "rack/http2_stream.hpp"
#include "rack/native_input.hpp"
#include "transport/tls_connection.hpp"
#include "ruby/encoding.h"
#include "ruby/thread.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <shared_mutex>
#include <unordered_map>
#include <openssl/err.h>
#include <poll.h>
#include <unistd.h>

namespace Vajra
{
  namespace rack
  {
    struct NativeHijackState
    {
      mutable std::mutex mutex;
      int client_fd = -1;
      VALUE rack_input = Qnil;
      std::shared_ptr<NativeInputState> input_state;
      std::shared_ptr<NativeHijackTransport> transport;
      std::size_t expected_body_bytes = 0;
      bool has_expected_body_bytes = false;
      bool called = false;
      bool committed = false;
    };
  }
}

namespace
{
  ID id_exception_message;
  ID id_call;
  ID id_close;
  ID id_each;
  ID id_for_fd;
  ID id_to_s;
  VALUE rb_key_content_length = Qnil;
  VALUE rb_key_content_type = Qnil;
  VALUE rb_key_path_info = Qnil;
  VALUE rb_key_query_string = Qnil;
  VALUE rb_key_rack_errors = Qnil;
  VALUE rb_key_rack_hijack = Qnil;
  VALUE rb_key_rack_input = Qnil;
  VALUE rb_key_http2_extended_connect = Qnil;
  VALUE rb_key_http2_stream = Qnil;
  VALUE rb_key_http2_websocket = Qnil;
  VALUE rb_key_rack_multiprocess = Qnil;
  VALUE rb_key_rack_multithread = Qnil;
  VALUE rb_key_rack_run_once = Qnil;
  VALUE rb_key_rack_url_scheme = Qnil;
  VALUE rb_key_rack_version = Qnil;
  VALUE rb_value_rack_version = Qnil;
  VALUE rb_cNativeHijack = Qnil;
  VALUE rb_cNativeTlsHijackIO = Qnil;
  VALUE rb_key_remote_addr = Qnil;
  VALUE rb_key_remote_port = Qnil;
  VALUE rb_key_request_method = Qnil;
  VALUE rb_key_script_name = Qnil;
  VALUE rb_key_server_name = Qnil;
  VALUE rb_key_server_port = Qnil;
  VALUE rb_key_server_protocol = Qnil;
  std::atomic<bool> rack_multithread{false};

  std::shared_mutex header_cache_mutex;
  std::unordered_map<std::string, VALUE> header_key_cache;
  constexpr std::size_t kResponseBodyMemoryBytes = 256 * 1024;

  struct HeaderCollectionContext
  {
    std::vector<Vajra::response::Header> headers;
    std::string error_message;
  };

  struct BodyCollectionContext
  {
    std::vector<std::string> chunks;
    std::shared_ptr<Vajra::response::ResponseBodyFile> body_file;
    std::size_t body_size = 0;
    std::string error_message;
  };

  struct NativeHijackWrapper
  {
    std::shared_ptr<Vajra::rack::NativeHijackState> state;
  };

  struct NativeTlsHijackIOState
  {
    std::unique_ptr<SSL, Vajra::transport::SslConnectionDeleter> ssl;
    int fd = -1;
    int read_timeout_milliseconds = 0;
    int write_timeout_milliseconds = 0;
    bool closed = false;
  };

  struct NativeTlsHijackIOWrapper
  {
    std::shared_ptr<NativeTlsHijackIOState> state;
  };

  struct HeaderBlockCallContext
  {
    VALUE headers;
    HeaderCollectionContext *collection;
  };

  struct BodyBlockCallContext
  {
    VALUE body;
    BodyCollectionContext *collection;
  };

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

  void install_http2_stream_if_present(VALUE env, const std::shared_ptr<Vajra::rack::Http2StreamState> &state)
  {
    if (!state)
    {
      return;
    }
    rb_hash_aset(env, rb_key_http2_stream, Vajra::rack::create_http2_stream(state));
    rb_hash_aset(env, rb_key_http2_extended_connect, Qtrue);
    rb_hash_aset(env, rb_key_http2_websocket, state->websocket ? Qtrue : Qfalse);
  }

  VALUE ruby_string_from_header_value(VALUE value)
  {
    return rb_funcall(value, id_to_s, 0);
  }

  VALUE frozen_ruby_key(const char *name)
  {
    VALUE key = rb_obj_freeze(rb_str_new_cstr(name));
    rb_gc_register_mark_object(key);
    return key;
  }

  void native_hijack_wrapper_mark(void *data)
  {
    auto *wrapper = static_cast<NativeHijackWrapper *>(data);
    if (wrapper != nullptr && wrapper->state && !NIL_P(wrapper->state->rack_input))
    {
      rb_gc_mark(wrapper->state->rack_input);
    }
  }

  void native_hijack_wrapper_free(void *data)
  {
    auto *wrapper = static_cast<NativeHijackWrapper *>(data);
    if (wrapper == nullptr)
    {
      return;
    }
    wrapper->state.reset();
    delete wrapper;
  }

  size_t native_hijack_wrapper_size(const void *data)
  {
    return data == nullptr ? 0 : sizeof(NativeHijackWrapper);
  }

  const rb_data_type_t native_hijack_type = {
      "Vajra::NativeHijack",
      {native_hijack_wrapper_mark, native_hijack_wrapper_free, native_hijack_wrapper_size, nullptr, {nullptr}},
      nullptr,
      nullptr,
      RUBY_TYPED_FREE_IMMEDIATELY};

  void native_tls_hijack_io_wrapper_free(void *data)
  {
    auto *wrapper = static_cast<NativeTlsHijackIOWrapper *>(data);
    if (wrapper == nullptr)
    {
      return;
    }
    if (wrapper->state && !wrapper->state->closed)
    {
      if (wrapper->state->ssl != nullptr)
      {
        SSL_shutdown(wrapper->state->ssl.get());
        wrapper->state->ssl.reset();
      }
      if (wrapper->state->fd >= 0)
      {
        close(wrapper->state->fd);
        wrapper->state->fd = -1;
      }
      wrapper->state->closed = true;
    }
    wrapper->state.reset();
    delete wrapper;
  }

  size_t native_tls_hijack_io_wrapper_size(const void *data)
  {
    return data == nullptr ? 0 : sizeof(NativeTlsHijackIOWrapper);
  }

  const rb_data_type_t native_tls_hijack_io_type = {
      "Vajra::NativeTlsHijackIO",
      {nullptr, native_tls_hijack_io_wrapper_free, native_tls_hijack_io_wrapper_size, nullptr, {nullptr}},
      nullptr,
      nullptr,
      RUBY_TYPED_FREE_IMMEDIATELY};

  NativeTlsHijackIOWrapper *native_tls_hijack_io_wrapper_from(VALUE self)
  {
    NativeTlsHijackIOWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, NativeTlsHijackIOWrapper, &native_tls_hijack_io_type, wrapper);
    if (wrapper == nullptr || !wrapper->state || wrapper->state->closed || wrapper->state->ssl == nullptr)
    {
      rb_raise(rb_eIOError, "rack.hijack IO is closed");
    }
    return wrapper;
  }

  NativeTlsHijackIOWrapper *native_tls_hijack_io_wrapper_unchecked(VALUE self)
  {
    NativeTlsHijackIOWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, NativeTlsHijackIOWrapper, &native_tls_hijack_io_type, wrapper);
    return wrapper;
  }

  std::string openssl_error_string()
  {
    const unsigned long error = ERR_get_error();
    if (error == 0)
    {
      return "no OpenSSL error available";
    }

    char buffer[256];
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return buffer;
  }

  struct PollWaitContext
  {
    int fd = -1;
    short events = 0;
    int timeout_milliseconds = 0;
    int result = 0;
    int error_number = 0;
  };

  void *poll_without_gvl(void *data)
  {
    auto *context = static_cast<PollWaitContext *>(data);
    pollfd descriptor{context->fd, context->events, 0};
    for (;;)
    {
      const int result = poll(&descriptor, 1, context->timeout_milliseconds);
      if (result > 0)
      {
        context->result = (descriptor.revents & context->events) != 0 ? 1 : 0;
        return nullptr;
      }
      if (result == 0)
      {
        context->result = 0;
        return nullptr;
      }
      if (errno != EINTR)
      {
        context->result = -1;
        context->error_number = errno;
        return nullptr;
      }
    }
  }

  bool wait_for_tls_hijack_events(int fd, short events, int timeout_milliseconds)
  {
    PollWaitContext context{fd, events, timeout_milliseconds, 0, 0};
    rb_thread_call_without_gvl(poll_without_gvl, &context, RUBY_UBF_IO, nullptr);
    return context.result > 0;
  }

  bool wait_for_tls_hijack_ssl_error(const NativeTlsHijackIOState &state, int ssl_error)
  {
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
      return wait_for_tls_hijack_events(state.fd, POLLIN | POLLHUP | POLLERR, state.read_timeout_milliseconds);
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
      return wait_for_tls_hijack_events(state.fd, POLLOUT | POLLHUP | POLLERR, state.write_timeout_milliseconds);
    }
    return false;
  }

  VALUE native_tls_hijack_io_new(
      std::unique_ptr<SSL, Vajra::transport::SslConnectionDeleter> ssl,
      int fd,
      int read_timeout_seconds,
      int write_timeout_seconds)
  {
    auto *wrapper = new NativeTlsHijackIOWrapper;
    wrapper->state = std::make_shared<NativeTlsHijackIOState>();
    wrapper->state->ssl = std::move(ssl);
    wrapper->state->fd = fd;
    wrapper->state->read_timeout_milliseconds = std::max(0, read_timeout_seconds) * 1000;
    wrapper->state->write_timeout_milliseconds = std::max(0, write_timeout_seconds) * 1000;
    return TypedData_Wrap_Struct(rb_cNativeTlsHijackIO, &native_tls_hijack_io_type, wrapper);
  }

  VALUE native_tls_hijack_io_write(VALUE self, VALUE value)
  {
    NativeTlsHijackIOWrapper *wrapper = native_tls_hijack_io_wrapper_from(self);
    VALUE string = StringValue(value);
    const char *data = RSTRING_PTR(string);
    const long length = RSTRING_LEN(string);
    long written = 0;
    std::string error_message;

    while (written < length)
    {
      const int result = SSL_write(
          wrapper->state->ssl.get(),
          data + written,
          static_cast<int>(length - written));
      if (result > 0)
      {
        written += result;
        continue;
      }

      const int ssl_error = SSL_get_error(wrapper->state->ssl.get(), result);
      if (wait_for_tls_hijack_ssl_error(*wrapper->state, ssl_error))
      {
        continue;
      }
      error_message = "TLS rack.hijack write failed: " + openssl_error_string();
      break;
    }

    if (!error_message.empty())
    {
      rb_raise(rb_eIOError, "%s", error_message.c_str());
    }
    return LONG2NUM(written);
  }

  VALUE native_tls_hijack_io_append(VALUE self, VALUE value)
  {
    native_tls_hijack_io_write(self, value);
    return self;
  }

  VALUE native_tls_hijack_io_read(int argc, VALUE *argv, VALUE self)
  {
    NativeTlsHijackIOWrapper *wrapper = native_tls_hijack_io_wrapper_from(self);
    VALUE length_value = Qnil;
    rb_scan_args(argc, argv, "01", &length_value);
    const bool read_all = NIL_P(length_value);
    const long requested_length = read_all ? 16 * 1024 : NUM2LONG(length_value);
    if (requested_length < 0)
    {
      rb_raise(rb_eArgError, "negative length %ld given", requested_length);
    }
    if (!read_all && requested_length == 0)
    {
      return rb_str_new("", 0);
    }

    VALUE output = rb_str_new("", 0);
    rb_enc_associate_index(output, rb_ascii8bit_encindex());
    std::vector<char> buffer(static_cast<std::size_t>(std::max<long>(1, requested_length)));
    std::string error_message;
    bool eof = false;
    for (;;)
    {
      const long target = read_all ? static_cast<long>(buffer.size()) : requested_length - RSTRING_LEN(output);
      const int result = SSL_read(wrapper->state->ssl.get(), buffer.data(), static_cast<int>(target));
      if (result > 0)
      {
        rb_str_cat(output, buffer.data(), result);
        if (!read_all && RSTRING_LEN(output) >= requested_length)
        {
          break;
        }
        continue;
      }

      const int ssl_error = SSL_get_error(wrapper->state->ssl.get(), result);
      if (ssl_error == SSL_ERROR_ZERO_RETURN)
      {
        eof = true;
        break;
      }
      if (wait_for_tls_hijack_ssl_error(*wrapper->state, ssl_error))
      {
        continue;
      }
      error_message = "TLS rack.hijack read failed: " + openssl_error_string();
      break;
    }

    if (!error_message.empty())
    {
      rb_raise(rb_eIOError, "%s", error_message.c_str());
    }
    if (eof && RSTRING_LEN(output) == 0 && !read_all)
    {
      return Qnil;
    }
    return output;
  }

  VALUE native_tls_hijack_io_readpartial(VALUE self, VALUE length_value)
  {
    NativeTlsHijackIOWrapper *wrapper = native_tls_hijack_io_wrapper_from(self);
    const long requested_length = NUM2LONG(length_value);
    if (requested_length <= 0)
    {
      rb_raise(rb_eArgError, "invalid read length %ld", requested_length);
    }
    VALUE output = rb_str_new("", 0);
    rb_enc_associate_index(output, rb_ascii8bit_encindex());
    std::vector<char> buffer(static_cast<std::size_t>(requested_length));
    std::string error_message;

    for (;;)
    {
      const int result = SSL_read(wrapper->state->ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
      if (result > 0)
      {
        rb_str_cat(output, buffer.data(), result);
        return output;
      }

      const int ssl_error = SSL_get_error(wrapper->state->ssl.get(), result);
      if (ssl_error == SSL_ERROR_ZERO_RETURN)
      {
        rb_raise(rb_eEOFError, "end of file reached");
      }
      if (wait_for_tls_hijack_ssl_error(*wrapper->state, ssl_error))
      {
        continue;
      }
      error_message = "TLS rack.hijack read failed: " + openssl_error_string();
      break;
    }

    rb_raise(rb_eIOError, "%s", error_message.c_str());
    return Qnil;
  }

  VALUE native_tls_hijack_io_flush(VALUE self)
  {
    native_tls_hijack_io_wrapper_from(self);
    return self;
  }

  VALUE native_tls_hijack_io_close(VALUE self)
  {
    NativeTlsHijackIOWrapper *wrapper = native_tls_hijack_io_wrapper_unchecked(self);
    if (wrapper == nullptr || !wrapper->state || wrapper->state->closed)
    {
      return Qnil;
    }
    if (wrapper->state->ssl != nullptr)
    {
      SSL_shutdown(wrapper->state->ssl.get());
      wrapper->state->ssl.reset();
    }
    if (wrapper->state->fd >= 0)
    {
      close(wrapper->state->fd);
      wrapper->state->fd = -1;
    }
    wrapper->state->closed = true;
    return Qnil;
  }

  VALUE native_tls_hijack_io_closed(VALUE self)
  {
    auto *wrapper = static_cast<NativeTlsHijackIOWrapper *>(RTYPEDDATA_DATA(self));
    return wrapper == nullptr || !wrapper->state || wrapper->state->closed ? Qtrue : Qfalse;
  }

  NativeHijackWrapper *native_hijack_wrapper_from(VALUE self)
  {
    NativeHijackWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, NativeHijackWrapper, &native_hijack_type, wrapper);
    if (wrapper == nullptr || !wrapper->state)
    {
      rb_raise(rb_eIOError, "rack.hijack is not available");
    }
    return wrapper;
  }

  VALUE native_hijack_call(VALUE self)
  {
    NativeHijackWrapper *wrapper = native_hijack_wrapper_from(self);
    std::string error_message;
    int client_fd = -1;
    std::shared_ptr<Vajra::rack::NativeHijackTransport> transport;
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      if (wrapper->state->client_fd < 0)
      {
        error_message = "rack.hijack is not available";
      }
      else if (wrapper->state->committed)
      {
        error_message = "rack.hijack is no longer available";
      }
      else if (wrapper->state->called)
      {
        error_message = "rack.hijack was already called";
      }
      else if (wrapper->state->input_state &&
               wrapper->state->has_expected_body_bytes &&
               !Vajra::rack::native_input_consumed_at_least(
                   wrapper->state->input_state.get(),
                   wrapper->state->expected_body_bytes))
      {
        error_message = "rack.hijack requires rack.input to be fully consumed";
      }
      else if (wrapper->state->input_state &&
               !wrapper->state->has_expected_body_bytes &&
               !Vajra::rack::native_input_eof_observed(wrapper->state->input_state.get()))
      {
        error_message = "rack.hijack requires rack.input to be fully consumed";
      }
      else if (wrapper->state->input_state && !Vajra::rack::native_input_fully_consumed(wrapper->state->input_state.get()))
      {
        error_message = "rack.hijack requires rack.input to be fully consumed";
      }
      else if (!wrapper->state->input_state && !NIL_P(wrapper->state->rack_input) && !Vajra::rack::native_input_fully_consumed(wrapper->state->rack_input))
      {
        error_message = "rack.hijack requires rack.input to be fully consumed";
      }
      else
      {
        wrapper->state->called = true;
        client_fd = wrapper->state->client_fd;
        transport = wrapper->state->transport;
      }
    }
    if (!error_message.empty())
    {
      rb_raise(rb_eIOError, "%s", error_message.c_str());
    }
    if (transport)
    {
      return transport->call();
    }

    VALUE keywords = rb_hash_new();
    rb_hash_aset(keywords, ID2SYM(rb_intern("autoclose")), Qtrue);
    VALUE arguments[] = {INT2NUM(client_fd), keywords};
    return rb_funcallv_kw(rb_cIO, id_for_fd, 2, arguments, RB_PASS_KEYWORDS);
  }

  VALUE native_hijack_new(std::shared_ptr<Vajra::rack::NativeHijackState> state)
  {
    auto *wrapper = new NativeHijackWrapper;
    wrapper->state = std::move(state);
    return TypedData_Wrap_Struct(rb_cNativeHijack, &native_hijack_type, wrapper);
  }

  class TlsNativeHijackTransport final : public Vajra::rack::NativeHijackTransport
  {
  public:
    explicit TlsNativeHijackTransport(Vajra::transport::TlsConnection &connection)
        : connection_(&connection)
    {
    }

    VALUE call() override
    {
      if (connection_ == nullptr)
      {
        rb_raise(rb_eIOError, "rack.hijack is not available");
      }
      auto ssl = connection_->release_ssl();
      if (ssl == nullptr)
      {
        rb_raise(rb_eIOError, "rack.hijack was already called");
      }
      return native_tls_hijack_io_new(
          std::move(ssl),
          connection_->fd(),
          connection_->read_timeout_seconds(),
          connection_->write_timeout_seconds());
    }

  private:
    Vajra::transport::TlsConnection *connection_ = nullptr;
  };

  bool rack_env_supports_full_hijack(
      const std::vector<Vajra::request::RackEnvEntry> &env_entries,
      int client_fd,
      const std::shared_ptr<Vajra::rack::NativeHijackTransport> &transport)
  {
    if (client_fd < 0)
    {
      return false;
    }

    bool http_1 = false;
    bool plain_http = false;
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      if (entry.key == "SERVER_PROTOCOL")
      {
        http_1 = entry.value == "HTTP/1.0" || entry.value == "HTTP/1.1";
      }
      else if (entry.key == "rack.url_scheme")
      {
        plain_http = entry.value == "http";
      }
    }
    return http_1 && (plain_http || transport != nullptr);
  }

  std::shared_ptr<Vajra::rack::NativeHijackState> install_hijack_if_supported(
      VALUE env,
      const std::vector<Vajra::request::RackEnvEntry> &env_entries,
      int client_fd,
      VALUE rack_input,
      std::shared_ptr<Vajra::rack::NativeInputState> input_state,
      std::shared_ptr<Vajra::rack::NativeHijackTransport> transport)
  {
    if (!rack_env_supports_full_hijack(env_entries, client_fd, transport))
    {
      return nullptr;
    }

    auto state = std::make_shared<Vajra::rack::NativeHijackState>();
    state->client_fd = client_fd;
    state->rack_input = rack_input;
    state->input_state = std::move(input_state);
    state->transport = std::move(transport);
    for (const Vajra::request::RackEnvEntry &entry : env_entries)
    {
      if (entry.key == "CONTENT_LENGTH")
      {
        try
        {
          state->expected_body_bytes = static_cast<std::size_t>(std::stoull(entry.value));
          state->has_expected_body_bytes = true;
        }
        catch (...)
        {
          state->expected_body_bytes = 1;
          state->has_expected_body_bytes = true;
        }
        break;
      }
    }
    rb_hash_aset(env, rb_key_rack_hijack, native_hijack_new(state));
    return state;
  }

  VALUE ruby_rack_env_key_from(const std::string &key)
  {
    if (key == "REQUEST_METHOD")
    {
      return rb_key_request_method;
    }
    if (key == "SCRIPT_NAME")
    {
      return rb_key_script_name;
    }
    if (key == "PATH_INFO")
    {
      return rb_key_path_info;
    }
    if (key == "QUERY_STRING")
    {
      return rb_key_query_string;
    }
    if (key == "SERVER_PROTOCOL")
    {
      return rb_key_server_protocol;
    }
    if (key == "SERVER_NAME")
    {
      return rb_key_server_name;
    }
    if (key == "SERVER_PORT")
    {
      return rb_key_server_port;
    }
    if (key == "REMOTE_ADDR")
    {
      return rb_key_remote_addr;
    }
    if (key == "REMOTE_PORT")
    {
      return rb_key_remote_port;
    }
    if (key == "rack.url_scheme")
    {
      return rb_key_rack_url_scheme;
    }
    if (key == "CONTENT_TYPE")
    {
      return rb_key_content_type;
    }
    if (key == "CONTENT_LENGTH")
    {
      return rb_key_content_length;
    }

    {
      std::shared_lock<std::shared_mutex> lock(header_cache_mutex);
      auto it = header_key_cache.find(key);
      if (it != header_key_cache.end())
      {
        return it->second;
      }
    }

    std::string http_key = "HTTP_" + key;
    VALUE ruby_key = frozen_ruby_key(http_key.c_str());

    {
      std::unique_lock<std::shared_mutex> lock(header_cache_mutex);
      auto it = header_key_cache.find(key);
      if (it != header_key_cache.end())
      {
        return it->second;
      }
      header_key_cache.emplace(key, ruby_key);
    }
    return ruby_key;
  }

  VALUE protected_exception_message(VALUE data)
  {
    auto *exception = reinterpret_cast<VALUE *>(data);
    return rb_funcall(*exception, id_exception_message, 0);
  }

  VALUE rack_header_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE blockarg);
  VALUE rack_body_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE blockarg);
  void append_response_body_chunk(BodyCollectionContext &context, std::string chunk);

  VALUE protected_header_each(VALUE data)
  {
    auto *context = reinterpret_cast<HeaderBlockCallContext *>(data);
    return rb_block_call(
        context->headers,
        id_each,
        0,
        nullptr,
        rack_header_each_callback,
        reinterpret_cast<VALUE>(context->collection));
  }

  VALUE protected_body_each(VALUE data)
  {
    auto *context = reinterpret_cast<BodyBlockCallContext *>(data);
    return rb_block_call(
        context->body,
        id_each,
        0,
        nullptr,
        rack_body_each_callback,
        reinterpret_cast<VALUE>(context->collection));
  }

  VALUE protected_close_rack_body(VALUE data)
  {
    const VALUE body = data;
    if (rb_respond_to(body, id_close) == 0)
    {
      return Qnil;
    }

    return rb_funcall(body, id_close, 0);
  }

  VALUE rack_header_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE)
  {
    auto *context = reinterpret_cast<HeaderCollectionContext *>(data);
    if (!context->error_message.empty())
    {
      return Qnil;
    }

    VALUE name = Qnil;
    VALUE value = Qnil;
    if (argc >= 2)
    {
      name = argv[0];
      value = argv[1];
    }
    else if (argc == 1 && TYPE(yielded) == T_ARRAY && RARRAY_LEN(yielded) >= 2)
    {
      name = rb_ary_entry(yielded, 0);
      value = rb_ary_entry(yielded, 1);
    }
    else
    {
      context->error_message = "Rack execution returned invalid headers";
      return Qnil;
    }

    try
    {
      std::string header_name = ruby_string_value(ruby_string_from_header_value(name));
      if (header_name == "rack.hijack")
      {
        context->error_message = "partial Rack hijack is not supported";
        return Qnil;
      }
      context->headers.push_back(Vajra::response::Header{
          std::move(header_name),
          ruby_string_value(ruby_string_from_header_value(value))});
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    return Qnil;
  }

  VALUE rack_body_each_callback(VALUE yielded, VALUE data, int, const VALUE *, VALUE)
  {
    auto *context = reinterpret_cast<BodyCollectionContext *>(data);
    if (!context->error_message.empty())
    {
      return Qnil;
    }

    try
    {
      append_response_body_chunk(*context, ruby_string_value(ruby_string_from_header_value(yielded)));
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
    }
    return Qnil;
  }

  std::shared_ptr<Vajra::response::ResponseBodyFile> open_response_body_file()
  {
    FILE *file = std::tmpfile();
    if (file == nullptr)
    {
      throw std::runtime_error(std::string("unable to create response body spill file: ") + std::strerror(errno));
    }
    return std::make_shared<Vajra::response::ResponseBodyFile>(file);
  }

  void write_response_body_file(
      const std::shared_ptr<Vajra::response::ResponseBodyFile> &body_file,
      const char *data,
      std::size_t length)
  {
    if (length == 0)
    {
      return;
    }
    if (std::fwrite(data, 1, length, body_file->file) != length)
    {
      throw std::runtime_error("unable to write response body spill file");
    }
    body_file->size += length;
  }

  void append_response_body_chunk(BodyCollectionContext &context, std::string chunk)
  {
    if (!context.body_file && context.body_size + chunk.size() > kResponseBodyMemoryBytes)
    {
      context.body_file = open_response_body_file();
      for (const std::string &existing : context.chunks)
      {
        write_response_body_file(context.body_file, existing.data(), existing.size());
      }
      context.chunks.clear();
      context.chunks.shrink_to_fit();
    }

    if (context.body_file)
    {
      write_response_body_file(context.body_file, chunk.data(), chunk.size());
    }
    else
    {
      context.chunks.push_back(std::move(chunk));
    }
    context.body_size += chunk.size();
  }

  void close_rack_body(VALUE body)
  {
    int state = 0;
    rb_protect(protected_close_rack_body, body, &state);
    if (state != 0)
    {
      throw Vajra::rack::RubyJumpTag(state);
    }
  }

  Vajra::response::Header response_header_from_ruby(VALUE pair)
  {
    if (TYPE(pair) != T_ARRAY || RARRAY_LEN(pair) != 2)
    {
      throw std::runtime_error("Rack execution returned an invalid header entry");
    }

    VALUE name = rb_ary_entry(pair, 0);
    VALUE value = rb_ary_entry(pair, 1);
    std::string header_name = ruby_string_value(ruby_string_from_header_value(name));
    if (header_name == "rack.hijack")
    {
      throw std::runtime_error("partial Rack hijack is not supported");
    }
    return Vajra::response::Header{std::move(header_name), ruby_string_value(ruby_string_from_header_value(value))};
  }

  BodyCollectionContext response_body_from_ruby(VALUE body)
  {
    BodyCollectionContext context;
    if (RB_TYPE_P(body, T_STRING) != 0)
    {
      append_response_body_chunk(context, ruby_string_value(body));
      return context;
    }
    if (TYPE(body) != T_ARRAY)
    {
      throw std::runtime_error("Rack execution returned invalid normalized body chunks");
    }

    context.chunks.reserve(std::min<std::size_t>(static_cast<std::size_t>(RARRAY_LEN(body)), 64));
    for (long index = 0; index < RARRAY_LEN(body); ++index)
    {
      append_response_body_chunk(context, ruby_string_value(rb_ary_entry(body, index)));
    }
    return context;
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
}

void Vajra::rack::RubyExecutionBridge::initialize()
{
  rb_global_variable(&rb_key_content_length);
  rb_global_variable(&rb_key_content_type);
  rb_global_variable(&rb_key_path_info);
  rb_global_variable(&rb_key_query_string);
  rb_global_variable(&rb_key_rack_errors);
  rb_global_variable(&rb_key_rack_hijack);
  rb_global_variable(&rb_key_rack_input);
  rb_global_variable(&rb_key_http2_extended_connect);
  rb_global_variable(&rb_key_http2_stream);
  rb_global_variable(&rb_key_http2_websocket);
  rb_global_variable(&rb_key_rack_multiprocess);
  rb_global_variable(&rb_key_rack_multithread);
  rb_global_variable(&rb_key_rack_run_once);
  rb_global_variable(&rb_key_rack_url_scheme);
  rb_global_variable(&rb_key_rack_version);
  rb_global_variable(&rb_value_rack_version);
  rb_global_variable(&rb_cNativeHijack);
  rb_global_variable(&rb_cNativeTlsHijackIO);
  rb_global_variable(&rb_key_remote_addr);
  rb_global_variable(&rb_key_remote_port);
  rb_global_variable(&rb_key_request_method);
  rb_global_variable(&rb_key_script_name);
  rb_global_variable(&rb_key_server_name);
  rb_global_variable(&rb_key_server_port);
  rb_global_variable(&rb_key_server_protocol);
  id_exception_message = rb_intern("message");
  id_call = rb_intern("call");
  id_close = rb_intern("close");
  id_each = rb_intern("each");
  id_for_fd = rb_intern("for_fd");
  id_to_s = rb_intern("to_s");
  rb_key_content_length = frozen_ruby_key("CONTENT_LENGTH");
  rb_key_content_type = frozen_ruby_key("CONTENT_TYPE");
  rb_key_path_info = frozen_ruby_key("PATH_INFO");
  rb_key_query_string = frozen_ruby_key("QUERY_STRING");
  rb_key_rack_errors = frozen_ruby_key("rack.errors");
  rb_key_rack_hijack = frozen_ruby_key("rack.hijack");
  rb_key_rack_input = frozen_ruby_key("rack.input");
  rb_key_http2_extended_connect = frozen_ruby_key("vajra.http2.extended_connect");
  rb_key_http2_stream = frozen_ruby_key("vajra.http2.stream");
  rb_key_http2_websocket = frozen_ruby_key("vajra.http2.websocket");
  rb_key_rack_multiprocess = frozen_ruby_key("rack.multiprocess");
  rb_key_rack_multithread = frozen_ruby_key("rack.multithread");
  rb_key_rack_run_once = frozen_ruby_key("rack.run_once");
  rb_key_rack_url_scheme = frozen_ruby_key("rack.url_scheme");
  rb_key_rack_version = frozen_ruby_key("rack.version");
  rb_key_remote_addr = frozen_ruby_key("REMOTE_ADDR");
  rb_key_remote_port = frozen_ruby_key("REMOTE_PORT");
  rb_key_request_method = frozen_ruby_key("REQUEST_METHOD");
  rb_key_script_name = frozen_ruby_key("SCRIPT_NAME");
  rb_key_server_name = frozen_ruby_key("SERVER_NAME");
  rb_key_server_port = frozen_ruby_key("SERVER_PORT");
  rb_key_server_protocol = frozen_ruby_key("SERVER_PROTOCOL");
  rb_value_rack_version = rb_ary_new_capa(2);
  rb_ary_push(rb_value_rack_version, INT2FIX(1));
  rb_ary_push(rb_value_rack_version, INT2FIX(6));
  rb_obj_freeze(rb_value_rack_version);

  VALUE mVajra = rb_define_module("Vajra");
  rb_cNativeHijack = rb_define_class_under(mVajra, "NativeHijack", rb_cObject);
  rb_undef_alloc_func(rb_cNativeHijack);
  rb_define_method(rb_cNativeHijack, "call", RUBY_METHOD_FUNC(native_hijack_call), 0);
  rb_cNativeTlsHijackIO = rb_define_class_under(mVajra, "NativeTlsHijackIO", rb_cObject);
  rb_undef_alloc_func(rb_cNativeTlsHijackIO);
  rb_define_method(rb_cNativeTlsHijackIO, "write", RUBY_METHOD_FUNC(native_tls_hijack_io_write), 1);
  rb_define_method(rb_cNativeTlsHijackIO, "<<", RUBY_METHOD_FUNC(native_tls_hijack_io_append), 1);
  rb_define_method(rb_cNativeTlsHijackIO, "read", RUBY_METHOD_FUNC(native_tls_hijack_io_read), -1);
  rb_define_method(rb_cNativeTlsHijackIO, "readpartial", RUBY_METHOD_FUNC(native_tls_hijack_io_readpartial), 1);
  rb_define_method(rb_cNativeTlsHijackIO, "flush", RUBY_METHOD_FUNC(native_tls_hijack_io_flush), 0);
  rb_define_method(rb_cNativeTlsHijackIO, "close", RUBY_METHOD_FUNC(native_tls_hijack_io_close), 0);
  rb_define_method(rb_cNativeTlsHijackIO, "closed?", RUBY_METHOD_FUNC(native_tls_hijack_io_closed), 0);

  const char *common_headers[] = {
      "ACCEPT", "ACCEPT_CHARSET", "ACCEPT_ENCODING", "ACCEPT_LANGUAGE", "ACCEPT_RANGES",
      "AGE", "ALLOW", "AUTHORIZATION", "CACHE_CONTROL", "CONNECTION", "COOKIE",
      "DATE", "EXPECT", "FORWARDED", "FROM", "HOST", "IF_MATCH", "IF_MODIFIED_SINCE",
      "IF_NONE_MATCH", "IF_RANGE", "IF_UNMODIFIED_SINCE", "MAX_FORWARDS", "ORIGIN",
      "PRAGMA", "PROXY_AUTHORIZATION", "RANGE", "REFERER", "TE", "TRAILER",
      "TRANSFER_ENCODING", "UPGRADE", "USER_AGENT", "VIA", "WARNING", "X_FORWARDED_FOR",
      "X_FORWARDED_HOST", "X_FORWARDED_PROTO", "X_REAL_IP", "X_REQUEST_ID"};

  std::unique_lock<std::shared_mutex> lock(header_cache_mutex);
  for (const char *header : common_headers)
  {
    std::string http_key = "HTTP_";
    http_key += header;
    header_key_cache.emplace(header, frozen_ruby_key(http_key.c_str()));
  }
}

ID Vajra::rack::RubyExecutionBridge::call_id()
{
  return id_call;
}

void Vajra::rack::RubyExecutionBridge::set_multithread(bool enabled)
{
  rack_multithread.store(enabled, std::memory_order_release);
}

VALUE Vajra::rack::RubyExecutionBridge::binary_string_from(const std::string &value)
{
  return ruby_binary_string_from(value);
}

VALUE Vajra::rack::RubyExecutionBridge::env_entries_array_from(
    const std::vector<Vajra::request::RackEnvEntry> &env_entries)
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

VALUE Vajra::rack::RubyExecutionBridge::rack_env_from(
    const std::vector<Vajra::request::RackEnvEntry> &env_entries,
    std::string request_body,
    int client_fd,
    std::shared_ptr<NativeHijackState> *hijack_state,
    std::shared_ptr<Http2StreamState> http2_stream,
    std::shared_ptr<NativeHijackTransport> native_hijack_transport)
{
  VALUE env = rb_hash_new();
  for (const Vajra::request::RackEnvEntry &entry : env_entries)
  {
    rb_hash_aset(env, ruby_rack_env_key_from(entry.key), ruby_binary_string_from(entry.value));
  }

  rb_hash_aset(env, rb_key_rack_version, rb_value_rack_version);
  std::shared_ptr<NativeInputState> input_state;
  VALUE rack_input = create_native_input_from_body(std::move(request_body)).value;
  rb_hash_aset(env, rb_key_rack_input, rack_input);
  std::shared_ptr<NativeHijackState> state = install_hijack_if_supported(
      env,
      env_entries,
      client_fd,
      rack_input,
      input_state,
      std::move(native_hijack_transport));
  install_http2_stream_if_present(env, http2_stream);
  if (hijack_state != nullptr)
  {
    *hijack_state = std::move(state);
  }
  rb_hash_aset(env, rb_key_rack_errors, rb_gv_get("$stderr"));
  rb_hash_aset(env, rb_key_rack_multithread, rack_multithread.load(std::memory_order_acquire) ? Qtrue : Qfalse);
  rb_hash_aset(env, rb_key_rack_multiprocess, Qfalse);
  rb_hash_aset(env, rb_key_rack_run_once, Qfalse);
  return env;
}

VALUE Vajra::rack::RubyExecutionBridge::rack_env_from(
    const std::vector<Vajra::request::RackEnvEntry> &env_entries,
    VALUE rack_input,
    int client_fd,
    std::shared_ptr<NativeInputState> input_state,
    std::shared_ptr<NativeHijackState> *hijack_state,
    std::shared_ptr<Http2StreamState> http2_stream,
    std::shared_ptr<NativeHijackTransport> native_hijack_transport)
{
  VALUE env = rb_hash_new();
  for (const Vajra::request::RackEnvEntry &entry : env_entries)
  {
    rb_hash_aset(env, ruby_rack_env_key_from(entry.key), ruby_binary_string_from(entry.value));
  }

  rb_hash_aset(env, rb_key_rack_version, rb_value_rack_version);
  rb_hash_aset(env, rb_key_rack_input, rack_input);
  std::shared_ptr<NativeHijackState> state = install_hijack_if_supported(
      env,
      env_entries,
      client_fd,
      rack_input,
      std::move(input_state),
      std::move(native_hijack_transport));
  install_http2_stream_if_present(env, http2_stream);
  if (hijack_state != nullptr)
  {
    *hijack_state = std::move(state);
  }
  rb_hash_aset(env, rb_key_rack_errors, rb_gv_get("$stderr"));
  rb_hash_aset(env, rb_key_rack_multithread, rack_multithread.load(std::memory_order_acquire) ? Qtrue : Qfalse);
  rb_hash_aset(env, rb_key_rack_multiprocess, Qfalse);
  rb_hash_aset(env, rb_key_rack_run_once, Qfalse);
  return env;
}

std::shared_ptr<Vajra::rack::NativeHijackTransport> Vajra::rack::tls_native_hijack_transport(
    Vajra::transport::TlsConnection &connection)
{
  return std::make_shared<TlsNativeHijackTransport>(connection);
}

bool Vajra::rack::RubyExecutionBridge::native_hijack_called(const std::shared_ptr<NativeHijackState> &state)
{
  if (!state)
  {
    return false;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->called;
}

void Vajra::rack::RubyExecutionBridge::commit_native_hijack(const std::shared_ptr<NativeHijackState> &state)
{
  if (!state)
  {
    return;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->called)
  {
    state->committed = true;
  }
}

void Vajra::rack::RubyExecutionBridge::close_rack_input(VALUE env)
{
  VALUE input = rb_hash_aref(env, rb_key_rack_input);
  if (NIL_P(input))
  {
    return;
  }

  close_rack_body(input);
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_normalized_result(VALUE value)
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
  BodyCollectionContext response_body = response_body_from_ruby(body);

  Vajra::response::Response response{
      Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
      std::move(response_headers),
      "",
      Vajra::response::ConnectionBehavior::close};
  response.body_chunks = std::move(response_body.chunks);
  response.body_file = std::move(response_body.body_file);
  return response;
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_rack_result(VALUE value)
{
  if (TYPE(value) != T_ARRAY || RARRAY_LEN(value) != 3)
  {
    throw std::runtime_error("Rack execution returned an invalid response");
  }

  VALUE status = rb_ary_entry(value, 0);
  VALUE headers = rb_ary_entry(value, 1);
  VALUE body = rb_ary_entry(value, 2);

  HeaderCollectionContext header_context;
  HeaderBlockCallContext header_block_context{headers, &header_context};
  int state = 0;
  rb_protect(protected_header_each, reinterpret_cast<VALUE>(&header_block_context), &state);
  if (state != 0)
  {
    close_rack_body(body);
    throw RubyJumpTag(state);
  }
  if (!header_context.error_message.empty())
  {
    close_rack_body(body);
    throw std::runtime_error(header_context.error_message);
  }

  BodyCollectionContext body_context;
  try
  {
    BodyBlockCallContext body_block_context{body, &body_context};
    state = 0;
    rb_protect(protected_body_each, reinterpret_cast<VALUE>(&body_block_context), &state);
    if (state != 0)
    {
      close_rack_body(body);
      throw RubyJumpTag(state);
    }
  }
  catch (...)
  {
    close_rack_body(body);
    throw;
  }
  close_rack_body(body);
  if (!body_context.error_message.empty())
  {
    throw std::runtime_error(body_context.error_message);
  }

  const int status_code = status_code_from_ruby(status);
  Vajra::response::Response response{
      Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
      std::move(header_context.headers),
      "",
      Vajra::response::ConnectionBehavior::close};
  response.body_chunks = std::move(body_context.chunks);
  response.body_file = std::move(body_context.body_file);
  return response;
}

std::string Vajra::rack::RubyExecutionBridge::exception_message(VALUE exception)
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
