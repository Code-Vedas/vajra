// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_request_executor.hpp"
#include "rack/native_input.hpp"
#include "rack/ruby_rack_transport.hpp"

#include "request/http_field_utils.hpp"
#include "request/rack_env.hpp"
#include "runtime/runtime_state.hpp"
#include "ruby.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
  std::string request_path_for(const std::string &target)
  {
    const std::size_t delimiter = target.find('?');
    return target.substr(0, delimiter);
  }

#include "rack/request_executor/request_execution_bridge_session.hpp"
}

std::unique_ptr<Vajra::rack::RackExecutionSession> Vajra::rack::RackExecutionTransport::start(
    const std::vector<request::RackEnvEntry> &env_entries,
    int client_fd,
    std::shared_ptr<NativeHijackTransport> native_hijack_transport) const
{
  (void)env_entries;
  (void)client_fd;
  (void)native_hijack_transport;
  throw std::runtime_error("Rack execution transport does not support native streaming start");
}

std::string Vajra::rack::RackExecutionTransport::stats_payload_json() const
{
  return Vajra::runtime::runtime_stats_payload_json();
}

std::string Vajra::rack::RackExecutionTransport::metrics_payload_text() const
{
  return Vajra::runtime::runtime_metrics_payload_text();
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor()
    : transport_(same_process_rack_execution_transport())
{
}

Vajra::rack::RackRequestExecutor::RackRequestExecutor(
    std::shared_ptr<const RackExecutionTransport> transport,
    ControlPlaneConfig control_plane_config)
    : transport_(transport ? std::move(transport) : same_process_rack_execution_transport()),
      control_plane_config_(std::move(control_plane_config))
{
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::control_response(
    const request::RequestContext &request_context) const
{
  const std::string path = request_path_for(request_context.request.request_line.target);
  response::Response response;
  response.status = {200, "OK"};
  response.headers.push_back({"Cache-Control", "no-store"});

  if (!control_plane_config_.stats_path.empty() &&
      path == control_plane_config_.stats_path &&
      request_context.request.request_line.method == "GET")
  {
    response.headers.push_back({"Content-Type", "application/json"});
    response.body = transport_->stats_payload_json();
    return response;
  }

  if (!control_plane_config_.metrics_endpoint.empty() &&
      path == control_plane_config_.metrics_endpoint &&
      request_context.request.request_line.method == "GET")
  {
    response.headers.push_back({"Content-Type", "text/plain; version=0.0.4"});
    response.body = transport_->metrics_payload_text();
    return response;
  }

  return std::nullopt;
}

bool Vajra::rack::RackRequestExecutor::async_execution_supported() const
{
  return transport_->async_execution_supported();
}

bool Vajra::rack::RackRequestExecutor::async_completion_supported() const
{
  return transport_->async_completion_supported();
}

std::unique_ptr<Vajra::request::RequestExecutionSession> Vajra::rack::RackRequestExecutor::start(
    const request::RequestContext &request_context) const
{
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return std::make_unique<RequestExecutionBridgeSession>(
      transport_->start(
          env_entries,
          request_context.client_fd,
          request_context.native_hijack_transport));
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    const request::RequestContext &request_context) const
{
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return transport_->execute(
      env_entries,
      request_context.request_body,
      request_context.client_fd,
      request_context.http2_stream,
      request_context.native_hijack_transport);
}

std::optional<Vajra::response::Response> Vajra::rack::RackRequestExecutor::execute(
    request::RequestContext &&request_context) const
{
  request::RackEnvBuilder builder;
  const std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return transport_->execute(
      env_entries,
      std::move(request_context.request_body),
      request_context.client_fd,
      std::move(request_context.http2_stream),
      std::move(request_context.native_hijack_transport));
}

bool Vajra::rack::RackRequestExecutor::execute_async(
    request::RequestContext &&request_context,
    request::RequestExecutor::CompletionCallback callback) const
{
  request::RackEnvBuilder builder;
  std::vector<request::RackEnvEntry> env_entries = builder.build(request_context);
  return transport_->execute_async(
      std::move(env_entries),
      std::move(request_context.request_body),
      request_context.client_fd,
      std::move(request_context.http2_stream),
      std::move(request_context.native_hijack_transport),
      std::move(callback));
}
