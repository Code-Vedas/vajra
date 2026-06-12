// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_RACK_NATIVE_INPUT_HPP
#define VAJRA_RACK_NATIVE_INPUT_HPP

#ifdef VAJRA_RUNTIME_TESTING
#ifndef VAJRA_RUBY_VALUE_STUB
#define VAJRA_RUBY_VALUE_STUB
using VALUE = unsigned long;
constexpr VALUE Qnil = 0;
#endif
#else
#include "ruby.h"
#endif

#include <cstddef>
#include <memory>
#include <string>

namespace Vajra
{
  namespace rack
  {
    struct NativeInputState;

    struct NativeInputHandle
    {
      VALUE value = Qnil;
      NativeInputState *state = nullptr;
      std::shared_ptr<NativeInputState> state_owner;
    };

    void initialize_native_input();
    std::shared_ptr<NativeInputState> create_native_input_state();
    NativeInputHandle create_native_input();
    NativeInputHandle create_native_input(std::shared_ptr<NativeInputState> state);
    NativeInputHandle create_native_input_from_body(std::string body);
    bool native_input_p(VALUE value);
    NativeInputState *native_input_state_from(VALUE value);
    void native_input_append(NativeInputState *state, const char *data, std::size_t length);
    void native_input_append(NativeInputState *state, const std::string &chunk);
    bool native_input_try_append(NativeInputState *state, const char *data, std::size_t length);
    void native_input_finish(NativeInputState *state);
    void native_input_fail(NativeInputState *state, const std::string &message);
    void native_input_close(NativeInputState *state);
#ifdef VAJRA_RUNTIME_TESTING
    inline bool native_input_closed(NativeInputState *)
    {
      return false;
    }
#else
    bool native_input_closed(NativeInputState *state);
#endif
    bool native_input_finished(NativeInputState *state);
#ifdef VAJRA_RUNTIME_TESTING
    inline bool native_input_fully_consumed(NativeInputState *)
    {
      return true;
    }
#else
    bool native_input_fully_consumed(NativeInputState *state);
#endif
    bool native_input_consumed_at_least(NativeInputState *state, std::size_t bytes);
    bool native_input_eof_observed(NativeInputState *state);
    bool native_input_fully_consumed(VALUE value);
    bool native_input_above_high_watermark(NativeInputState *state);
    bool native_input_below_low_watermark(NativeInputState *state);
    std::size_t native_input_buffered_bytes(NativeInputState *state);
    std::size_t native_input_pruned_bytes(NativeInputState *state);
    std::size_t native_input_spilled_bytes(NativeInputState *state);
    std::size_t native_input_watermark_wait_count(NativeInputState *state);
#ifdef VAJRA_RUNTIME_TESTING
    inline std::size_t native_input_take_consumed_bytes(NativeInputState *)
    {
      return 0;
    }
#else
    std::size_t native_input_take_consumed_bytes(NativeInputState *state);
#endif
  }
}

#endif
