// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ipc/channel_kind.hpp"
#include "ipc/frame_contract.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

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

      try
      {
        static_cast<void>(Vajra::ipc::wire_id(static_cast<Vajra::ipc::FrameFamily>(0xFFFF)));
        fail("unknown ipc frame family unexpectedly produced a wire id");
      }
      catch (const std::invalid_argument &)
      {
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

    void test_reserved_families_have_no_supported_activation_version()
    {
      const std::optional<Vajra::ipc::ProtocolVersion> reserved_first_supported =
          Vajra::ipc::first_supported_protocol_version(Vajra::ipc::FrameFamily::telemetry_status_reserved);
      if (reserved_first_supported.has_value())
      {
        fail("reserved ipc frame family unexpectedly reported a supported activation version");
      }

      const std::optional<Vajra::ipc::ProtocolVersion> request_first_supported =
          Vajra::ipc::first_supported_protocol_version(Vajra::ipc::FrameFamily::request_execution_input);
      if (!request_first_supported.has_value() ||
          request_first_supported.value() != Vajra::ipc::kProtocolVersion1_0)
      {
        fail("active ipc frame family did not report its first supported version");
      }
    }
  }

  void run_ipc_frame_contract_tests()
  {
    test_frame_families_round_trip_through_wire_ids();
    test_unknown_wire_id_is_rejected();
    test_unknown_enum_values_are_not_treated_as_valid_control_families();
    test_frame_families_are_assigned_to_exactly_one_channel();
    test_request_and_control_families_stay_separated();
    test_reserved_families_have_no_supported_activation_version();
  }
}
