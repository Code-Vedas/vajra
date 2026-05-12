// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "rack_env.hpp"

#include "request_head_error.hpp"

#include <cctype>
#include <utility>

namespace
{
  constexpr std::size_t kFixedRackEnvEntryCount = 10;

  bool valid_header_name_character(unsigned char character)
  {
    if ((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z'))
    {
      return true;
    }

    switch (character)
    {
      case '!':
      case '#':
      case '$':
      case '%':
      case '&':
      case '\'':
      case '*':
      case '+':
      case '-':
      case '.':
      case '^':
      case '`':
      case '|':
      case '~':
        return true;
      default:
        return false;
    }
  }

  bool valid_header_value_character(unsigned char character)
  {
    return character == '\t' || (character >= 0x20 && character != 0x7F);
  }

  std::string normalize_header_env_key(const std::string &header_name)
  {
    std::string normalized;
    normalized.reserve(header_name.size());

    for (const unsigned char character : header_name)
    {
      if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9'))
      {
        normalized.push_back(static_cast<char>(std::toupper(character)));
        continue;
      }

      if (character == '_')
      {
        throw Vajra::request::bad_request_error(
            "request header names must not contain underscores for Rack environment translation");
      }

      if (!valid_header_name_character(character))
      {
        throw Vajra::request::bad_request_error("invalid request header name for Rack environment");
      }

      if (character == '-')
      {
        normalized.push_back('_');
        continue;
      }

      throw Vajra::request::bad_request_error("invalid request header name for Rack environment");
    }

    if (normalized.empty())
    {
      throw Vajra::request::bad_request_error("missing request header name for Rack environment");
    }

    if (normalized == "CONTENT_TYPE" || normalized == "CONTENT_LENGTH")
    {
      return normalized;
    }

    return "HTTP_" + normalized;
  }

  void validate_header_value(const std::string &value)
  {
    for (const unsigned char character : value)
    {
      if (!valid_header_value_character(character))
      {
        throw Vajra::request::bad_request_error("invalid request header value for Rack environment");
      }
    }
  }

  void insert_or_append_header(
      std::vector<Vajra::request::RackEnvEntry> &entries,
      const std::string &key,
      const std::string &value)
  {
    for (Vajra::request::RackEnvEntry &entry : entries)
    {
      if (entry.key == key)
      {
        if (key == "CONTENT_LENGTH" || key == "CONTENT_TYPE")
        {
          throw Vajra::request::bad_request_error("duplicate Rack CGI header is not allowed");
        }

        entry.value += (key == "HTTP_COOKIE" ? "; " : ",") + value;
        return;
      }
    }

    entries.push_back(Vajra::request::RackEnvEntry{key, value});
  }
}

std::vector<Vajra::request::RackEnvEntry> Vajra::request::RackEnvBuilder::build(
    const RequestContext &request_context) const
{
  const RackRequestTarget request_target = split_target(request_context.request.request_line.target);

  std::vector<RackEnvEntry> entries;
  entries.reserve(request_context.request.headers.size() + kFixedRackEnvEntryCount);
  entries.push_back(RackEnvEntry{"REQUEST_METHOD", request_context.request.request_line.method});
  entries.push_back(RackEnvEntry{"SCRIPT_NAME", ""});
  entries.push_back(RackEnvEntry{"PATH_INFO", request_target.path_info});
  entries.push_back(RackEnvEntry{"QUERY_STRING", request_target.query_string});
  entries.push_back(RackEnvEntry{"SERVER_PROTOCOL", request_context.request.request_line.version});
  entries.push_back(RackEnvEntry{"SERVER_NAME", request_context.socket.server_name});
  entries.push_back(RackEnvEntry{"SERVER_PORT", std::to_string(request_context.socket.server_port)});
  entries.push_back(RackEnvEntry{"REMOTE_ADDR", request_context.socket.remote_address});
  entries.push_back(RackEnvEntry{"REMOTE_PORT", std::to_string(request_context.socket.remote_port)});
  entries.push_back(RackEnvEntry{"rack.url_scheme", request_context.socket.scheme});

  for (const ParsedHeader &header : request_context.request.headers)
  {
    validate_header_value(header.value);
    insert_or_append_header(entries, normalize_header_env_key(header.name), header.value);
  }

  return entries;
}

Vajra::request::RackRequestTarget Vajra::request::RackEnvBuilder::split_target(const std::string &target) const
{
  if (target.empty() || target[0] != '/')
  {
    throw bad_request_error("request target must use an absolute path for Rack environment translation");
  }

  const std::size_t query_delimiter = target.find('?');
  if (query_delimiter == std::string::npos)
  {
    return RackRequestTarget{target, ""};
  }

  return RackRequestTarget{
      target.substr(0, query_delimiter),
      target.substr(query_delimiter + 1)};
}
