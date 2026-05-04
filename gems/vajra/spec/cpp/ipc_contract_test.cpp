// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ipc/channel_kind.hpp"
#include "ipc/frame_contract.hpp"
#include "ipc/frame_header.hpp"
#include "ipc/protocol_compatibility.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace VajraSpecCpp
{
  namespace
  {
    void test_frame_families_round_trip_through_wire_ids()
    {
      for (const Vajra::ipc::FrameFamily family : Vajra::ipc::kFrameFamilies)
      {
        const std::uint16_t encoded_wire_id = Vajra::ipc::wire_id(family);
        const std::optional<Vajra::ipc::FrameFamily> decoded_family = Vajra::ipc::decode_frame_family(encoded_wire_id);
        if (!decoded_family.has_value() || decoded_family.value() != family)
        {
          fail("ipc frame family did not round trip through its wire id");
        }
      }
    }

    void test_unknown_wire_id_is_rejected()
    {
      if (Vajra::ipc::decode_frame_family(0xFFFF).has_value())
      {
        fail("unknown ipc frame wire id decoded successfully");
      }
    }

    void test_unknown_enum_values_are_not_treated_as_valid_control_families()
    {
      const Vajra::ipc::FrameFamily unknown_family = static_cast<Vajra::ipc::FrameFamily>(0x9999);

      if (Vajra::ipc::known_frame_family(unknown_family))
      {
        fail("unknown ipc frame family was treated as known");
      }

      if (Vajra::ipc::valid_on_channel(unknown_family, Vajra::ipc::ChannelKind::control))
      {
        fail("unknown ipc frame family was treated as valid on the control channel");
      }

      try
      {
        static_cast<void>(Vajra::ipc::owning_channel(unknown_family));
        fail("unknown ipc frame family produced an owning channel");
      }
      catch (const std::invalid_argument &)
      {
      }
    }

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
        if (std::strcmp(error.what(), "cannot encode reserved ipc frame family") != 0)
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
      catch (const std::invalid_argument &)
      {
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

      if (error != Vajra::ipc::HeaderDecodeError::unavailable_frame_family)
      {
        fail("reserved ipc frame family did not report the right decode failure");
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

    void test_frame_families_are_assigned_to_exactly_one_channel()
    {
      for (const Vajra::ipc::FrameFamily family : Vajra::ipc::kFrameFamilies)
      {
        const bool valid_on_request_channel = Vajra::ipc::valid_on_channel(family, Vajra::ipc::ChannelKind::request);
        const bool valid_on_control_channel = Vajra::ipc::valid_on_channel(family, Vajra::ipc::ChannelKind::control);
        if (valid_on_request_channel == valid_on_control_channel)
        {
          fail("ipc frame family must belong to exactly one channel");
        }
      }
    }

    void test_request_and_control_families_stay_separated()
    {
      if (!Vajra::ipc::valid_on_channel(
              Vajra::ipc::FrameFamily::request_execution_input,
              Vajra::ipc::ChannelKind::request))
      {
        fail("request execution input frame family is not valid on the request channel");
      }

      if (Vajra::ipc::valid_on_channel(
              Vajra::ipc::FrameFamily::request_execution_input,
              Vajra::ipc::ChannelKind::control))
      {
        fail("request execution input frame family leaked onto the control channel");
      }

      if (!Vajra::ipc::valid_on_channel(
              Vajra::ipc::FrameFamily::lifecycle_command,
              Vajra::ipc::ChannelKind::control))
      {
        fail("lifecycle command frame family is not valid on the control channel");
      }

      if (Vajra::ipc::valid_on_channel(
              Vajra::ipc::FrameFamily::lifecycle_command,
              Vajra::ipc::ChannelKind::request))
      {
        fail("lifecycle command frame family leaked onto the request channel");
      }
    }

    void test_reserved_frame_family_is_not_implicitly_available()
    {
      if (!Vajra::ipc::reserved_family(Vajra::ipc::FrameFamily::telemetry_status_reserved))
      {
        fail("reserved telemetry frame family is not marked reserved");
      }

      if (Vajra::ipc::frame_family_available(
              Vajra::ipc::FrameFamily::telemetry_status_reserved,
              Vajra::ipc::kProtocolVersion1_0))
      {
        fail("reserved telemetry frame family became available in the baseline protocol");
      }
    }

    void test_protocol_version_is_compatible_with_itself()
    {
      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::kProtocolVersion1_0,
              Vajra::ipc::kProtocolVersion1_0) != Vajra::ipc::CompatibilityResult::compatible)
      {
        fail("baseline ipc protocol version is not compatible with itself");
      }
    }

    void test_unsupported_local_protocol_version_is_rejected()
    {
      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::ProtocolVersion{2, 0},
              Vajra::ipc::ProtocolVersion{2, 0}) != Vajra::ipc::CompatibilityResult::unsupported_local_version)
      {
        fail("ipc compatibility check accepted an unsupported local protocol version");
      }
    }

    void test_incompatible_major_version_is_rejected()
    {
      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::kProtocolVersion1_0,
              Vajra::ipc::ProtocolVersion{2, 0}) != Vajra::ipc::CompatibilityResult::incompatible_major)
      {
        fail("ipc compatibility check accepted an incompatible major version");
      }
    }

    void test_unsupported_minor_version_is_rejected()
    {
      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::ProtocolVersion{1, 1},
              Vajra::ipc::kProtocolVersion1_0) != Vajra::ipc::CompatibilityResult::unsupported_local_version)
      {
        fail("ipc compatibility check accepted an unsupported local minor version");
      }

      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::kProtocolVersion1_0,
              Vajra::ipc::ProtocolVersion{1, 1}) != Vajra::ipc::CompatibilityResult::unsupported_remote_version)
      {
        fail("ipc compatibility check did not report an unsupported remote version");
      }

      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::ProtocolVersion{1, 1},
              Vajra::ipc::ProtocolVersion{1, 0}) != Vajra::ipc::CompatibilityResult::unsupported_local_version)
      {
        fail("ipc compatibility check accepted a locally unsupported older-minor comparison");
      }
    }

    void test_supported_frame_families_follow_protocol_compatibility()
    {
      if (!Vajra::ipc::frame_family_available(
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::kProtocolVersion1_0))
      {
        fail("protocol negotiation frame family is unavailable in the baseline protocol");
      }

      if (Vajra::ipc::frame_family_available(
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::ProtocolVersion{1, 1}))
      {
        fail("unsupported protocol versions must not expose active frame families");
      }

      if (Vajra::ipc::frame_family_available(
              static_cast<Vajra::ipc::FrameFamily>(0x9999),
              Vajra::ipc::kProtocolVersion1_0))
      {
        fail("unknown frame families must not become available in the baseline protocol");
      }

      if (Vajra::ipc::frame_family_available(
              Vajra::ipc::FrameFamily::telemetry_status_reserved,
              Vajra::ipc::ProtocolVersion{1, 1}))
      {
        fail("reserved frame families must not become available without an explicit version table entry");
      }
    }
  }

  void run_ipc_contract_tests()
  {
    test_frame_families_round_trip_through_wire_ids();
    test_unknown_wire_id_is_rejected();
    test_unknown_enum_values_are_not_treated_as_valid_control_families();
    test_frame_headers_round_trip_through_binary_encoding();
    test_frame_header_encoding_rejects_invalid_contract_tuples();
    test_reserved_header_bits_are_rejected_during_header_decode();
    test_unknown_channel_kind_is_rejected_during_header_decode();
    test_unknown_frame_family_is_rejected_during_header_decode();
    test_channel_family_mismatch_is_rejected_during_header_decode();
    test_unsupported_protocol_versions_are_rejected_during_header_decode();
    test_protocol_version_negotiation_headers_decode_even_for_unsupported_versions();
    test_reserved_frame_families_are_rejected_during_header_decode();
    test_frame_families_are_assigned_to_exactly_one_channel();
    test_request_and_control_families_stay_separated();
    test_reserved_frame_family_is_not_implicitly_available();
    test_protocol_version_is_compatible_with_itself();
    test_unsupported_local_protocol_version_is_rejected();
    test_incompatible_major_version_is_rejected();
    test_unsupported_minor_version_is_rejected();
    test_supported_frame_families_follow_protocol_compatibility();
  }
}
