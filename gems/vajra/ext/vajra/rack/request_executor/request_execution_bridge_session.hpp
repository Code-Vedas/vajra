// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

class RequestExecutionBridgeSession final : public Vajra::request::RequestExecutionSession
{
public:
  explicit RequestExecutionBridgeSession(std::unique_ptr<Vajra::rack::RackExecutionSession> session)
      : session_(std::move(session))
  {
  }

  void append_request_body_bytes(const char *data, std::size_t length) override
  {
    session_->append_request_body_bytes(data, length);
  }

  bool try_append_request_body_bytes(const char *data, std::size_t length) override
  {
    return session_->try_append_request_body_bytes(data, length);
  }

  Vajra::rack::NativeInputState *native_input_state() override
  {
    return session_->native_input_state();
  }

  std::shared_ptr<Vajra::rack::NativeInputState> native_input_state_owner() override
  {
    return session_->native_input_state_owner();
  }

  void finish_request_body() override
  {
    session_->finish_request_body();
  }

  void fail_request_body(const std::string &message) noexcept override
  {
    session_->fail_request_body(message);
  }

  std::optional<Vajra::response::Response> finish() override
  {
    return session_->finish();
  }

private:
  std::unique_ptr<Vajra::rack::RackExecutionSession> session_;
};
