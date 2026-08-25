// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/ruby_execution_bridge.hpp"

#include "rack/http2_stream.hpp"
#include "rack/native_input.hpp"
#include "response/http_header_utils.hpp"
#include "transport/tls_connection.hpp"
#include "ruby/encoding.h"
#include "ruby/thread.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <unordered_map>
#include <openssl/err.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace Vajra
{
  namespace rack
  {
    struct NativeHijackState
    {
      mutable std::mutex mutex;
      Vajra::platform::SocketHandle client_fd = Vajra::platform::kInvalidSocket;
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
  ID id_equal;
  ID id_instance_method;
  ID id_to_s;
  ID id_unbind;
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
  VALUE rb_array_each_method = Qnil;
  VALUE rb_key_remote_addr = Qnil;
  VALUE rb_key_remote_port = Qnil;
  VALUE rb_key_request_method = Qnil;
  VALUE rb_key_script_name = Qnil;
  VALUE rb_key_server_name = Qnil;
  VALUE rb_key_server_port = Qnil;
  VALUE rb_key_server_protocol = Qnil;
  std::atomic<bool> rack_multithread{false};

  // This map contains only the fixed vocabulary installed at extension load.
  // Request supplied header names must never be retained here: keeping Ruby
  // keys alive for each distinct HTTP_* name turns header diversity into a
  // process-lifetime memory allocation.
  std::unordered_map<std::string, VALUE> header_key_cache;

  struct HeaderCollectionContext
  {
    std::vector<Vajra::response::Header> headers;
    std::string error_message;
  };

  struct BodyCollectionContext
  {
    std::vector<std::string> chunks;
    std::string error_message;
  };

  struct BodyStreamingContext
  {
    std::shared_ptr<Vajra::response::ResponseBodyStream> body_stream;
    std::string error_message;
    std::function<void()> response_ready_callback;
    bool response_ready_published = false;
    bool cancelled = false;
  };

  struct NativeHijackWrapper
  {
    std::shared_ptr<Vajra::rack::NativeHijackState> state;
  };

  struct NativeTlsHijackIOState
  {
    std::unique_ptr<SSL, Vajra::transport::SslConnectionDeleter> ssl;
    Vajra::platform::SocketHandle fd = Vajra::platform::kInvalidSocket;
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

  struct StreamingBodyBlockCallContext
  {
    VALUE body;
    BodyStreamingContext *streaming;
  };

  struct ResponseBodyPushContext
  {
    Vajra::response::ResponseBodyStream *body_stream = nullptr;
    const char *data = nullptr;
    std::size_t size = 0;
    std::size_t accepted_bytes = 0;
    bool stopped = false;
    bool ran = false;
    std::exception_ptr *exception = nullptr;
  };

  struct BuiltinEachCheckContext
  {
    VALUE body = Qnil;
    bool trusted = false;
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
    return rb_funcallv(value, id_to_s, 0, nullptr);
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
      if (Vajra::platform::socket_valid(wrapper->state->fd))
      {
        Vajra::platform::close_socket(wrapper->state->fd);
        wrapper->state->fd = Vajra::platform::kInvalidSocket;
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
    if (wrapper == nullptr || !wrapper->state || wrapper->state->closed ||
        !Vajra::platform::socket_valid(wrapper->state->fd))
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
    Vajra::platform::SocketHandle fd = Vajra::platform::kInvalidSocket;
    Vajra::platform::WaitEvent event = Vajra::platform::WaitEvent::read;
    int timeout_milliseconds = 0;
    bool ready = false;
  };

  void *poll_without_gvl(void *data)
  {
    auto *context = static_cast<PollWaitContext *>(data);
    context->ready = Vajra::platform::wait_socket(
        context->fd,
        context->event,
        context->timeout_milliseconds);
    return nullptr;
  }

  bool wait_for_tls_hijack_events(
      Vajra::platform::SocketHandle fd,
      Vajra::platform::WaitEvent event,
      int timeout_milliseconds)
  {
    PollWaitContext context{fd, event, timeout_milliseconds, false};
    rb_thread_call_without_gvl(poll_without_gvl, &context, RUBY_UBF_IO, nullptr);
    return context.ready;
  }

  bool wait_for_tls_hijack_ssl_error(const NativeTlsHijackIOState &state, int ssl_error)
  {
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
      return wait_for_tls_hijack_events(
          state.fd,
          Vajra::platform::WaitEvent::read,
          state.read_timeout_milliseconds);
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
      return wait_for_tls_hijack_events(
          state.fd,
          Vajra::platform::WaitEvent::write,
          state.write_timeout_milliseconds);
    }
    return false;
  }

  VALUE native_tls_hijack_io_new(
      std::unique_ptr<SSL, Vajra::transport::SslConnectionDeleter> ssl,
      Vajra::platform::SocketHandle fd,
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
      const int result = wrapper->state->ssl != nullptr
                             ? SSL_write(
                                   wrapper->state->ssl.get(),
                                   data + written,
                                   static_cast<int>(length - written))
                             : static_cast<int>(Vajra::platform::send_socket(
                                   wrapper->state->fd,
                                   data + written,
                                   static_cast<std::size_t>(length - written)));
      if (result > 0)
      {
        written += result;
        continue;
      }

      if (wrapper->state->ssl != nullptr)
      {
        const int ssl_error = SSL_get_error(wrapper->state->ssl.get(), result);
        if (wait_for_tls_hijack_ssl_error(*wrapper->state, ssl_error))
        {
          continue;
        }
        error_message = "TLS rack.hijack write failed: " + openssl_error_string();
        break;
      }
      const int socket_error = Vajra::platform::socket_last_error();
      if ((Vajra::platform::socket_error_interrupted(socket_error) ||
           Vajra::platform::socket_error_would_block(socket_error)) &&
          wait_for_tls_hijack_events(
              wrapper->state->fd,
              Vajra::platform::WaitEvent::write,
              wrapper->state->write_timeout_milliseconds))
      {
        continue;
      }
      error_message = "rack.hijack write failed: " + Vajra::platform::socket_error_message(socket_error);
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
      const int result = wrapper->state->ssl != nullptr
                             ? SSL_read(wrapper->state->ssl.get(), buffer.data(), static_cast<int>(target))
                             : static_cast<int>(Vajra::platform::receive_socket(
                                   wrapper->state->fd,
                                   buffer.data(),
                                   static_cast<std::size_t>(target)));
      if (result > 0)
      {
        rb_str_cat(output, buffer.data(), result);
        if (!read_all && RSTRING_LEN(output) >= requested_length)
        {
          break;
        }
        continue;
      }

      if (wrapper->state->ssl == nullptr && result == 0)
      {
        eof = true;
        break;
      }
      if (wrapper->state->ssl != nullptr)
      {
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
      const int socket_error = Vajra::platform::socket_last_error();
      if ((Vajra::platform::socket_error_interrupted(socket_error) ||
           Vajra::platform::socket_error_would_block(socket_error)) &&
          wait_for_tls_hijack_events(
              wrapper->state->fd,
              Vajra::platform::WaitEvent::read,
              wrapper->state->read_timeout_milliseconds))
      {
        continue;
      }
      error_message = "rack.hijack read failed: " + Vajra::platform::socket_error_message(socket_error);
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
      const int result = wrapper->state->ssl != nullptr
                             ? SSL_read(wrapper->state->ssl.get(), buffer.data(), static_cast<int>(buffer.size()))
                             : static_cast<int>(Vajra::platform::receive_socket(
                                   wrapper->state->fd,
                                   buffer.data(),
                                   buffer.size()));
      if (result > 0)
      {
        rb_str_cat(output, buffer.data(), result);
        return output;
      }

      if (wrapper->state->ssl == nullptr && result == 0)
      {
        rb_raise(rb_eEOFError, "end of file reached");
      }
      if (wrapper->state->ssl != nullptr)
      {
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
      const int socket_error = Vajra::platform::socket_last_error();
      if ((Vajra::platform::socket_error_interrupted(socket_error) ||
           Vajra::platform::socket_error_would_block(socket_error)) &&
          wait_for_tls_hijack_events(
              wrapper->state->fd,
              Vajra::platform::WaitEvent::read,
              wrapper->state->read_timeout_milliseconds))
      {
        continue;
      }
      error_message = "rack.hijack read failed: " + Vajra::platform::socket_error_message(socket_error);
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
    if (Vajra::platform::socket_valid(wrapper->state->fd))
    {
      Vajra::platform::close_socket(wrapper->state->fd);
      wrapper->state->fd = Vajra::platform::kInvalidSocket;
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
    Vajra::platform::SocketHandle client_fd = Vajra::platform::kInvalidSocket;
    std::shared_ptr<Vajra::rack::NativeHijackTransport> transport;
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      if (!Vajra::platform::socket_valid(wrapper->state->client_fd))
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

#ifdef _WIN32
    if (!Vajra::platform::set_socket_nonblocking(client_fd, true))
    {
      rb_raise(rb_eIOError, "rack.hijack could not configure the native socket");
    }
    return native_tls_hijack_io_new(nullptr, client_fd, 30, 30);
#else

    VALUE keywords = rb_hash_new();
    rb_hash_aset(keywords, ID2SYM(rb_intern("autoclose")), Qtrue);
    VALUE arguments[] = {
        ULL2NUM(static_cast<unsigned long long>(client_fd)),
        rb_str_new_cstr("r+"),
        keywords};
    return rb_funcallv_kw(rb_cIO, id_for_fd, 3, arguments, RB_PASS_KEYWORDS);
#endif
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
      Vajra::platform::SocketHandle client_fd,
      const std::shared_ptr<Vajra::rack::NativeHijackTransport> &transport)
  {
    if (!Vajra::platform::socket_valid(client_fd))
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
      Vajra::platform::SocketHandle client_fd,
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

    const auto it = header_key_cache.find(key);
    if (it != header_key_cache.end())
    {
      return it->second;
    }

    const std::string http_key = "HTTP_" + key;
    // Preserve the immutable Rack env key contract without registering a
    // process-lifetime GC root; the request env owns this value.
    return rb_obj_freeze(rb_str_new(http_key.data(), ruby_string_length_for(http_key)));
  }

  VALUE protected_exception_message(VALUE data)
  {
    auto *exception = reinterpret_cast<VALUE *>(data);
    return rb_funcallv(*exception, id_exception_message, 0, nullptr);
  }

  VALUE rack_header_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE blockarg);
  VALUE rack_body_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE blockarg);
  VALUE rack_streaming_body_each_callback(VALUE yielded, VALUE data, int argc, const VALUE *argv, VALUE blockarg);
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

  VALUE protected_streaming_body_each(VALUE data)
  {
    auto *context = reinterpret_cast<StreamingBodyBlockCallContext *>(data);
    return rb_block_call(
        context->body,
        id_each,
        0,
        nullptr,
        rack_streaming_body_each_callback,
        reinterpret_cast<VALUE>(context->streaming));
  }

  VALUE protected_close_rack_body(VALUE data)
  {
    const VALUE body = data;
    if (rb_respond_to(body, id_close) == 0)
    {
      return Qnil;
    }

    return rb_funcallv(body, id_close, 0, nullptr);
  }

  VALUE protected_builtin_each_check(VALUE data)
  {
    auto *context = reinterpret_cast<BuiltinEachCheckContext *>(data);
    const VALUE current_method = rb_obj_method(context->body, ID2SYM(id_each));
    const VALUE current_unbound_method = rb_funcallv(current_method, id_unbind, 0, nullptr);
    context->trusted = RTEST(rb_funcallv(current_unbound_method, id_equal, 1, &rb_array_each_method));
    return Qnil;
  }

  bool array_uses_original_core_each(VALUE body)
  {
    BuiltinEachCheckContext context{body, false};
    int state = 0;
    rb_protect(protected_builtin_each_check, reinterpret_cast<VALUE>(&context), &state);
    if (state != 0)
    {
      // An application may make method lookup exotic.  It is safe to fall
      // back to transport framing, whereas guessing a fixed byte count is
      // not.
      rb_set_errinfo(Qnil);
      return false;
    }
    return context.trusted;
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

  void *push_response_body_without_gvl(void *data)
  {
    auto *context = static_cast<ResponseBodyPushContext *>(data);
    context->ran = true;
    try
    {
      const Vajra::response::ResponseBodyStream::PushResult result =
          context->body_stream->push_for(
              context->data,
              context->size,
              std::chrono::milliseconds(5));
      context->accepted_bytes = result.size;
      context->stopped = result.status == Vajra::response::ResponseBodyStream::PushStatus::stopped;
    }
    catch (...)
    {
      if (context->exception != nullptr)
      {
        *context->exception = std::current_exception();
      }
      context->stopped = true;
    }
    return nullptr;
  }

  VALUE rack_streaming_body_each_callback(VALUE yielded, VALUE data, int, const VALUE *, VALUE)
  {
    auto *context = reinterpret_cast<BodyStreamingContext *>(data);
    if (context->cancelled || context->body_stream->cancelled())
    {
      context->cancelled = true;
      rb_iter_break();
      return Qnil;
    }

    try
    {
      // Rack body chunks are byte strings.  Header values intentionally use
      // #to_s normalization, but doing that here would turn a malformed body
      // (for example an Integer) into wire bytes and conceal the Rack error.
      VALUE chunk = yielded;
      if (RB_TYPE_P(chunk, T_STRING) == 0)
      {
        throw std::runtime_error("Rack execution returned a non-string response body chunk");
      }

      const std::size_t chunk_length = static_cast<std::size_t>(RSTRING_LEN(chunk));
      // For an HTTP/1 direct execution, response-ready waits until the first
      // non-empty body byte.  This retains Rack's pre-commit error behavior:
      // an each/close failure before payload can still become a 500.  HTTP/2
      // explicitly publishes before #each so a peer reset can cancel a
      // producer that waits before its first yield.
      if (chunk_length > 0 && !context->response_ready_published && context->response_ready_callback)
      {
        context->response_ready_callback();
        context->response_ready_published = true;
      }
      bool stopped = false;
      for (std::size_t offset = 0; offset < chunk_length;)
      {
        // Copy before releasing the GVL.  The no-GVL context is POD and owns no
        // Ruby value: Thread#raise can therefore resume through rb_protect
        // without leaving a registered stack root or a C++ destructor behind.
        const std::size_t current_length = static_cast<std::size_t>(RSTRING_LEN(chunk));
        if (current_length < offset)
        {
          throw std::runtime_error("Rack response body chunk was mutated while streaming");
        }
        const std::size_t byte_count = std::min(
            std::min(
                Vajra::response::ResponseBodyStream::kMaximumChunkBytes,
                chunk_length - offset),
            current_length - offset);
        if (byte_count == 0)
        {
          throw std::runtime_error("Rack response body chunk was mutated while streaming");
        }

        char native_copy[Vajra::response::ResponseBodyStream::kMaximumChunkBytes];
        std::memcpy(native_copy, RSTRING_PTR(chunk) + offset, byte_count);
        bool interrupted = false;
        {
          std::exception_ptr push_exception;
          ResponseBodyPushContext push_context{
              context->body_stream.get(),
              native_copy,
              byte_count,
              0,
              false,
              false,
              &push_exception};
          // A condition-variable wait is not IO.  Return after a short
          // predicate-aware wait so Ruby interrupts are observed at a safe
          // boundary; rb_thread_call_without_gvl2 does not long-jump through
          // the native producer state on reacquire.
          rb_thread_call_without_gvl2(push_response_body_without_gvl, &push_context, nullptr, nullptr);
          if (push_exception)
          {
            std::rethrow_exception(push_exception);
          }
          interrupted = !push_context.ran;
          if (!interrupted && push_context.stopped)
          {
            context->cancelled = context->body_stream->cancelled();
            if (!context->cancelled)
            {
              context->error_message = "response body stream stopped before Rack body completed";
              context->body_stream->fail(context->error_message);
            }
            stopped = true;
          }
          else if (!interrupted)
          {
            offset += push_context.accepted_bytes;
          }
        }
        if (interrupted)
        {
          // `push_exception` and the no-GVL context have been destroyed
          // before this Ruby long-jump boundary.
          rb_thread_check_ints();
          return Qnil;
        }
        if (stopped)
        {
          break;
        }
      }
      if (stopped)
      {
        rb_iter_break();
      }
    }
    catch (const std::exception &error)
    {
      context->error_message = error.what();
      context->body_stream->fail(context->error_message);
      rb_iter_break();
    }
    return Qnil;
  }

  void append_response_body_chunk(BodyCollectionContext &context, std::string chunk)
  {
    context.chunks.push_back(std::move(chunk));
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

  std::optional<std::size_t> known_rack_response_body_length(VALUE body)
  {
    // A Rack body is consumed through #each below.  A subclass, singleton
    // method, or a redefinition/prepend of Array#each can legally yield bytes
    // unrelated to the backing array.  Only an exact Array whose resolved
    // #each still matches the core method captured during initialization is
    // safe to pre-size.
    if (TYPE(body) != T_ARRAY || rb_obj_class(body) != rb_cArray)
    {
      return std::nullopt;
    }
    if (!array_uses_original_core_each(body))
    {
      return std::nullopt;
    }

    std::size_t length = 0;
    for (long index = 0; index < RARRAY_LEN(body); ++index)
    {
      VALUE chunk = rb_ary_entry(body, index);
      if (RB_TYPE_P(chunk, T_STRING) == 0)
      {
        return std::nullopt;
      }
      const std::size_t chunk_length = static_cast<std::size_t>(RSTRING_LEN(chunk));
      if (chunk_length > std::numeric_limits<std::size_t>::max() - length)
      {
        return std::nullopt;
      }
      length += chunk_length;
    }
    return length;
  }

  std::optional<std::size_t> parse_rack_content_length(const std::string &value)
  {
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t'))
    {
      ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
    {
      --end;
    }
    if (begin == end)
    {
      return std::nullopt;
    }

    std::size_t length = 0;
    for (std::size_t index = begin; index < end; ++index)
    {
      const char character = value[index];
      if (character < '0' || character > '9')
      {
        return std::nullopt;
      }
      const std::size_t digit = static_cast<std::size_t>(character - '0');
      if (length > (std::numeric_limits<std::size_t>::max() - digit) / 10)
      {
        return std::nullopt;
      }
      length = length * 10 + digit;
    }
    return length;
  }

  std::optional<std::size_t> trusted_rack_response_body_length(
      VALUE body,
      const std::vector<Vajra::response::Header> &headers)
  {
    const std::optional<std::size_t> observed_length = known_rack_response_body_length(body);
    if (!observed_length)
    {
      return std::nullopt;
    }

    bool saw_content_length = false;
    for (const Vajra::response::Header &header : headers)
    {
      if (Vajra::response::header_name_equals(header.name, "transfer-encoding"))
      {
        // A live source must not reinterpret application transfer coding as a
        // fixed byte count.  The HTTP writer will choose its own framing.
        return std::nullopt;
      }
      if (!Vajra::response::header_name_equals(header.name, "content-length"))
      {
        continue;
      }

      const std::optional<std::size_t> declared_length = parse_rack_content_length(header.value);
      if (!declared_length || *declared_length != *observed_length || saw_content_length)
      {
        return std::nullopt;
      }
      saw_content_length = true;
    }

    // A built-in Array body supplies a finite current byte count.  The stream
    // enforces it on every producer push and turns later mutation/overflow
    // into a closed HTTP/1 response or an HTTP/2 stream error.  Overridden
    // arrays and arbitrary enumerables remain unknown-length and therefore
    // use transport framing.
    return observed_length;
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
  rb_global_variable(&rb_array_each_method);
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
  id_equal = rb_intern("==");
  id_for_fd = rb_intern("for_fd");
  id_instance_method = rb_intern("instance_method");
  id_to_s = rb_intern("to_s");
  id_unbind = rb_intern("unbind");
  {
    const VALUE name = ID2SYM(id_each);
    rb_array_each_method = rb_funcallv(rb_cArray, id_instance_method, 1, &name);
  }
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
    Vajra::platform::SocketHandle client_fd,
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
    Vajra::platform::SocketHandle client_fd,
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

  int status_code = 0;
  try
  {
    status_code = status_code_from_ruby(status);
  }
  catch (...)
  {
    close_rack_body(body);
    throw;
  }
  BodyCollectionContext response_body = response_body_from_ruby(body);

  Vajra::response::Response response{
      Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
      std::move(response_headers),
      "",
      Vajra::response::ConnectionBehavior::close};
  response.body_chunks = std::move(response_body.chunks);
  return response;
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_rack_result_head(
    VALUE value,
    VALUE *body_out,
    std::shared_ptr<Vajra::response::ConnectionBufferBudget> connection_buffer_budget)
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

  int status_code = 0;
  try
  {
    status_code = status_code_from_ruby(status);
  }
  catch (...)
  {
    close_rack_body(body);
    throw;
  }
  Vajra::response::Response response{
      Vajra::response::Status{status_code, reason_phrase_for_status(status_code)},
      std::move(header_context.headers),
      "",
      Vajra::response::ConnectionBehavior::close};
  response.body_stream = std::make_shared<Vajra::response::ResponseBodyStream>(
      Vajra::response::ResponseBodyStream::kDefaultCapacityBytes,
      trusted_rack_response_body_length(body, response.headers),
      std::move(connection_buffer_budget));
  if (body_out != nullptr)
  {
    *body_out = body;
  }
  return response;
}

void Vajra::rack::RackResponseHandler::stream_rack_body(
    VALUE body,
    const std::shared_ptr<Vajra::response::ResponseBodyStream> &body_stream,
    std::function<void()> response_ready_callback,
    bool publish_response_before_each)
{
  if (!body_stream)
  {
    throw std::runtime_error("Rack response body stream is missing");
  }

  BodyStreamingContext streaming_context{
      body_stream,
      "",
      std::move(response_ready_callback),
      false,
      false};

  // The Rack result has already been validated into a response head and owns
  // its bounded source.  Hand that source to the transport before invoking
  // body.each: a producer is allowed to wait before its first yield, and an
  // HTTP/2 peer/session cancellation must be able to wake that producer in
  // that state.  This publication is deliberately not producer completion;
  // mark_producer_finished() remains below, after Rack close has returned.
  if (publish_response_before_each && streaming_context.response_ready_callback)
  {
    streaming_context.response_ready_callback();
    streaming_context.response_ready_published = true;
  }

  int state = 0;
  if (body_stream->cancelled())
  {
    streaming_context.cancelled = true;
  }
  else
  {
    StreamingBodyBlockCallContext body_block_context{body, &streaming_context};
    rb_protect(protected_streaming_body_each, reinterpret_cast<VALUE>(&body_block_context), &state);
  }

  std::string error_message;
  if (state != 0 && !streaming_context.cancelled && !body_stream->cancelled())
  {
    error_message = RubyExecutionBridge::exception_message(rb_errinfo());
    rb_set_errinfo(Qnil);
  }
  if (error_message.empty() && !streaming_context.error_message.empty())
  {
    error_message = streaming_context.error_message;
  }

  state = 0;
  rb_protect(protected_close_rack_body, body, &state);
  if (state != 0 && !streaming_context.cancelled && !body_stream->cancelled())
  {
    error_message = RubyExecutionBridge::exception_message(rb_errinfo());
    rb_set_errinfo(Qnil);
  }

  if (body_stream->cancelled())
  {
    // A consumer cancellation unblocks body.each before the producer has run
    // Rack's close callback.  Publish the completion only after close so the
    // scheduler does not admit another request onto the same Ruby worker too
    // early.
    body_stream->mark_producer_finished();
    return;
  }
  if (!error_message.empty())
  {
    if (!streaming_context.response_ready_published)
    {
      // No response was handed off, but keep producer completion semantics
      // uniform: body.close has run and admission may now be released.
      body_stream->mark_producer_finished();
      throw std::runtime_error(error_message);
    }
    body_stream->fail(std::move(error_message));
    // fail() describes wire state only.  It must not imply body.close has
    // returned: a Rack close hook may still block while this session owns its
    // execution admission.
    body_stream->mark_producer_finished();
    return;
  }
  body_stream->finish();
  body_stream->mark_producer_finished();
}

Vajra::response::Response Vajra::rack::RackResponseHandler::response_from_rack_result(VALUE value)
{
  VALUE body = Qnil;
  Vajra::response::Response response = response_from_rack_result_head(value, &body);
  response.body_stream.reset();

  BodyCollectionContext body_context;
  int state = 0;
  try
  {
    BodyBlockCallContext body_block_context{body, &body_context};
    state = 0;
    rb_protect(protected_body_each, reinterpret_cast<VALUE>(&body_block_context), &state);
    if (state != 0)
    {
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

  response.body_chunks = std::move(body_context.chunks);
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
