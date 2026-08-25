// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef VAJRA_SPEC_FUZZ_CONNECTION_HPP
#define VAJRA_SPEC_FUZZ_CONNECTION_HPP

#include "transport/connection.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace VajraSpecFuzz
{
  class BufferedConnection final : public Vajra::transport::Connection
  {
  public:
    explicit BufferedConnection(std::string input = "") : input_(std::move(input)) {}

    Vajra::platform::SocketHandle fd() const override { return Vajra::platform::kInvalidSocket; }
    bool wait_readable(int) override { return read_offset_ < input_.size(); }

    Vajra::platform::SignedSize read(char *buffer, std::size_t length) override
    {
      if (read_offset_ >= input_.size())
      {
        return 0;
      }
      const std::size_t copied = std::min(length, input_.size() - read_offset_);
      std::memcpy(buffer, input_.data() + read_offset_, copied);
      read_offset_ += copied;
      return static_cast<Vajra::platform::SignedSize>(copied);
    }

    Vajra::platform::SignedSize write(const char *buffer, std::size_t length) override
    {
      output_.append(buffer, length);
      return static_cast<Vajra::platform::SignedSize>(length);
    }

    std::string protocol() const override { return "http/1.1"; }
    bool tls() const override { return false; }

  private:
    std::string input_;
    std::size_t read_offset_ = 0;
    std::string output_;
  };
}

#endif
