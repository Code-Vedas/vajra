// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack/native_input.hpp"

#include "ruby/encoding.h"
#include "ruby/thread.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Vajra
{
  namespace rack
  {
    struct NativeInputState
    {
      mutable std::mutex mutex;
      std::condition_variable data_condition;
      std::condition_variable capacity_condition;
      std::deque<std::string> memory_chunks;
      std::size_t memory_base_offset = 0;
      std::size_t memory_bytes = 0;
      FILE *spill_file = nullptr;
      std::size_t bytes_written = 0;
      std::size_t read_offset = 0;
      std::size_t consumed_since_last_observation = 0;
      std::size_t pruned_bytes = 0;
      std::size_t spilled_bytes = 0;
      std::size_t watermark_wait_count = 0;
      std::size_t memory_limit = 1024 * 1024;
      std::size_t high_watermark = 1024 * 1024;
      std::size_t low_watermark = 512 * 1024;
      bool finished = false;
      bool closed = false;
      bool eof_observed = false;
      std::string error_message;

      ~NativeInputState()
      {
        if (spill_file != nullptr)
        {
          std::fclose(spill_file);
          spill_file = nullptr;
        }
      }
    };
  }
}

namespace
{
  struct NativeInputWrapper
  {
    std::shared_ptr<Vajra::rack::NativeInputState> state;
    std::string *body = nullptr;
    std::size_t read_offset = 0;
    bool closed = false;
    bool shared_empty = false;
  };

  struct WaitContext
  {
    Vajra::rack::NativeInputState *state;
    enum class Predicate
    {
      data,
      capacity
    } predicate;
  };

  VALUE rb_cNativeInput = Qnil;
  VALUE rb_empty_native_input = Qnil;
  constexpr std::size_t kNativeInputChunkBytes = 16 * 1024;
  constexpr std::size_t kNativeInputPruneThresholdBytes = 256 * 1024;

  void raise_io_error(const std::string &message)
  {
    rb_raise(rb_eIOError, "%s", message.c_str());
  }

  long ruby_string_length_for(std::size_t length)
  {
    if (length > static_cast<std::size_t>(std::numeric_limits<long>::max()))
    {
      rb_raise(rb_eArgError, "native input chunk exceeds Ruby string length limit");
    }
    return static_cast<long>(length);
  }

  VALUE binary_string_from(const std::string &value)
  {
    VALUE ruby_string = rb_str_new(value.empty() ? "" : value.data(), ruby_string_length_for(value.size()));
    rb_enc_associate_index(ruby_string, rb_ascii8bit_encindex());
    return ruby_string;
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

  void native_input_wrapper_mark(void *data)
  {
    (void)data;
  }

  void native_input_wrapper_free(void *data)
  {
    auto *wrapper = static_cast<NativeInputWrapper *>(data);
    if (wrapper == nullptr)
    {
      return;
    }
    wrapper->state.reset();
    delete wrapper->body;
    wrapper->body = nullptr;
    delete wrapper;
  }

  size_t native_input_wrapper_size(const void *data)
  {
    return data == nullptr ? 0 : sizeof(NativeInputWrapper);
  }

  const rb_data_type_t native_input_type = {
      "Vajra::NativeInput",
      {native_input_wrapper_mark, native_input_wrapper_free, native_input_wrapper_size, nullptr, {nullptr}},
      nullptr,
      nullptr,
      RUBY_TYPED_FREE_IMMEDIATELY};

  VALUE native_input_wrap(VALUE klass, NativeInputWrapper *wrapper)
  {
    return TypedData_Wrap_Struct(klass, &native_input_type, wrapper);
  }

  NativeInputWrapper *native_input_wrapper_from(VALUE self)
  {
    NativeInputWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, NativeInputWrapper, &native_input_type, wrapper);
    if (wrapper == nullptr || (wrapper->closed && !wrapper->shared_empty))
    {
      rb_raise(rb_eIOError, "rack.input is closed");
    }
    return wrapper;
  }

  Vajra::rack::NativeInputState *native_input_state_from_ruby(VALUE self)
  {
    auto *wrapper = native_input_wrapper_from(self);
    if (!wrapper->state)
    {
      rb_raise(rb_eTypeError, "expected live Vajra::NativeInput");
    }
    return wrapper->state.get();
  }

  VALUE native_input_allocate(VALUE klass)
  {
    auto *wrapper = new NativeInputWrapper;
    wrapper->state = std::make_shared<Vajra::rack::NativeInputState>();
    return TypedData_Wrap_Struct(klass, &native_input_type, wrapper);
  }

  VALUE preloaded_native_input(std::string body = "")
  {
    auto *wrapper = new NativeInputWrapper;
    wrapper->body = new std::string(std::move(body));
    return TypedData_Wrap_Struct(rb_cNativeInput, &native_input_type, wrapper);
  }

  VALUE shared_empty_native_input()
  {
    if (NIL_P(rb_empty_native_input))
    {
      auto *wrapper = new NativeInputWrapper;
      wrapper->body = new std::string();
      wrapper->closed = true;
      wrapper->shared_empty = true;
      rb_empty_native_input = TypedData_Wrap_Struct(rb_cNativeInput, &native_input_type, wrapper);
    }
    return rb_empty_native_input;
  }

  std::size_t preloaded_body_length(const NativeInputWrapper &wrapper)
  {
    return wrapper.body == nullptr ? 0 : wrapper.body->size();
  }

  const char *preloaded_body_data(const NativeInputWrapper &wrapper)
  {
    return wrapper.body == nullptr ? "" : wrapper.body->data();
  }

  VALUE read_preloaded_native_input(NativeInputWrapper &wrapper, VALUE length_value, VALUE outbuf)
  {
    if (!NIL_P(length_value))
    {
      const long requested_length = NUM2LONG(length_value);
      if (requested_length < 0)
      {
        rb_raise(rb_eArgError, "negative length");
      }
      if (requested_length == 0)
      {
        return replace_outbuf(outbuf, binary_string_from(std::string()));
      }
    }

    const std::size_t body_length = preloaded_body_length(wrapper);
    if (wrapper.read_offset >= body_length)
    {
      return replace_outbuf(outbuf, NIL_P(length_value) ? binary_string_from(std::string()) : Qnil);
    }

    if (NIL_P(length_value))
    {
      const std::size_t length = body_length - wrapper.read_offset;
      VALUE data = binary_string_from(preloaded_body_data(wrapper) + wrapper.read_offset, length);
      wrapper.read_offset = body_length;
      return replace_outbuf(outbuf, data);
    }

    const std::size_t unread = body_length - wrapper.read_offset;
    const std::size_t length = std::min<std::size_t>(unread, static_cast<std::size_t>(NUM2LONG(length_value)));
    VALUE data = binary_string_from(preloaded_body_data(wrapper) + wrapper.read_offset, length);
    wrapper.read_offset += length;
    return replace_outbuf(outbuf, data);
  }

  VALUE gets_preloaded_native_input(NativeInputWrapper &wrapper, const std::string &separator)
  {
    const std::size_t body_length = preloaded_body_length(wrapper);
    if (wrapper.read_offset >= body_length)
    {
      return Qnil;
    }

    const char *data = preloaded_body_data(wrapper) + wrapper.read_offset;
    const std::size_t unread = body_length - wrapper.read_offset;
    const char *separator_position = nullptr;
    if (separator.size() == 1)
    {
      separator_position = static_cast<const char *>(std::memchr(data, separator[0], unread));
    }
    else
    {
      const char *end = data + unread;
      const auto found = std::search(data, end, separator.begin(), separator.end());
      if (found != end)
      {
        separator_position = found;
      }
    }

    const std::size_t length = separator_position == nullptr
                                   ? unread
                                   : static_cast<std::size_t>(separator_position - data) + separator.size();
    VALUE result = binary_string_from(data, length);
    wrapper.read_offset += length;
    return result;
  }

  void ensure_open_locked(const Vajra::rack::NativeInputState &state)
  {
    if (state.closed)
    {
      throw std::runtime_error("rack.input is closed");
    }
  }

  void ensure_writable_locked(const Vajra::rack::NativeInputState &state)
  {
    ensure_open_locked(state);
    if (state.finished)
    {
      throw std::runtime_error("rack.input is already finished");
    }
    if (!state.error_message.empty())
    {
      throw std::runtime_error(state.error_message);
    }
  }

  std::size_t available_locked(const Vajra::rack::NativeInputState &state)
  {
    return state.bytes_written - state.read_offset;
  }

  bool wait_predicate_locked(const WaitContext &context)
  {
    const Vajra::rack::NativeInputState &state = *context.state;
    if (state.closed || !state.error_message.empty() || state.finished)
    {
      return true;
    }

    switch (context.predicate)
    {
    case WaitContext::Predicate::data:
      return available_locked(state) > 0;
    case WaitContext::Predicate::capacity:
      return available_locked(state) < state.high_watermark;
    }
    return true;
  }

  void *wait_without_gvl(void *data)
  {
    auto *context = static_cast<WaitContext *>(data);
    std::unique_lock<std::mutex> lock(context->state->mutex);
    if (context->predicate == WaitContext::Predicate::capacity)
    {
      context->state->capacity_condition.wait(lock, [context]()
                                              { return wait_predicate_locked(*context); });
    }
    else
    {
      context->state->data_condition.wait(lock, [context]()
                                          { return wait_predicate_locked(*context); });
    }
    return nullptr;
  }

  void wait_for_data(Vajra::rack::NativeInputState &state)
  {
    WaitContext context{&state, WaitContext::Predicate::data};
    rb_thread_call_without_gvl(wait_without_gvl, &context, nullptr, nullptr);
  }

  void wait_for_capacity_native(Vajra::rack::NativeInputState &state)
  {
    WaitContext context{&state, WaitContext::Predicate::capacity};
    wait_without_gvl(&context);
  }

  void open_spill_locked(Vajra::rack::NativeInputState &state)
  {
    if (state.spill_file != nullptr)
    {
      return;
    }

    state.spill_file = std::tmpfile();
    if (state.spill_file == nullptr)
    {
      throw std::runtime_error(std::string("unable to create native rack.input spill file: ") + std::strerror(errno));
    }

    if (state.memory_bytes > 0)
    {
      for (const std::string &chunk : state.memory_chunks)
      {
        if (std::fwrite(chunk.data(), 1, chunk.size(), state.spill_file) != chunk.size())
        {
          throw std::runtime_error("unable to write native rack.input spill file from chunks");
        }
        state.spilled_bytes += chunk.size();
      }
      state.memory_chunks.clear();
      state.memory_chunks.shrink_to_fit();
      state.memory_base_offset = state.bytes_written;
      state.memory_bytes = 0;
    }
  }

  void append_memory_locked(Vajra::rack::NativeInputState &state, const char *data, std::size_t length)
  {
    std::size_t offset = 0;
    while (offset < length)
    {
      const std::size_t slice = std::min<std::size_t>(kNativeInputChunkBytes, length - offset);
      state.memory_chunks.emplace_back(data + offset, slice);
      state.memory_bytes += slice;
      offset += slice;
    }
  }

  void write_locked(Vajra::rack::NativeInputState &state, const char *data, std::size_t length)
  {
    if (length == 0)
    {
      return;
    }

    if (state.spill_file != nullptr || state.memory_bytes + length > state.memory_limit)
    {
      open_spill_locked(state);
      if (std::fseek(state.spill_file, static_cast<long>(state.bytes_written), SEEK_SET) != 0)
      {
        throw std::runtime_error("unable to seek native rack.input spill file for write");
      }
      if (std::fwrite(data, 1, length, state.spill_file) != length)
      {
        throw std::runtime_error("unable to write native rack.input spill file");
      }
      state.spilled_bytes += length;
    }
    else
    {
      append_memory_locked(state, data, length);
    }
    state.bytes_written += length;
  }

  void preserve_and_prune_consumed_locked(Vajra::rack::NativeInputState &state)
  {
    if (state.spill_file != nullptr || state.memory_chunks.empty())
    {
      return;
    }
    if (state.finished || state.read_offset < kNativeInputPruneThresholdBytes)
    {
      return;
    }
    if (state.memory_bytes < kNativeInputPruneThresholdBytes)
    {
      return;
    }

    const std::size_t pruned = state.memory_bytes;
    open_spill_locked(state);
    state.pruned_bytes += pruned;
  }

  std::optional<std::string> pull_to_string_locked(Vajra::rack::NativeInputState &state, std::size_t length)
  {
    const std::size_t readable = std::min(length, available_locked(state));
    if (readable == 0)
    {
      return std::nullopt;
    }

    std::string result(readable, '\0');
    char *dest = result.data();

    if (state.spill_file != nullptr)
    {
      if (std::fseek(state.spill_file, static_cast<long>(state.read_offset), SEEK_SET) != 0)
      {
        throw std::runtime_error("unable to seek native rack.input spill file for read");
      }
      const std::size_t read = std::fread(dest, 1, readable, state.spill_file);
      if (read != readable)
      {
        result.resize(read);
      }
    }
    else
    {
      std::size_t offset = state.read_offset;
      std::size_t remaining = readable;
      std::size_t chunk_start = state.memory_base_offset;
      for (const std::string &chunk : state.memory_chunks)
      {
        const std::size_t chunk_end = chunk_start + chunk.size();
        if (offset < chunk_end)
        {
          const std::size_t local_offset = offset - chunk_start;
          const std::size_t slice = std::min<std::size_t>(chunk.size() - local_offset, remaining);
          std::memcpy(dest + (readable - remaining), chunk.data() + local_offset, slice);
          remaining -= slice;
          offset += slice;
          if (remaining == 0)
          {
            break;
          }
        }
        chunk_start = chunk_end;
      }
    }

    state.read_offset += result.size();
    state.consumed_since_last_observation += result.size();
    preserve_and_prune_consumed_locked(state);
    state.capacity_condition.notify_all();
    return result;
  }

  std::optional<std::size_t> find_separator_read_length_locked(
      Vajra::rack::NativeInputState &state,
      std::size_t offset,
      std::size_t length,
      const std::string &separator)
  {
    if (length == 0 || separator.empty())
    {
      return std::nullopt;
    }

    if (state.spill_file != nullptr)
    {
      if (std::fseek(state.spill_file, static_cast<long>(offset), SEEK_SET) != 0)
      {
        throw std::runtime_error("unable to seek native rack.input spill file for separator scan");
      }

      char buffer[kNativeInputChunkBytes];
      std::size_t scanned = 0;
      std::size_t matched = 0;
      while (scanned < length)
      {
        const std::size_t requested = std::min<std::size_t>(sizeof(buffer), length - scanned);
        const std::size_t read = std::fread(buffer, 1, requested, state.spill_file);
        if (read == 0)
        {
          return std::nullopt;
        }
        if (separator.size() == 1)
        {
          const void *found = std::memchr(buffer, separator[0], read);
          if (found != nullptr)
          {
            return scanned + static_cast<std::size_t>(static_cast<const char *>(found) - buffer) + 1;
          }
        }
        else
        {
          for (std::size_t index = 0; index < read; ++index)
          {
            if (buffer[index] == separator[matched])
            {
              ++matched;
              if (matched == separator.size())
              {
                return scanned + index + 1;
              }
            }
            else
            {
              matched = buffer[index] == separator[0] ? 1 : 0;
            }
          }
        }
        scanned += read;
      }
      return std::nullopt;
    }

    std::size_t chunk_start = state.memory_base_offset;
    std::size_t remaining = length;
    std::size_t scanned = 0;
    std::size_t matched = 0;
    for (const std::string &chunk : state.memory_chunks)
    {
      const std::size_t chunk_end = chunk_start + chunk.size();
      if (offset < chunk_end)
      {
        const std::size_t local_offset = offset - chunk_start;
        const std::size_t slice = std::min<std::size_t>(chunk.size() - local_offset, remaining);
        const char *slice_data = chunk.data() + local_offset;
        if (separator.size() == 1)
        {
          const void *found = std::memchr(slice_data, separator[0], slice);
          if (found != nullptr)
          {
            return scanned + static_cast<std::size_t>(static_cast<const char *>(found) - slice_data) + 1;
          }
        }
        else
        {
          for (std::size_t index = 0; index < slice; ++index)
          {
            if (slice_data[index] == separator[matched])
            {
              ++matched;
              if (matched == separator.size())
              {
                return scanned + index + 1;
              }
            }
            else
            {
              matched = slice_data[index] == separator[0] ? 1 : 0;
            }
          }
        }
        remaining -= slice;
        offset += slice;
        scanned += slice;
        if (remaining == 0)
        {
          break;
        }
      }
      chunk_start = chunk_end;
    }
    return std::nullopt;
  }

  VALUE native_input_from_string(VALUE klass, VALUE body)
  {
    (void)klass;
    StringValue(body);
    if (RSTRING_LEN(body) == 0)
    {
      return preloaded_native_input();
    }
    return preloaded_native_input(std::string(RSTRING_PTR(body), static_cast<std::size_t>(RSTRING_LEN(body))));
  }

  VALUE native_input_initialize(int argc, VALUE *argv, VALUE self)
  {
    VALUE options = Qnil;
    rb_scan_args(argc, argv, "0:", &options);
    if (!NIL_P(options))
    {
      ID memory_limit_id = rb_intern("memory_limit");
      VALUE memory_limit = rb_hash_lookup(options, ID2SYM(memory_limit_id));
      if (!NIL_P(memory_limit))
      {
        auto state = native_input_state_from_ruby(self);
        state->memory_limit = NUM2SIZET(memory_limit);
        state->high_watermark = std::max<std::size_t>(state->memory_limit, 64 * 1024);
        state->low_watermark = state->high_watermark / 2;
      }
    }
    return self;
  }

  VALUE native_input_read(int argc, VALUE *argv, VALUE self)
  {
    try
    {
      VALUE length_value = Qnil;
      VALUE outbuf = Qnil;
      rb_scan_args(argc, argv, "02", &length_value, &outbuf);

      if (!NIL_P(length_value))
      {
        const long requested_length = NUM2LONG(length_value);
        if (requested_length < 0)
        {
          rb_raise(rb_eArgError, "negative length");
        }
        if (requested_length == 0)
        {
          return replace_outbuf(outbuf, binary_string_from(std::string()));
        }
      }

      auto wrapper = native_input_wrapper_from(self);
      if (!wrapper->state)
      {
        return read_preloaded_native_input(*wrapper, length_value, outbuf);
      }
      auto state = wrapper->state;

      if (NIL_P(length_value))
      {
        VALUE result = rb_str_new(nullptr, 0);
        rb_enc_associate_index(result, rb_ascii8bit_encindex());
        for (;;)
        {
          std::optional<std::string> chunk_data;
          bool finished = false;
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            ensure_open_locked(*state);
            if (!state->error_message.empty())
            {
              throw std::runtime_error(state->error_message);
            }
            chunk_data = pull_to_string_locked(*state, available_locked(*state));
            finished = state->finished;
          }

          if (chunk_data)
          {
            VALUE chunk = binary_string_from(*chunk_data);
            rb_str_concat(result, chunk);
          }

          if (finished && !chunk_data)
          {
            {
              std::lock_guard<std::mutex> lock(state->mutex);
              state->eof_observed = true;
            }
            return replace_outbuf(outbuf, result);
          }

          if (!chunk_data)
          {
            wait_for_data(*state);
          }
        }
      }

      const std::size_t total_requested = static_cast<std::size_t>(NUM2LONG(length_value));
      std::size_t total_read = 0;
      VALUE result = Qnil;

      for (;;)
      {
        std::optional<std::string> chunk_data;
        bool finished = false;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          ensure_open_locked(*state);
          if (!state->error_message.empty())
          {
            throw std::runtime_error(state->error_message);
          }

          const std::size_t available = available_locked(*state);
          if (available > 0)
          {
            chunk_data = pull_to_string_locked(*state, std::min(available, total_requested - total_read));
          }
          finished = state->finished;
        }

        if (chunk_data)
        {
          VALUE chunk = binary_string_from(*chunk_data);
          if (NIL_P(result))
          {
            result = chunk;
          }
          else
          {
            rb_str_concat(result, chunk);
          }
          total_read += chunk_data->size();
        }

        if (total_read == total_requested || (finished && total_read > 0))
        {
          return replace_outbuf(outbuf, result);
        }

        if (finished && total_read == 0)
        {
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->eof_observed = true;
          }
          return replace_outbuf(outbuf, Qnil);
        }

        if (!chunk_data)
        {
          wait_for_data(*state);
        }
      }
    }
    catch (const std::runtime_error &error)
    {
      raise_io_error(error.what());
    }
    return Qnil;
  }

  VALUE native_input_gets(int argc, VALUE *argv, VALUE self)
  {
    try
    {
      VALUE separator_value = Qnil;
      rb_scan_args(argc, argv, "01", &separator_value);
      auto wrapper = native_input_wrapper_from(self);

      if (argc == 0)
      {
        separator_value = rb_str_new_cstr("\n");
      }
      if (NIL_P(separator_value))
      {
        VALUE arguments[] = {Qnil};
        return native_input_read(1, arguments, self);
      }

      StringValue(separator_value);
      const std::string separator(RSTRING_PTR(separator_value), static_cast<std::size_t>(RSTRING_LEN(separator_value)));
      if (separator.empty())
      {
        rb_raise(rb_eArgError, "separator cannot be empty");
      }
      if (!wrapper->state)
      {
        return gets_preloaded_native_input(*wrapper, separator);
      }

      auto state = wrapper->state;
      VALUE result = Qnil;

      for (;;)
      {
        std::optional<std::string> chunk_data;
        bool finished = false;
        bool found_separator = false;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          ensure_open_locked(*state);
          if (!state->error_message.empty())
          {
            throw std::runtime_error(state->error_message);
          }

          const std::size_t available = available_locked(*state);
          if (available > 0)
          {
            const std::optional<std::size_t> separator_read_length =
                find_separator_read_length_locked(*state, state->read_offset, available, separator);
            if (separator_read_length.has_value())
            {
              chunk_data = pull_to_string_locked(*state, *separator_read_length);
              found_separator = true;
            }
            else
            {
              chunk_data = pull_to_string_locked(*state, available);
            }
          }
          finished = state->finished;
        }

        if (chunk_data)
        {
          VALUE chunk = binary_string_from(*chunk_data);
          if (NIL_P(result))
          {
            result = chunk;
          }
          else
          {
            rb_str_concat(result, chunk);
          }
        }

        if (found_separator || (finished && !chunk_data))
        {
          if (finished && !chunk_data)
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->eof_observed = true;
          }
          return result;
        }

        if (!chunk_data)
        {
          wait_for_data(*state);
        }
      }
    }
    catch (const std::runtime_error &error)
    {
      raise_io_error(error.what());
    }
    return Qnil;
  }

  VALUE native_input_each(VALUE self)
  {
    RETURN_ENUMERATOR(self, 0, nullptr);
    for (;;)
    {
      VALUE line = native_input_gets(0, nullptr, self);
      if (NIL_P(line))
      {
        break;
      }
      rb_yield(line);
    }
    return self;
  }

  VALUE native_input_rewind(VALUE self)
  {
    try
    {
      auto wrapper = native_input_wrapper_from(self);
      if (!wrapper->state)
      {
        wrapper->read_offset = 0;
        return INT2FIX(0);
      }
      auto state = wrapper->state;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        ensure_open_locked(*state);
        if (!state->error_message.empty())
        {
          throw std::runtime_error(state->error_message);
        }
        state->read_offset = 0;
        state->eof_observed = false;
        state->capacity_condition.notify_all();
        return INT2FIX(0);
      }
    }
    catch (const std::runtime_error &error)
    {
      raise_io_error(error.what());
    }
    return Qnil;
  }

  VALUE native_input_close_method(VALUE self)
  {
    NativeInputWrapper *wrapper = nullptr;
    TypedData_Get_Struct(self, NativeInputWrapper, &native_input_type, wrapper);
    if (wrapper == nullptr || wrapper->closed)
    {
      return Qnil;
    }
    if (wrapper->shared_empty)
    {
      return Qnil;
    }
    if (wrapper->state)
    {
      Vajra::rack::native_input_close(wrapper->state.get());
    }
    delete wrapper->body;
    wrapper->body = nullptr;
    wrapper->read_offset = 0;
    wrapper->closed = true;
    return Qnil;
  }

  VALUE native_input_external_encoding(VALUE)
  {
    return rb_enc_from_encoding(rb_ascii8bit_encoding());
  }
}

void Vajra::rack::initialize_native_input()
{
  VALUE mVajra = rb_define_module("Vajra");
  rb_global_variable(&rb_empty_native_input);
  rb_cNativeInput = rb_define_class_under(mVajra, "NativeInput", rb_cObject);
  rb_define_alloc_func(rb_cNativeInput, native_input_allocate);
  rb_define_singleton_method(rb_cNativeInput, "from_string", RUBY_METHOD_FUNC(native_input_from_string), 1);
  rb_define_method(rb_cNativeInput, "initialize", RUBY_METHOD_FUNC(native_input_initialize), -1);
  rb_define_method(rb_cNativeInput, "read", RUBY_METHOD_FUNC(native_input_read), -1);
  rb_define_method(rb_cNativeInput, "gets", RUBY_METHOD_FUNC(native_input_gets), -1);
  rb_define_method(rb_cNativeInput, "each", RUBY_METHOD_FUNC(native_input_each), 0);
  rb_define_method(rb_cNativeInput, "rewind", RUBY_METHOD_FUNC(native_input_rewind), 0);
  rb_define_method(rb_cNativeInput, "close", RUBY_METHOD_FUNC(native_input_close_method), 0);
  rb_define_method(rb_cNativeInput, "external_encoding", RUBY_METHOD_FUNC(native_input_external_encoding), 0);
}

Vajra::rack::NativeInputHandle Vajra::rack::create_native_input()
{
  return create_native_input(create_native_input_state());
}

std::shared_ptr<Vajra::rack::NativeInputState> Vajra::rack::create_native_input_state()
{
  return std::make_shared<Vajra::rack::NativeInputState>();
}

Vajra::rack::NativeInputHandle Vajra::rack::create_native_input(std::shared_ptr<NativeInputState> state)
{
  if (!state)
  {
    throw std::runtime_error("native rack.input state is missing");
  }
  auto *wrapper = new NativeInputWrapper;
  wrapper->state = std::move(state);
  VALUE input = native_input_wrap(rb_cNativeInput, wrapper);
  return NativeInputHandle{input, native_input_state_from(input), wrapper->state};
}

Vajra::rack::NativeInputHandle Vajra::rack::create_native_input_from_body(std::string body)
{
  VALUE input = body.empty() ? shared_empty_native_input() : preloaded_native_input(std::move(body));
  return NativeInputHandle{input, nullptr, nullptr};
}

bool Vajra::rack::native_input_p(VALUE value)
{
  return !NIL_P(value) && rb_obj_is_kind_of(value, rb_cNativeInput) == Qtrue;
}

Vajra::rack::NativeInputState *Vajra::rack::native_input_state_from(VALUE value)
{
  if (!native_input_p(value))
  {
    rb_raise(rb_eTypeError, "expected Vajra::NativeInput");
  }
  return native_input_state_from_ruby(value);
}

void Vajra::rack::native_input_append(NativeInputState *state, const char *data, std::size_t length)
{
  if (state == nullptr)
  {
    throw std::runtime_error("native rack.input state is missing");
  }

  std::size_t offset = 0;
  while (offset < length)
  {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      ensure_writable_locked(*state);
      const std::size_t available = available_locked(*state);
      if (available < state->high_watermark)
      {
        const std::size_t capacity = state->high_watermark - available;
        const std::size_t slice = std::min(capacity, length - offset);
        write_locked(*state, data + offset, slice);
        offset += slice;
        state->data_condition.notify_all();
        continue;
      }
      ++state->watermark_wait_count;
    }
    wait_for_capacity_native(*state);
  }
}

void Vajra::rack::native_input_append(NativeInputState *state, const std::string &chunk)
{
  native_input_append(state, chunk.data(), chunk.size());
}

bool Vajra::rack::native_input_try_append(NativeInputState *state, const char *data, std::size_t length)
{
  if (state == nullptr)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed || state->finished || !state->error_message.empty())
  {
    return false;
  }

  const std::size_t available = available_locked(*state);
  if (available >= state->high_watermark || length > state->high_watermark - available)
  {
    return false;
  }

  write_locked(*state, data, length);
  state->data_condition.notify_all();
  return true;
}

void Vajra::rack::native_input_finish(NativeInputState *state)
{
  if (state == nullptr)
  {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->closed)
    {
      state->finished = true;
    }
  }
  state->data_condition.notify_all();
  state->capacity_condition.notify_all();
}

void Vajra::rack::native_input_fail(NativeInputState *state, const std::string &message)
{
  if (state == nullptr)
  {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->closed)
    {
      state->error_message = message;
      state->finished = true;
    }
  }
  state->data_condition.notify_all();
  state->capacity_condition.notify_all();
}

void Vajra::rack::native_input_close(NativeInputState *state)
{
  if (state == nullptr)
  {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->finished = true;
    state->memory_chunks.clear();
    state->memory_chunks.shrink_to_fit();
    state->memory_base_offset = state->bytes_written;
    state->memory_bytes = 0;
    if (state->spill_file != nullptr)
    {
      std::fclose(state->spill_file);
      state->spill_file = nullptr;
    }
  }
  state->data_condition.notify_all();
  state->capacity_condition.notify_all();
}

bool Vajra::rack::native_input_closed(NativeInputState *state)
{
  if (state == nullptr)
  {
    return true;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->closed;
}

bool Vajra::rack::native_input_finished(NativeInputState *state)
{
  if (state == nullptr)
  {
    return true;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->finished || state->closed;
}

bool Vajra::rack::native_input_fully_consumed(VALUE value)
{
  if (!native_input_p(value))
  {
    return true;
  }

  NativeInputWrapper *wrapper = nullptr;
  TypedData_Get_Struct(value, NativeInputWrapper, &native_input_type, wrapper);
  if (wrapper == nullptr)
  {
    return true;
  }
  if (!wrapper->state)
  {
    return wrapper->body == nullptr || wrapper->read_offset >= wrapper->body->size();
  }

  const auto state = wrapper->state;
  const std::lock_guard<std::mutex> lock(state->mutex);
  return (state->finished || state->closed) && state->read_offset >= state->bytes_written;
}

bool Vajra::rack::native_input_fully_consumed(NativeInputState *state)
{
  if (state == nullptr)
  {
    return true;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return (state->finished || state->closed) && state->read_offset >= state->bytes_written;
}

bool Vajra::rack::native_input_consumed_at_least(NativeInputState *state, std::size_t bytes)
{
  if (state == nullptr)
  {
    return bytes == 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return (state->finished || state->closed) && state->read_offset >= bytes;
}

bool Vajra::rack::native_input_eof_observed(NativeInputState *state)
{
  if (state == nullptr)
  {
    return true;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->eof_observed;
}

bool Vajra::rack::native_input_above_high_watermark(NativeInputState *state)
{
  if (state == nullptr)
  {
    return false;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return available_locked(*state) >= state->high_watermark;
}

bool Vajra::rack::native_input_below_low_watermark(NativeInputState *state)
{
  if (state == nullptr)
  {
    return true;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return available_locked(*state) <= state->low_watermark;
}

std::size_t Vajra::rack::native_input_buffered_bytes(NativeInputState *state)
{
  if (state == nullptr)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return available_locked(*state);
}

std::size_t Vajra::rack::native_input_pruned_bytes(NativeInputState *state)
{
  if (state == nullptr)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->pruned_bytes;
}

std::size_t Vajra::rack::native_input_spilled_bytes(NativeInputState *state)
{
  if (state == nullptr)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->spilled_bytes;
}

std::size_t Vajra::rack::native_input_watermark_wait_count(NativeInputState *state)
{
  if (state == nullptr)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  return state->watermark_wait_count;
}

std::size_t Vajra::rack::native_input_take_consumed_bytes(NativeInputState *state)
{
  if (state == nullptr)
  {
    return 0;
  }
  const std::lock_guard<std::mutex> lock(state->mutex);
  const std::size_t consumed = state->consumed_since_last_observation;
  state->consumed_since_last_observation = 0;
  return consumed;
}
