// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/http2_stream.hpp"

#ifndef VAJRA_RUNTIME_TESTING
#include "ruby/encoding.h"
#include "ruby/thread.h"
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#ifndef VAJRA_RUNTIME_TESTING
namespace
{
  struct Http2StreamWrapper
  {
    std::shared_ptr<Vajra::rack::Http2StreamState> state;
  };

  struct WaitContext
  {
    Vajra::rack::Http2StreamState *state;
    enum class Predicate
    {
      data,
      capacity
    } predicate;
  };

  VALUE rb_mVajra = Qnil;
  VALUE rb_mHTTP2 = Qnil;
  VALUE rb_cStream = Qnil;
  ID id_to_s;
  constexpr std::uint32_t kCancelErrorCode = 0x8;

  long ruby_string_length_for(std::size_t length)
  {
    if (length > static_cast<std::size_t>(std::numeric_limits<long>::max()))
    {
      rb_raise(rb_eArgError, "HTTP/2 stream chunk exceeds Ruby string length limit");
    }
    return static_cast<long>(length);
  }

  VALUE binary_string_from(const char *data, std::size_t length)
  {
    VALUE ruby_string = rb_str_new(length == 0 ? "" : data, ruby_string_length_for(length));
    rb_enc_associate_index(ruby_string, rb_ascii8bit_encindex());
    return ruby_string;
  }

  VALUE replace_outbuf(VALUE outbuf, VALUE data)
  {
    if (NIL_P(outbuf))
    {
      return data;
    }
    if (NIL_P(data))
    {
      rb_str_replace(outbuf, rb_str_new(nullptr, 0));
    }
    else
    {
      rb_str_replace(outbuf, data);
    }
    return outbuf;
  }

  void http2_stream_wrapper_mark(void *) {}

  void http2_stream_wrapper_free(void *data)
  {
    auto *wrapper = static_cast<Http2StreamWrapper *>(data);
    if (wrapper == nullptr)
    {
      return;
    }
    wrapper->state.reset();
    delete wrapper;
  }

  size_t http2_stream_wrapper_size(const void *data)
  {
    return data == nullptr ? 0 : sizeof(Http2StreamWrapper);
  }

  const rb_data_type_t http2_stream_type = {
      "Vajra::HTTP2::Stream",
      {http2_stream_wrapper_mark, http2_stream_wrapper_free, http2_stream_wrapper_size, nullptr, {nullptr}},
      nullptr,
      nullptr,
      RUBY_TYPED_FREE_IMMEDIATELY};

  Http2StreamWrapper *http2_stream_wrapper_from(VALUE self)
  {
    Http2StreamWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, Http2StreamWrapper, &http2_stream_type, wrapper);
    if (wrapper == nullptr || !wrapper->state)
    {
      rb_raise(rb_eIOError, "HTTP/2 stream is not available");
    }
    return wrapper;
  }

  VALUE http2_stream_wrap(VALUE klass, std::shared_ptr<Vajra::rack::Http2StreamState> state)
  {
    auto *wrapper = new Http2StreamWrapper;
    wrapper->state = std::move(state);
    return TypedData_Wrap_Struct(klass, &http2_stream_type, wrapper);
  }

  const char *unusable_error_message(const Vajra::rack::Http2StreamState &state)
  {
    if (state.reset)
    {
      return "HTTP/2 stream was reset";
    }
    if (state.closed)
    {
      return "HTTP/2 stream is closed";
    }
    return nullptr;
  }

  void raise_unusable_if_needed(const char *error_message)
  {
    if (error_message != nullptr)
    {
      rb_raise(rb_eIOError, "%s", error_message);
    }
  }

  void *wait_without_gvl(void *data)
  {
    auto *context = static_cast<WaitContext *>(data);
    std::unique_lock<std::mutex> lock(context->state->mutex);
    if (context->predicate == WaitContext::Predicate::data)
    {
      context->state->data_condition.wait(lock, [context]()
                                          { return context->state->reset ||
                                                   context->state->closed ||
                                                   context->state->peer_closed ||
                                                   !context->state->inbound_chunks.empty(); });
    }
    else
    {
      context->state->capacity_condition.wait(lock, [context]()
                                              { return context->state->reset ||
                                                       context->state->closed ||
                                                       context->state->outbound_bytes < context->state->high_watermark; });
    }
    return nullptr;
  }

  VALUE http2_stream_accept(int argc, VALUE *argv, VALUE self)
  {
    VALUE status_value = Qnil;
    VALUE headers_value = Qnil;
    rb_scan_args(argc, argv, "02", &status_value, &headers_value);

    int status = 200;
    if (!NIL_P(status_value))
    {
      if (RB_INTEGER_TYPE_P(status_value) == 0)
      {
        rb_raise(rb_eArgError, "HTTP/2 stream accept status must be an Integer");
      }
      status = NUM2INT(status_value);
      if (status < 100 || status > 599)
      {
        rb_raise(rb_eArgError, "HTTP/2 stream accept status must be between 100 and 599");
      }
    }

    std::vector<Vajra::response::Header> headers;
    if (!NIL_P(headers_value))
    {
      Check_Type(headers_value, T_HASH);
      VALUE keys = rb_funcall(headers_value, rb_intern("keys"), 0);
      for (long index = 0; index < RARRAY_LEN(keys); ++index)
      {
        VALUE key = rb_ary_entry(keys, index);
        VALUE value = rb_hash_aref(headers_value, key);
        VALUE key_string = rb_funcall(key, id_to_s, 0);
        VALUE value_string = rb_funcall(value, id_to_s, 0);
        headers.push_back(Vajra::response::Header{
            std::string(RSTRING_PTR(key_string), static_cast<std::size_t>(RSTRING_LEN(key_string))),
            std::string(RSTRING_PTR(value_string), static_cast<std::size_t>(RSTRING_LEN(value_string)))});
      }
    }

    auto *wrapper = http2_stream_wrapper_from(self);
    const char *error_message = nullptr;
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      error_message = unusable_error_message(*wrapper->state);
      if (error_message == nullptr && wrapper->state->accepted)
      {
        error_message = "HTTP/2 stream was already accepted";
      }
      if (error_message == nullptr)
      {
        wrapper->state->accepted = true;
        wrapper->state->accept_status = status;
        wrapper->state->accept_headers = std::move(headers);
      }
    }
    raise_unusable_if_needed(error_message);
    wrapper->state->event_condition.notify_all();
    return self;
  }

  VALUE http2_stream_read(int argc, VALUE *argv, VALUE self)
  {
    VALUE length_value = Qnil;
    VALUE outbuf = Qnil;
    rb_scan_args(argc, argv, "02", &length_value, &outbuf);

    std::optional<std::size_t> requested;
    if (!NIL_P(length_value))
    {
      const long length = NUM2LONG(length_value);
      if (length < 0)
      {
        rb_raise(rb_eArgError, "negative HTTP/2 stream read length");
      }
      if (length == 0)
      {
        return replace_outbuf(outbuf, binary_string_from("", 0));
      }
      requested = static_cast<std::size_t>(length);
    }

    auto *wrapper = http2_stream_wrapper_from(self);
    std::string collected;
    for (;;)
    {
      std::optional<std::string> result;
      bool result_nil = false;
      const char *error_message = nullptr;
      {
        std::unique_lock<std::mutex> lock(wrapper->state->mutex);
        error_message = unusable_error_message(*wrapper->state);
        if (error_message == nullptr)
        {
          if (!wrapper->state->inbound_chunks.empty())
          {
            if (requested)
            {
              std::string &front = wrapper->state->inbound_chunks.front();
              const std::size_t byte_count = std::min(*requested, front.size());
              result = std::string(front.data(), byte_count);
              if (byte_count == front.size())
              {
                wrapper->state->inbound_chunks.pop_front();
              }
              else
              {
                front.erase(0, byte_count);
              }
              wrapper->state->inbound_bytes -= byte_count;
              wrapper->state->consumed_since_last_observation += byte_count;
              if (wrapper->state->inbound_bytes <= wrapper->state->low_watermark)
              {
                wrapper->state->capacity_condition.notify_all();
              }
              wrapper->state->event_condition.notify_all();
            }
            else
            {
              while (!wrapper->state->inbound_chunks.empty())
              {
                std::string &front = wrapper->state->inbound_chunks.front();
                const std::size_t byte_count = front.size();
                collected.append(front.data(), byte_count);
                wrapper->state->inbound_chunks.pop_front();
                wrapper->state->inbound_bytes -= byte_count;
                wrapper->state->consumed_since_last_observation += byte_count;
              }
              if (wrapper->state->inbound_bytes <= wrapper->state->low_watermark)
              {
                wrapper->state->capacity_condition.notify_all();
              }
              wrapper->state->event_condition.notify_all();
              if (wrapper->state->peer_closed)
              {
                result = collected;
              }
            }
          }
          if (!result && wrapper->state->peer_closed)
          {
            if (!collected.empty())
            {
              result = collected;
            }
            else
            {
              result_nil = true;
            }
          }
        }
      }
      raise_unusable_if_needed(error_message);
      if (result)
      {
        return replace_outbuf(outbuf, binary_string_from(result->data(), result->size()));
      }
      if (result_nil)
      {
        return replace_outbuf(outbuf, Qnil);
      }

      WaitContext context{wrapper->state.get(), WaitContext::Predicate::data};
      rb_thread_call_without_gvl(wait_without_gvl, &context, RUBY_UBF_IO, nullptr);
    }
  }

  VALUE http2_stream_write(VALUE self, VALUE value)
  {
    StringValue(value);
    auto *wrapper = http2_stream_wrapper_from(self);
    std::string chunk(RSTRING_PTR(value), static_cast<std::size_t>(RSTRING_LEN(value)));
    std::size_t offset = 0;
    while (offset < chunk.size())
    {
      const char *error_message = nullptr;
      {
        std::unique_lock<std::mutex> lock(wrapper->state->mutex);
        error_message = unusable_error_message(*wrapper->state);
        if (error_message == nullptr && !wrapper->state->accepted)
        {
          error_message = "HTTP/2 stream must be accepted before write";
        }
        if (error_message == nullptr && wrapper->state->outbound_bytes < wrapper->state->high_watermark)
        {
          const std::size_t capacity = wrapper->state->high_watermark - wrapper->state->outbound_bytes;
          const std::size_t byte_count = std::min(capacity, chunk.size() - offset);
          wrapper->state->outbound_chunks.emplace_back(chunk.data() + offset, byte_count);
          wrapper->state->outbound_bytes += byte_count;
          offset += byte_count;
          wrapper->state->event_condition.notify_all();
          continue;
        }
      }
      raise_unusable_if_needed(error_message);
      WaitContext context{wrapper->state.get(), WaitContext::Predicate::capacity};
      rb_thread_call_without_gvl(wait_without_gvl, &context, RUBY_UBF_IO, nullptr);
    }
    return SIZET2NUM(chunk.size());
  }

  VALUE http2_stream_flush(VALUE self)
  {
    auto *wrapper = http2_stream_wrapper_from(self);
    const char *error_message = nullptr;
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      error_message = unusable_error_message(*wrapper->state);
    }
    raise_unusable_if_needed(error_message);
    wrapper->state->event_condition.notify_all();
    return self;
  }

  VALUE http2_stream_close(VALUE self)
  {
    auto *wrapper = http2_stream_wrapper_from(self);
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      if (!wrapper->state->closed)
      {
        wrapper->state->app_closed = true;
        wrapper->state->closed = true;
      }
    }
    wrapper->state->data_condition.notify_all();
    wrapper->state->capacity_condition.notify_all();
    wrapper->state->event_condition.notify_all();
    return Qnil;
  }

  VALUE rb_http2_stream_reset(int argc, VALUE *argv, VALUE self)
  {
    VALUE error_value = Qnil;
    rb_scan_args(argc, argv, "01", &error_value);
    std::uint32_t error_code = kCancelErrorCode;
    if (!NIL_P(error_value) && RB_INTEGER_TYPE_P(error_value))
    {
      error_code = NUM2UINT(error_value);
    }
    auto *wrapper = http2_stream_wrapper_from(self);
    Vajra::rack::http2_stream_reset(wrapper->state.get(), error_code);
    return Qnil;
  }

  VALUE rb_http2_stream_closed(VALUE self)
  {
    auto *wrapper = http2_stream_wrapper_from(self);
    std::lock_guard<std::mutex> lock(wrapper->state->mutex);
    return (wrapper->state->closed || wrapper->state->reset || wrapper->state->peer_closed) ? Qtrue : Qfalse;
  }

  VALUE http2_stream_protocol(VALUE self)
  {
    auto *wrapper = http2_stream_wrapper_from(self);
    std::string protocol;
    {
      std::lock_guard<std::mutex> lock(wrapper->state->mutex);
      protocol = wrapper->state->protocol;
    }
    return binary_string_from(protocol.data(), protocol.size());
  }

  VALUE http2_stream_stream_id(VALUE self)
  {
    auto *wrapper = http2_stream_wrapper_from(self);
    std::lock_guard<std::mutex> lock(wrapper->state->mutex);
    return INT2NUM(wrapper->state->stream_id);
  }
}
#endif

void Vajra::rack::initialize_http2_stream()
{
#ifdef VAJRA_RUNTIME_TESTING
#else
  id_to_s = rb_intern("to_s");
  rb_mVajra = rb_define_module("Vajra");
  rb_mHTTP2 = rb_define_module_under(rb_mVajra, "HTTP2");
  rb_cStream = rb_define_class_under(rb_mHTTP2, "Stream", rb_cObject);
  rb_undef_alloc_func(rb_cStream);
  rb_define_method(rb_cStream, "accept", RUBY_METHOD_FUNC(http2_stream_accept), -1);
  rb_define_method(rb_cStream, "read", RUBY_METHOD_FUNC(http2_stream_read), -1);
  rb_define_method(rb_cStream, "write", RUBY_METHOD_FUNC(http2_stream_write), 1);
  rb_define_method(rb_cStream, "flush", RUBY_METHOD_FUNC(http2_stream_flush), 0);
  rb_define_method(rb_cStream, "close", RUBY_METHOD_FUNC(http2_stream_close), 0);
  rb_define_method(rb_cStream, "reset", RUBY_METHOD_FUNC(rb_http2_stream_reset), -1);
  rb_define_method(rb_cStream, "closed?", RUBY_METHOD_FUNC(rb_http2_stream_closed), 0);
  rb_define_method(rb_cStream, "protocol", RUBY_METHOD_FUNC(http2_stream_protocol), 0);
  rb_define_method(rb_cStream, "stream_id", RUBY_METHOD_FUNC(http2_stream_stream_id), 0);
#endif
}

VALUE Vajra::rack::create_http2_stream(std::shared_ptr<Http2StreamState> state)
{
#ifdef VAJRA_RUNTIME_TESTING
  (void)state;
  return Qnil;
#else
  return http2_stream_wrap(rb_cStream, std::move(state));
#endif
}

bool Vajra::rack::http2_stream_p(VALUE value)
{
#ifdef VAJRA_RUNTIME_TESTING
  (void)value;
  return false;
#else
  return rb_typeddata_is_kind_of(value, &http2_stream_type) != 0;
#endif
}

void Vajra::rack::http2_stream_append_inbound(Http2StreamState *state, const char *data, std::size_t length)
{
  if (length == 0)
  {
    return;
  }
  std::unique_lock<std::mutex> lock(state->mutex);
  state->capacity_condition.wait(lock, [state, length]()
                                 { return state->reset ||
                                          state->closed ||
                                          state->inbound_bytes + length <= state->high_watermark; });
  if (state->reset || state->closed)
  {
    return;
  }
  state->inbound_chunks.emplace_back(data, length);
  state->inbound_bytes += length;
  lock.unlock();
  state->data_condition.notify_all();
}

bool Vajra::rack::http2_stream_try_append_inbound(Http2StreamState *state, const char *data, std::size_t length)
{
  if (length == 0)
  {
    return true;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->reset || state->closed)
  {
    return true;
  }
  if (state->inbound_bytes + length > state->high_watermark)
  {
    return false;
  }
  state->inbound_chunks.emplace_back(data, length);
  state->inbound_bytes += length;
  state->data_condition.notify_all();
  return true;
}

void Vajra::rack::http2_stream_finish_inbound(Http2StreamState *state)
{
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->peer_closed = true;
  }
  state->data_condition.notify_all();
  state->event_condition.notify_all();
}

void Vajra::rack::http2_stream_reset(Http2StreamState *state, std::uint32_t error_code)
{
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->reset = true;
    state->closed = true;
    state->reset_error_code = error_code;
  }
  state->data_condition.notify_all();
  state->capacity_condition.notify_all();
  state->event_condition.notify_all();
}

std::size_t Vajra::rack::http2_stream_take_consumed_bytes(Http2StreamState *state)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  const std::size_t consumed = state->consumed_since_last_observation;
  state->consumed_since_last_observation = 0;
  return consumed;
}

bool Vajra::rack::http2_stream_has_outbound(Http2StreamState *state)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  return !state->outbound_chunks.empty();
}

bool Vajra::rack::http2_stream_app_closed(Http2StreamState *state)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->app_closed || state->closed || state->reset;
}

std::size_t Vajra::rack::http2_stream_drain_outbound(Http2StreamState *state, std::uint8_t *buffer, std::size_t length)
{
  std::size_t copied = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    while (copied < length && !state->outbound_chunks.empty())
    {
      std::string &front = state->outbound_chunks.front();
      const std::size_t byte_count = std::min(length - copied, front.size());
      std::memcpy(buffer + copied, front.data(), byte_count);
      copied += byte_count;
      state->outbound_bytes -= byte_count;
      if (byte_count == front.size())
      {
        state->outbound_chunks.pop_front();
      }
      else
      {
        front.erase(0, byte_count);
      }
    }
  }
  if (copied > 0)
  {
    state->capacity_condition.notify_all();
  }
  return copied;
}

bool Vajra::rack::http2_stream_take_accept(
    Http2StreamState *state,
    int &status,
    std::vector<Vajra::response::Header> &headers)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->accepted || state->accept_observed)
  {
    return false;
  }
  state->accept_observed = true;
  status = state->accept_status;
  headers = state->accept_headers;
  return true;
}

bool Vajra::rack::http2_stream_closed(Http2StreamState *state)
{
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->closed || state->reset || state->peer_closed;
}
