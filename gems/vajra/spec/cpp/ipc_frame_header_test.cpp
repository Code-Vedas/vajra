// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ipc/channel_kind.hpp"
#include "ipc/frame_header.hpp"
#include "ipc/protocol_compatibility.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <array>
#include <optional>
#include <stdexcept>

namespace VajraSpecCpp
{
  namespace
  {
    void test_frame_headers_round_trip_through_binary_encoding()
    {
      const Vajra::ipc::FrameHeader expected_header = {
          Vajra::ipc::ChannelKind::control,
          Vajra::ipc::FrameFamily::protocol_version_negotiation,
          Vajra::ipc::kProtocolVersion1_0,
      };
      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
          Vajra::ipc::encode_frame_header(expected_header);

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      const std::optional<Vajra::ipc::FrameHeader> decoded_header =
          Vajra::ipc::decode_frame_header(encoded_header, error);

      if (!decoded_header.has_value())
      {
        fail("ipc frame header did not decode after binary encoding");
      }

      if (decoded_header->channel != expected_header.channel ||
          decoded_header->family != expected_header.family ||
          decoded_header->version != expected_header.version)
      {
        fail("ipc frame header did not round trip through binary encoding");
      }

      if (error != Vajra::ipc::HeaderDecodeError::none)
      {
        fail("successful ipc frame header decode did not clear the decode error");
      }
    }

    void test_frame_header_encoding_rejects_invalid_contract_tuples()
    {
      try
      {
        static_cast<void>(Vajra::ipc::encode_frame_header({
            Vajra::ipc::ChannelKind::request,
            Vajra::ipc::FrameFamily::lifecycle_command,
            Vajra::ipc::kProtocolVersion1_0,
        }));
        fail("ipc frame header encoding accepted a channel/family mismatch");
      }
      catch (const std::invalid_argument &)
      {
      }

      try
      {
        static_cast<void>(Vajra::ipc::encode_frame_header({
            Vajra::ipc::ChannelKind::control,
            Vajra::ipc::FrameFamily::telemetry_status_reserved,
            Vajra::ipc::kProtocolVersion1_0,
        }));
        fail("ipc frame header encoding accepted a reserved frame family");
      }
      catch (const std::invalid_argument &error)
      {
        if (std::string(error.what()).find("reserved ipc frame family") == std::string::npos)
        {
          fail("reserved ipc frame family did not report the right encode failure");
        }
      }

      try
      {
        static_cast<void>(Vajra::ipc::encode_frame_header({
            Vajra::ipc::ChannelKind::control,
            Vajra::ipc::FrameFamily::process_registration_identity,
            Vajra::ipc::ProtocolVersion{1, 1},
        }));
        fail("ipc frame header encoding accepted an unsupported protocol version");
      }
      catch (const std::invalid_argument &error)
      {
        if (std::string(error.what()).find("unsupported protocol version") == std::string::npos)
        {
          fail("unsupported ipc protocol version did not report the right encode failure");
        }
      }

      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> negotiation_header =
          Vajra::ipc::encode_frame_header({
              Vajra::ipc::ChannelKind::control,
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::ProtocolVersion{1, 1},
          });

      if (negotiation_header[4] != 0x00 || negotiation_header[5] != 0x01 ||
          negotiation_header[6] != 0x00 || negotiation_header[7] != 0x01)
      {
        fail("ipc negotiation frame header did not preserve the unsupported advertised version");
      }
    }

    void test_reserved_header_bits_are_rejected_during_header_decode()
    {
      std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
          Vajra::ipc::encode_frame_header({
              Vajra::ipc::ChannelKind::control,
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::kProtocolVersion1_0,
          });
      encoded_header[1] = 0x01;

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("reserved ipc header bits decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::reserved_bits_set)
      {
        fail("reserved ipc header bits did not report the right decode failure");
      }
    }

    void test_unknown_channel_kind_is_rejected_during_header_decode()
    {
      std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
          Vajra::ipc::encode_frame_header({
              Vajra::ipc::ChannelKind::request,
              Vajra::ipc::FrameFamily::request_execution_input,
              Vajra::ipc::kProtocolVersion1_0,
          });
      encoded_header[0] = 0x7F;

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::channel_family_mismatch;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("invalid ipc channel kind decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::unknown_channel_kind)
      {
        fail("invalid ipc channel kind did not report the right decode failure");
      }
    }

    void test_unsupported_protocol_versions_are_rejected_during_header_decode()
    {
      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header = {
          static_cast<std::uint8_t>(Vajra::ipc::ChannelKind::control),
          0x00,
          0x20,
          0x02,
          0x00,
          0x01,
          0x00,
          0x01,
      };

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("unsupported ipc protocol version decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::unsupported_protocol_version)
      {
        fail("unsupported ipc protocol version did not report the right decode failure");
      }
    }

    void test_protocol_version_negotiation_headers_decode_even_for_unsupported_versions()
    {
      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header = {
          static_cast<std::uint8_t>(Vajra::ipc::ChannelKind::control),
          0x00,
          0x20,
          0x01,
          0x00,
          0x01,
          0x00,
          0x01,
      };

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unsupported_protocol_version;
      const std::optional<Vajra::ipc::FrameHeader> decoded_header =
          Vajra::ipc::decode_frame_header(encoded_header, error);

      if (!decoded_header.has_value())
      {
        fail("protocol negotiation header for an unsupported version did not decode structurally");
      }

      if (decoded_header->family != Vajra::ipc::FrameFamily::protocol_version_negotiation ||
          decoded_header->version != Vajra::ipc::ProtocolVersion{1, 1})
      {
        fail("protocol negotiation header did not preserve the advertised remote version");
      }
    }

    void test_reserved_frame_families_are_rejected_during_header_decode()
    {
      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header = {
          static_cast<std::uint8_t>(Vajra::ipc::ChannelKind::control),
          0x00,
          0x20,
          0x07,
          0x00,
          0x01,
          0x00,
          0x00,
      };

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("reserved ipc frame family decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::reserved_frame_family)
      {
        fail("reserved ipc frame family did not report the right decode failure");
      }

      const std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> unsupported_version_header = {
          static_cast<std::uint8_t>(Vajra::ipc::ChannelKind::control),
          0x00,
          0x20,
          0x07,
          0x00,
          0x01,
          0x00,
          0x01,
      };

      error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(unsupported_version_header, error).has_value())
      {
        fail("reserved ipc frame family with an unsupported version decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::reserved_frame_family)
      {
        fail("reserved ipc frame family lost its specific decode failure on an unsupported version");
      }
    }

    void test_unknown_frame_family_is_rejected_during_header_decode()
    {
      std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
          Vajra::ipc::encode_frame_header({
              Vajra::ipc::ChannelKind::control,
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::kProtocolVersion1_0,
          });
      encoded_header[2] = 0xFF;
      encoded_header[3] = 0xFF;

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("invalid ipc frame family decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::unknown_frame_family)
      {
        fail("invalid ipc frame family did not report the right decode failure");
      }
    }

    void test_channel_family_mismatch_is_rejected_during_header_decode()
    {
      std::array<std::uint8_t, Vajra::ipc::kFrameHeaderSize> encoded_header =
          Vajra::ipc::encode_frame_header({
              Vajra::ipc::ChannelKind::request,
              Vajra::ipc::FrameFamily::request_execution_input,
              Vajra::ipc::kProtocolVersion1_0,
          });
      encoded_header[0] = static_cast<std::uint8_t>(Vajra::ipc::ChannelKind::control);

      Vajra::ipc::HeaderDecodeError error = Vajra::ipc::HeaderDecodeError::unknown_channel_kind;
      if (Vajra::ipc::decode_frame_header(encoded_header, error).has_value())
      {
        fail("ipc channel/family mismatch decoded successfully");
      }

      if (error != Vajra::ipc::HeaderDecodeError::channel_family_mismatch)
      {
        fail("ipc channel/family mismatch did not report the right decode failure");
      }
    }
  }

  void run_ipc_frame_header_tests()
  {
    test_frame_headers_round_trip_through_binary_encoding();
    test_frame_header_encoding_rejects_invalid_contract_tuples();
    test_reserved_header_bits_are_rejected_during_header_decode();
    test_unknown_channel_kind_is_rejected_during_header_decode();
    test_unknown_frame_family_is_rejected_during_header_decode();
    test_channel_family_mismatch_is_rejected_during_header_decode();
    test_unsupported_protocol_versions_are_rejected_during_header_decode();
    test_protocol_version_negotiation_headers_decode_even_for_unsupported_versions();
    test_reserved_frame_families_are_rejected_during_header_decode();
  }
}
