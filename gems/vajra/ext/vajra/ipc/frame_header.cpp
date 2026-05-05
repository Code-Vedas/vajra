// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "frame_header.hpp"

#include "protocol_compatibility.hpp"

#include <stdexcept>

namespace Vajra
{
  namespace ipc
  {
    namespace
    {
      std::uint16_t read_big_endian_u16(const std::array<std::uint8_t, kFrameHeaderSize> &encoded_header, std::size_t offset)
      {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(encoded_header[offset]) << 8) |
            static_cast<std::uint16_t>(encoded_header[offset + 1]));
      }

      std::uint32_t read_big_endian_u32(const std::array<std::uint8_t, kFrameHeaderSize> &encoded_header, std::size_t offset)
      {
        return (static_cast<std::uint32_t>(encoded_header[offset]) << 24) |
               (static_cast<std::uint32_t>(encoded_header[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(encoded_header[offset + 2]) << 8) |
               static_cast<std::uint32_t>(encoded_header[offset + 3]);
      }

      void write_big_endian_u16(
          std::array<std::uint8_t, kFrameHeaderSize> &encoded_header,
          std::size_t offset,
          std::uint16_t value)
      {
        encoded_header[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        encoded_header[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
      }

      void write_big_endian_u32(
          std::array<std::uint8_t, kFrameHeaderSize> &encoded_header,
          std::size_t offset,
          std::uint32_t value)
      {
        encoded_header[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
        encoded_header[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        encoded_header[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        encoded_header[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
      }

      std::array<std::uint8_t, kFrameHeaderSize> build_encoded_header(const FrameHeader &header)
      {
        std::array<std::uint8_t, kFrameHeaderSize> encoded_header{};
        encoded_header[0] = static_cast<std::uint8_t>(header.channel);
        encoded_header[1] = 0;
        write_big_endian_u16(encoded_header, 2, wire_id(header.family));
        write_big_endian_u16(encoded_header, 4, header.version.major);
        write_big_endian_u16(encoded_header, 6, header.version.minor);
        write_big_endian_u32(encoded_header, 8, header.payload_length);
        return encoded_header;
      }
    }

    std::array<std::uint8_t, kFrameHeaderSize> encode_frame_header(const FrameHeader &header)
    {
      if (header.payload_length > kMaxFramePayloadLength)
      {
        throw std::invalid_argument("cannot encode ipc frame header with payload length above the maximum frame size");
      }

      switch (validate_outbound_frame(header.channel, header.family, header.version))
      {
      case FrameValidationError::none:
        return build_encoded_header(header);
      case FrameValidationError::unknown_frame_family:
        throw std::invalid_argument("cannot encode unknown ipc frame family");
      case FrameValidationError::channel_family_mismatch:
        throw std::invalid_argument("cannot encode ipc frame family on the wrong channel");
      case FrameValidationError::reserved_frame_family:
        throw std::invalid_argument("cannot encode reserved ipc frame family");
      case FrameValidationError::unsupported_protocol_version:
        throw std::invalid_argument("cannot encode ipc frame header for an unsupported protocol version");
      case FrameValidationError::unavailable_frame_family:
        throw std::invalid_argument("cannot encode unavailable ipc frame family for the requested protocol version");
      }

      throw std::logic_error("unhandled outbound ipc frame validation result");
    }

    std::optional<FrameHeader> decode_frame_header(
        const std::array<std::uint8_t, kFrameHeaderSize> &encoded_header,
        HeaderDecodeError &error)
    {
      error = HeaderDecodeError::none;

      if (encoded_header[1] != 0)
      {
        error = HeaderDecodeError::reserved_bits_set;
        return std::nullopt;
      }

      const std::uint32_t payload_length = read_big_endian_u32(encoded_header, 8);
      if (payload_length > kMaxFramePayloadLength)
      {
        error = HeaderDecodeError::payload_too_large;
        return std::nullopt;
      }

      const std::uint8_t encoded_channel = encoded_header[0];
      const std::optional<FrameFamily> family = decode_frame_family(read_big_endian_u16(encoded_header, 2));
      const ProtocolVersion version = {
          read_big_endian_u16(encoded_header, 4),
          read_big_endian_u16(encoded_header, 6),
      };

      ChannelKind channel;
      if (encoded_channel == static_cast<std::uint8_t>(ChannelKind::request))
      {
        channel = ChannelKind::request;
      }
      else if (encoded_channel == static_cast<std::uint8_t>(ChannelKind::control))
      {
        channel = ChannelKind::control;
      }
      else
      {
        error = HeaderDecodeError::unknown_channel_kind;
        return std::nullopt;
      }

      if (!family.has_value())
      {
        error = HeaderDecodeError::unknown_frame_family;
        return std::nullopt;
      }

      if (family.value() == FrameFamily::protocol_version_negotiation &&
          !supported_protocol_version(version))
      {
        error = HeaderDecodeError::unsupported_protocol_version;
        return FrameHeader{
            channel,
            family.value(),
            version,
            payload_length,
        };
      }

      switch (validate_inbound_frame(channel, family.value(), version))
      {
      case FrameValidationError::none:
        return FrameHeader{
            channel,
            family.value(),
            version,
            payload_length,
        };
      case FrameValidationError::unknown_frame_family:
        error = HeaderDecodeError::unknown_frame_family;
        return std::nullopt;
      case FrameValidationError::channel_family_mismatch:
        error = HeaderDecodeError::channel_family_mismatch;
        return std::nullopt;
      case FrameValidationError::reserved_frame_family:
        error = HeaderDecodeError::reserved_frame_family;
        return std::nullopt;
      case FrameValidationError::unsupported_protocol_version:
        error = HeaderDecodeError::unsupported_protocol_version;
        return std::nullopt;
      case FrameValidationError::unavailable_frame_family:
        error = HeaderDecodeError::unavailable_frame_family;
        return std::nullopt;
      }

      throw std::logic_error("unhandled inbound ipc frame validation result");
    }
  }
}
