// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response_serializer.hpp"

#include <cctype>

namespace
{
  bool contains_http_control_bytes(const std::string &value)
  {
    for (const unsigned char character : value)
    {
      if (character == '\r' || character == '\n')
      {
        return true;
      }
    }

    return false;
  }
}

std::string Vajra::response::ResponseSerializer::serialize(const Response &response) const
{
  validate_status(response.status);

  std::string serialized =
      "HTTP/1.1 " + std::to_string(response.status.code) + " " + response.status.reason_phrase + "\r\n";

  for (const Header &header : response.headers)
  {
    validate_header(header);
    serialized += header.name + ": " + header.value + "\r\n";
  }

  serialized += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  serialized += "Connection: close\r\n";
  serialized += "\r\n";
  serialized += response.body;

  return serialized;
}

void Vajra::response::ResponseSerializer::validate_status(const Status &status) const
{
  if (status.code < 100 || status.code > 999)
  {
    throw SerializationError("response status code must be a three-digit HTTP status");
  }

  if (status.reason_phrase.empty())
  {
    throw SerializationError("response reason phrase must not be empty");
  }

  if (contains_http_control_bytes(status.reason_phrase))
  {
    throw SerializationError("response reason phrase contains an unsafe control character");
  }
}

void Vajra::response::ResponseSerializer::validate_header(const Header &header) const
{
  if (header.name.empty())
  {
    throw SerializationError("response header name must not be empty");
  }

  for (const unsigned char character : header.name)
  {
    if (character == ':' || std::iscntrl(character) != 0 || std::isspace(character) != 0)
    {
      throw SerializationError("response header name contains an unsafe character");
    }
  }

  if (contains_http_control_bytes(header.value))
  {
    throw SerializationError("response header value contains an unsafe control character");
  }
}
