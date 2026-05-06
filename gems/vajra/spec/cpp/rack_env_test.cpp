// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/rack_env.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

namespace VajraSpecCpp
{
  namespace
  {
    const Vajra::request::RackEnvEntry *find_entry(
        const std::vector<Vajra::request::RackEnvEntry> &entries,
        const std::string &key)
    {
      for (const Vajra::request::RackEnvEntry &entry : entries)
      {
        if (entry.key == key)
        {
          return &entry;
        }
      }

      return nullptr;
    }

    void expect_entry_value(
        const std::vector<Vajra::request::RackEnvEntry> &entries,
        const std::string &key,
        const std::string &expected_value)
    {
      const Vajra::request::RackEnvEntry *entry = find_entry(entries, key);
      if (entry == nullptr)
      {
        fail("missing Rack env entry: " + key);
      }

      if (entry->value != expected_value)
      {
        fail("unexpected Rack env entry value for " + key);
      }
    }

    void test_split_target_separates_path_and_query()
    {
      Vajra::request::RackEnvBuilder builder;
      const Vajra::request::RackRequestTarget target = builder.split_target("/projects?filter=active");

      if (target.path_info != "/projects")
      {
        fail("Rack target path info was not split correctly");
      }

      if (target.query_string != "filter=active")
      {
        fail("Rack target query string was not split correctly");
      }
    }

    void test_split_target_rejects_non_absolute_targets()
    {
      Vajra::request::RackEnvBuilder builder;

      try
      {
        (void)builder.split_target("projects?filter=active");
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request)
        {
          fail("invalid Rack target used the wrong failure kind");
        }

        return;
      }

      fail("non-absolute Rack target was accepted");
    }

    void test_build_maps_method_path_query_headers_and_socket_context()
    {
      Vajra::request::RackEnvBuilder builder;
      const Vajra::request::RequestContext request_context{
          Vajra::request::ParsedRequest{
              Vajra::request::ParsedRequestLine{"GET", "/projects?filter=active", "HTTP/1.1"},
              {
                  Vajra::request::ParsedHeader{"Host", "example.test"},
                  Vajra::request::ParsedHeader{"Content-Type", "application/json"},
                  Vajra::request::ParsedHeader{"X-Trace-Id", "abc123"},
                  Vajra::request::ParsedHeader{"X-Trace-Id", "def456"},
              }},
          Vajra::request::SocketContext{"127.0.0.2", 54'321, "127.0.0.1", 3000, "http"}};

      const std::vector<Vajra::request::RackEnvEntry> env_entries = builder.build(request_context);

      expect_entry_value(env_entries, "REQUEST_METHOD", "GET");
      expect_entry_value(env_entries, "SCRIPT_NAME", "");
      expect_entry_value(env_entries, "PATH_INFO", "/projects");
      expect_entry_value(env_entries, "QUERY_STRING", "filter=active");
      expect_entry_value(env_entries, "SERVER_PROTOCOL", "HTTP/1.1");
      expect_entry_value(env_entries, "SERVER_NAME", "127.0.0.1");
      expect_entry_value(env_entries, "SERVER_PORT", "3000");
      expect_entry_value(env_entries, "REMOTE_ADDR", "127.0.0.2");
      expect_entry_value(env_entries, "REMOTE_PORT", "54321");
      expect_entry_value(env_entries, "rack.url_scheme", "http");
      expect_entry_value(env_entries, "HTTP_HOST", "example.test");
      expect_entry_value(env_entries, "CONTENT_TYPE", "application/json");
      expect_entry_value(env_entries, "HTTP_X_TRACE_ID", "abc123,def456");
    }

    void test_build_rejects_unsupported_header_name_characters()
    {
      Vajra::request::RackEnvBuilder builder;
      const Vajra::request::RequestContext request_context{
          Vajra::request::ParsedRequest{
              Vajra::request::ParsedRequestLine{"GET", "/", "HTTP/1.1"},
              {Vajra::request::ParsedHeader{"X/Trace", "abc123"}}},
          Vajra::request::SocketContext{"127.0.0.1", 10'000, "127.0.0.1", 3000, "http"}};

      try
      {
        (void)builder.build(request_context);
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::bad_request)
        {
          fail("invalid Rack header name used the wrong failure kind");
        }

        return;
      }

      fail("unsupported Rack header name characters were accepted");
    }
  }

  void run_rack_env_tests()
  {
    test_split_target_separates_path_and_query();
    test_split_target_rejects_non_absolute_targets();
    test_build_maps_method_path_query_headers_and_socket_context();
    test_build_rejects_unsupported_header_name_characters();
  }
}
