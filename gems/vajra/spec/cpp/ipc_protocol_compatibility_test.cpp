// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ipc/frame_contract.hpp"
#include "ipc/protocol_compatibility.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

namespace VajraSpecCpp
{
  namespace
  {
    void test_reserved_frame_family_is_not_implicitly_available()
    {
      if (!Vajra::ipc::reserved_family(Vajra::ipc::FrameFamily::telemetry_status_reserved))
      {
        fail("reserved telemetry frame family is not marked reserved");
      }

      if (Vajra::ipc::frame_family_active_for_protocol_version(
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
              Vajra::ipc::kProtocolVersion1_0,
              Vajra::ipc::ProtocolVersion{1, 1}) != Vajra::ipc::CompatibilityResult::remote_newer_minor)
      {
        fail("ipc compatibility check did not report a newer remote minor version");
      }

      if (Vajra::ipc::check_compatibility(
              Vajra::ipc::ProtocolVersion{1, 1},
              Vajra::ipc::ProtocolVersion{1, 0}) != Vajra::ipc::CompatibilityResult::unsupported_local_version)
      {
        fail("ipc compatibility check accepted a locally unsupported minor version");
      }
    }

    void test_supported_frame_families_follow_protocol_compatibility()
    {
      if (!Vajra::ipc::frame_family_active_for_protocol_version(
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::kProtocolVersion1_0))
      {
        fail("protocol negotiation frame family is unavailable in the baseline protocol");
      }

      if (Vajra::ipc::frame_family_active_for_protocol_version(
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::ProtocolVersion{1, 1}))
      {
        fail("unsupported protocol versions must not expose post-negotiation active frame families");
      }

      if (Vajra::ipc::frame_family_active_for_protocol_version(
              static_cast<Vajra::ipc::FrameFamily>(0x9999),
              Vajra::ipc::kProtocolVersion1_0))
      {
        fail("unknown frame families must not become available in the baseline protocol");
      }

      if (Vajra::ipc::frame_family_active_for_protocol_version(
              Vajra::ipc::FrameFamily::telemetry_status_reserved,
              Vajra::ipc::ProtocolVersion{1, 1}))
      {
        fail("reserved frame families must not become available without an explicit version table entry");
      }
    }

    void test_outbound_negotiation_frames_require_explicit_activation()
    {
      if (Vajra::ipc::validate_outbound_frame(
              Vajra::ipc::ChannelKind::control,
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::kProtocolVersion1_0) != Vajra::ipc::FrameValidationError::none)
      {
        fail("baseline negotiation frame is not encodable for its active protocol version");
      }

      if (Vajra::ipc::validate_outbound_frame(
              Vajra::ipc::ChannelKind::control,
              Vajra::ipc::FrameFamily::protocol_version_negotiation,
              Vajra::ipc::ProtocolVersion{1, 1}) != Vajra::ipc::FrameValidationError::unsupported_protocol_version)
      {
        fail("unsupported protocol versions must not make negotiation frames encodable without explicit activation");
      }
    }
  }

  void run_ipc_protocol_compatibility_tests()
  {
    test_reserved_frame_family_is_not_implicitly_available();
    test_protocol_version_is_compatible_with_itself();
    test_unsupported_local_protocol_version_is_rejected();
    test_incompatible_major_version_is_rejected();
    test_unsupported_minor_version_is_rejected();
    test_supported_frame_families_follow_protocol_compatibility();
    test_outbound_negotiation_frames_require_explicit_activation();
  }
}
