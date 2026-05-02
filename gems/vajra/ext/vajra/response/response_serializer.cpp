// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response_serializer.hpp"

#include <cctype>

namespace
{
  bool contains_invalid_http_text_bytes(const std::string &value)
  {
    for (const unsigned char character : value)
    {
      if (std::iscntrl(character) != 0 && character != '\t')
      {
        return true;
      }
    }

    return false;
  }

  std::string lowercase_header_name(const std::string &name)
  {
    std::string normalized;
    normalized.reserve(name.size());

    for (const unsigned char character : name)
    {
      normalized.push_back(static_cast<char>(std::tolower(character)));
    }

    return normalized;
  }

  bool forbidden_framing_header(const std::string &name)
  {
    const std::string normalized_name = lowercase_header_name(name);
    return normalized_name == "content-length" || normalized_name == "connection" ||
           normalized_name == "transfer-encoding";
  }

  bool forbids_message_body(int status_code)
  {
    return (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;
  }

  bool valid_header_name_character(unsigned char character)
  {
    if (character >= 0x80)
    {
      return false;
    }

    if (std::isalnum(character) != 0)
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
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
    }
  }
}

std::string Vajra::response::ResponseSerializer::serialize(const Response &response) const
{
  validate_response(response);

  std::string serialized =
      "HTTP/1.1 " + std::to_string(response.status.code) + " " + response.status.reason_phrase + "\r\n";

  for (const Header &header : response.headers)
  {
    validate_header(header);
    serialized += header.name + ": " + header.value + "\r\n";
  }

  if (!forbids_message_body(response.status.code))
  {
    serialized += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
  }
  serialized += "Connection: close\r\n";
  serialized += "\r\n";

  if (!forbids_message_body(response.status.code))
  {
    serialized += response.body;
  }

  return serialized;
}

void Vajra::response::ResponseSerializer::validate_response(const Response &response) const
{
  validate_status(response.status);

  for (const Header &header : response.headers)
  {
    validate_header(header);
  }

  if (forbids_message_body(response.status.code) && !response.body.empty())
  {
    throw SerializationError("response status must not include a message body");
  }
}

void Vajra::response::ResponseSerializer::validate_status(const Status &status) const
{
  if (status.code < 100 || status.code > 599)
  {
    throw SerializationError("response status code must be within the HTTP/1.1 range 100-599");
  }

  if (status.reason_phrase.empty())
  {
    throw SerializationError("response reason phrase must not be empty");
  }

  if (contains_invalid_http_text_bytes(status.reason_phrase))
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
    if (!valid_header_name_character(character))
    {
      throw SerializationError("response header name contains an unsafe character");
    }
  }

  if (forbidden_framing_header(header.name))
  {
    throw SerializationError("response headers must not override HTTP framing headers");
  }

  if (contains_invalid_http_text_bytes(header.value))
  {
    throw SerializationError("response header value contains an unsafe control character");
  }
}
