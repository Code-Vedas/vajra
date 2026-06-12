// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "response_serializer.hpp"
#include "http_header_utils.hpp"

#include <array>

std::string Vajra::response::ResponseSerializer::serialize(const Response &response) const
{
  std::string serialized = serialize_head(response);
  if (status_forbids_message_body(response.status.code))
  {
    return serialized;
  }

  if (response_has_body_chunks(response))
  {
    for (const std::string &chunk : response.body_chunks)
    {
      serialized += chunk;
    }
    return serialized;
  }

  if (response_has_body_file(response))
  {
    if (std::fseek(response.body_file->file, 0, SEEK_SET) != 0)
    {
      throw SerializationError("response body file cannot be rewound");
    }
    std::array<char, 16 * 1024> buffer;
    while (true)
    {
      const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), response.body_file->file);
      if (read > 0)
      {
        serialized.append(buffer.data(), read);
      }
      if (read < buffer.size())
      {
        if (std::ferror(response.body_file->file) != 0)
        {
          throw SerializationError("response body file cannot be read");
        }
        break;
      }
    }
    return serialized;
  }

  serialized += response.body;
  return serialized;
}

std::string Vajra::response::ResponseSerializer::serialize_head(const Response &response) const
{
  validate_response(response);

  const bool no_message_body = status_forbids_message_body(response.status.code);
  const bool close_connection = response.connection_behavior == ConnectionBehavior::close;
  std::string serialized;
  std::size_t estimated_size = response.status.reason_phrase.size() + 32;

  for (const Header &header : response.headers)
  {
    estimated_size += header.name.size() + header.value.size() + 4;
  }

  if (!no_message_body)
  {
    estimated_size += 20;
  }

  serialized.reserve(estimated_size);
  serialized += "HTTP/1.1 ";
  serialized += std::to_string(response.status.code);
  serialized += " ";
  serialized += response.status.reason_phrase;
  serialized += "\r\n";

  for (const Header &header : response.headers)
  {
    serialized.append(header.name);
    serialized.append(": ");
    serialized.append(header.value);
    serialized.append("\r\n");
  }

  if (!no_message_body)
  {
    serialized.append("Content-Length: ");
    serialized.append(std::to_string(response_body_size(response)));
    serialized.append("\r\n");
  }
  if (close_connection)
  {
    serialized += "Connection: close\r\n";
  }
  serialized += "\r\n";

  return serialized;
}

void Vajra::response::ResponseSerializer::validate(const Response &response) const
{
  validate_response(response);
}

void Vajra::response::ResponseSerializer::validate_response(const Response &response) const
{
  validate_status(response.status);

  for (const Header &header : response.headers)
  {
    validate_header(header);
  }

  if (status_forbids_message_body(response.status.code) && !response_body_empty(response))
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

  if (Vajra::response::framing_header_name(header.name))
  {
    throw SerializationError("response headers must not override HTTP framing headers");
  }

  if (contains_invalid_http_text_bytes(header.value))
  {
    throw SerializationError("response header value contains an unsafe control character");
  }
}
