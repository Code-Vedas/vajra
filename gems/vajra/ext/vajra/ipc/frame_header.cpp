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

      void write_big_endian_u16(
          std::array<std::uint8_t, kFrameHeaderSize> &encoded_header,
          std::size_t offset,
          std::uint16_t value)
      {
        encoded_header[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        encoded_header[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
      }

      std::array<std::uint8_t, kFrameHeaderSize> build_encoded_header(const FrameHeader &header)
      {
        std::array<std::uint8_t, kFrameHeaderSize> encoded_header{};
        encoded_header[0] = static_cast<std::uint8_t>(header.channel);
        encoded_header[1] = 0;
        write_big_endian_u16(encoded_header, 2, wire_id(header.family));
        write_big_endian_u16(encoded_header, 4, header.version.major);
        write_big_endian_u16(encoded_header, 6, header.version.minor);
        return encoded_header;
      }
    }

    std::array<std::uint8_t, kFrameHeaderSize> encode_frame_header(const FrameHeader &header)
    {
      if (!known_frame_family(header.family))
      {
        throw std::invalid_argument("cannot encode unknown ipc frame family");
      }

      if (!valid_on_channel(header.family, header.channel))
      {
        throw std::invalid_argument("cannot encode ipc frame family on the wrong channel");
      }

      if (reserved_family(header.family))
      {
        throw std::invalid_argument("cannot encode reserved ipc frame family");
      }

      if (!supported_protocol_version(header.version))
      {
        throw std::invalid_argument("cannot encode ipc frame header for an unsupported protocol version");
      }

      if (header.family == FrameFamily::protocol_version_negotiation)
      {
        return build_encoded_header(header);
      }

      if (!frame_family_active_for_protocol_version(header.family, header.version))
      {
        throw std::invalid_argument("cannot encode unavailable ipc frame family for the requested protocol version");
      }

      return build_encoded_header(header);
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

      if (!valid_on_channel(family.value(), channel))
      {
        error = HeaderDecodeError::channel_family_mismatch;
        return std::nullopt;
      }

      if (family.value() == FrameFamily::protocol_version_negotiation)
      {
        return FrameHeader{
            channel,
            family.value(),
            version,
        };
      }

      if (reserved_family(family.value()))
      {
        error = HeaderDecodeError::reserved_frame_family;
        return std::nullopt;
      }

      if (!supported_protocol_version(version))
      {
        error = HeaderDecodeError::unsupported_protocol_version;
        return std::nullopt;
      }

      if (!frame_family_active_for_protocol_version(family.value(), version))
      {
        error = HeaderDecodeError::unavailable_frame_family;
        return std::nullopt;
      }

      return FrameHeader{
          channel,
          family.value(),
          version,
      };
    }
  }
}
