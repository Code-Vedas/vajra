// Copyright Codevedas Inc. 2025-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "request/request_head_parser.hpp"
#include "request/request_head_size_validator.hpp"
#include "test_suites.hpp"
#include "test_support.hpp"

#include <random>
#include <string>
#include <vector>

namespace VajraSpecCpp
{
  namespace
  {
    void test_parse_request_head_parses_request_line_and_headers()
    {
      const std::string request_head =
          "GET /projects?filter=active HTTP/1.1\r\n"
          "Host: example.test\r\n"
          "User-Agent: vajra-test\r\n"
          "X-Trace-Id: abc123\r\n"
          "\r\n";

      Vajra::request::RequestHeadParser parser;
      const Vajra::request::ParsedRequest request = parser.parse(request_head);

      if (request.request_line.method != "GET")
      {
        fail("request method was not parsed correctly");
      }

      if (request.request_line.target != "/projects?filter=active")
      {
        fail("request target was not parsed correctly");
      }

      if (request.request_line.version != "HTTP/1.1")
      {
        fail("request version was not parsed correctly");
      }

      if (request.headers.size() != 3)
      {
        fail("request headers were not parsed correctly");
      }

      if (request.headers[0].name != "Host" || request.headers[0].value != "example.test")
      {
        fail("first request header was not parsed correctly");
      }

      if (request.headers[2].name != "X-Trace-Id" || request.headers[2].value != "abc123")
      {
        fail("later request headers were not parsed correctly");
      }
    }

    void test_parse_request_head_preserves_header_order_and_empty_values()
    {
      Vajra::request::RequestHeadParser parser;
      const Vajra::request::ParsedRequest request = parser.parse(
          "POST /submit HTTP/1.1\r\n"
          "Host: example.test\r\n"
          "X-Empty:\r\n"
          "X-Trimmed:   kept\r\n"
          "\r\n");

      if (request.headers.size() != 3)
      {
        fail("header count did not match expected value");
      }

      if (request.headers[1].name != "X-Empty" || request.headers[1].value != "")
      {
        fail("empty header value was not preserved");
      }

      if (request.headers[2].name != "X-Trimmed" || request.headers[2].value != "kept")
      {
        fail("header value trimming or ordering regressed");
      }
    }

    void test_parse_request_head_rejects_malformed_request_line_variants()
    {
      const std::vector<std::string> invalid_request_heads = {
          "GET /only-two-parts\r\nHost: example.test\r\n\r\n",
          " /missing-method HTTP/1.1\r\nHost: example.test\r\n\r\n",
          "GET  HTTP/1.1\r\nHost: example.test\r\n\r\n",
          "GET / HTTP/1.1 EXTRA\r\nHost: example.test\r\n\r\n"};

      for (const std::string &request_head : invalid_request_heads)
      {
        expect_parse_error(request_head, Vajra::request::HeadFailureKind::bad_request, "invalid request line");
      }
    }

    void test_parse_request_head_rejects_invalid_header_variants()
    {
      const std::vector<std::string> invalid_request_heads = {
          "GET / HTTP/1.1\r\nHost example.test\r\n\r\n",
          "GET / HTTP/1.1\r\n: example.test\r\n\r\n",
          "GET / HTTP/1.1\r\nBad Header: value\r\n\r\n"};

      expect_parse_error(
          invalid_request_heads[0],
          Vajra::request::HeadFailureKind::bad_request,
          "invalid header line");
      expect_parse_error(
          invalid_request_heads[1],
          Vajra::request::HeadFailureKind::bad_request,
          "invalid header line");
      expect_parse_error(
          invalid_request_heads[2],
          Vajra::request::HeadFailureKind::bad_request,
          "invalid header name");
    }

    void test_parse_request_head_rejects_invalid_http_version_variants()
    {
      const std::vector<std::string> invalid_request_heads = {
          "GET / HTTP/2.0\r\nHost: example.test\r\n\r\n",
          "GET / HTTP/1.0\r\nHost: example.test\r\n\r\n",
          "GET / http/1.1\r\nHost: example.test\r\n\r\n"};

      for (const std::string &request_head : invalid_request_heads)
      {
        expect_parse_error(request_head, Vajra::request::HeadFailureKind::bad_request, "invalid HTTP version");
      }
    }

    void test_validate_request_head_size_rejects_oversized_request_head()
    {
      try
      {
        Vajra::request::RequestHeadSizeValidator validator(Vajra::request::kDefaultMaxRequestHeadBytes);
        validator.validate(Vajra::request::kDefaultMaxRequestHeadBytes + 1);
      }
      catch (const Vajra::request::HeadError &error)
      {
        if (error.kind() != Vajra::request::HeadFailureKind::header_too_large)
        {
          fail("oversized request head used the wrong failure kind");
        }

        if (std::string(error.what()).find("request head exceeds maximum size") == std::string::npos)
        {
          fail("oversized request head used the wrong failure message");
        }

        return;
      }

      fail("oversized request head was not rejected");
    }

    void test_head_reader_accepts_fragmented_request_head()
    {
      const std::vector<std::string> chunks = {
          "GET /fragmented HTTP/1.1\r\nHost: example.test\r\nConnection: close\r",
          "\n",
          "\r",
          "\n"};

      const std::string expected_request_head =
          "GET /fragmented HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
      const ReaderOutcome outcome = read_request_head_from_chunks(chunks, expected_request_head.size());

      if (outcome.error)
      {
        std::rethrow_exception(outcome.error);
      }

      if (!outcome.result.complete)
      {
        fail("fragmented request head was not marked complete");
      }

      if (outcome.result.request_head != expected_request_head)
      {
        fail("fragmented request head bytes were not preserved");
      }
    }

    void test_head_reader_returns_incomplete_on_peer_close_before_boundary()
    {
      const ReaderOutcome outcome = read_request_head_from_chunks(
          {"GET /partial HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n"},
          Vajra::request::kDefaultMaxRequestHeadBytes);

      if (outcome.error)
      {
        std::rethrow_exception(outcome.error);
      }

      if (outcome.result.complete)
      {
        fail("partial request head was incorrectly marked complete");
      }

      if (outcome.result.request_head.empty())
      {
        fail("partial request head bytes were not captured");
      }
    }

    void test_head_reader_accepts_exact_limit_request_head()
    {
      const std::string request_head =
          "GET /exact-limit HTTP/1.1\r\nHost: example.test\r\nX-Fill: 123456\r\n\r\n";
      const ReaderOutcome outcome = read_request_head_from_chunks({request_head}, request_head.size());

      if (outcome.error)
      {
        std::rethrow_exception(outcome.error);
      }

      if (!outcome.result.complete)
      {
        fail("exact-limit request head was not accepted");
      }

      if (outcome.result.request_head.size() != request_head.size())
      {
        fail("exact-limit request head size changed unexpectedly");
      }
    }

    void test_head_reader_rejects_limit_plus_one_request_head()
    {
      const std::string request_head =
          "GET /limit-plus-one HTTP/1.1\r\nHost: example.test\r\nX-Fill: 1234567\r\n\r\n";

      expect_reader_error(
          {request_head},
          request_head.size() - 1,
          Vajra::request::HeadFailureKind::header_too_large,
          "request head exceeds maximum size");
    }

    void test_property_generated_valid_request_heads_parse()
    {
      std::mt19937 generator(29);

      for (int index = 0; index < 50; ++index)
      {
        const int path_suffix = static_cast<int>(generator() % 10000);
        const int trace_suffix = static_cast<int>(generator() % 100000);
        const std::string method = (index % 2 == 0) ? "GET" : "POST";
        const std::string target = "/generated/" + std::to_string(path_suffix) + "?case=" + std::to_string(index);
        const std::string request_head =
            method + " " + target + " HTTP/1.1\r\n" +
            "Host: generated.test\r\n" +
            "X-Trace-Id: trace-" + std::to_string(trace_suffix) + "\r\n" +
            "X-Optional:" + (index % 3 == 0 ? "" : " value") + "\r\n" +
            "\r\n";

        expect_parse_success(request_head, method, target, "HTTP/1.1", 3);
      }
    }

    void test_property_generated_invalid_request_lines_reject()
    {
      std::mt19937 generator(291);

      for (int index = 0; index < 50; ++index)
      {
        const int suffix = static_cast<int>(generator() % 10000);
        const std::string malformed_request_line =
            (index % 2 == 0)
                ? "GET  HTTP/1.1"
                : "GET /generated/" + std::to_string(suffix) + " HTTP/1.1 EXTRA";

        expect_parse_error(
            malformed_request_line + "\r\nHost: generated.test\r\n\r\n",
            Vajra::request::HeadFailureKind::bad_request,
            "invalid request line");
      }
    }

    void test_property_generated_invalid_headers_reject()
    {
      std::mt19937 generator(292);

      for (int index = 0; index < 50; ++index)
      {
        const int suffix = static_cast<int>(generator() % 10000);
        const std::string invalid_header =
            (index % 2 == 0)
                ? "Bad Header " + std::to_string(suffix) + ": value"
                : "MissingColon " + std::to_string(suffix);

        expect_parse_error(
            "GET /generated HTTP/1.1\r\n" + invalid_header + "\r\n\r\n",
            Vajra::request::HeadFailureKind::bad_request,
            index % 2 == 0 ? "invalid header name" : "invalid header line");
      }
    }

    void test_property_generated_partial_request_heads_never_complete()
    {
      const std::string complete_request_head =
          "GET /partial-generated HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";

      for (std::size_t prefix_length = 1; prefix_length + 1 < complete_request_head.size(); prefix_length += 7)
      {
        const ReaderOutcome outcome = read_request_head_from_chunks(
            {complete_request_head.substr(0, prefix_length)},
            Vajra::request::kDefaultMaxRequestHeadBytes);

        if (outcome.error)
        {
          std::rethrow_exception(outcome.error);
        }

        if (outcome.result.complete)
        {
          fail("partial request head was incorrectly marked complete in generated coverage");
        }
      }
    }

    void test_property_generated_near_boundary_size_classification()
    {
      for (std::size_t fill_length = 0; fill_length < 32; ++fill_length)
      {
        const std::string base_request_head =
            "GET /size-check HTTP/1.1\r\nHost: example.test\r\nX-Fill: " +
            std::string(fill_length, 'a') + "\r\n\r\n";
        const std::size_t exact_limit = base_request_head.size();

        const ReaderOutcome exact_outcome = read_request_head_from_chunks({base_request_head}, exact_limit);
        if (exact_outcome.error)
        {
          std::rethrow_exception(exact_outcome.error);
        }

        if (!exact_outcome.result.complete)
        {
          fail("near-boundary exact request head was not accepted");
        }

        expect_reader_error(
            {base_request_head},
            exact_limit - 1,
            Vajra::request::HeadFailureKind::header_too_large,
            "request head exceeds maximum size");
      }
    }
  }

  void run_request_head_tests()
  {
    test_parse_request_head_parses_request_line_and_headers();
    test_parse_request_head_preserves_header_order_and_empty_values();
    test_parse_request_head_rejects_malformed_request_line_variants();
    test_parse_request_head_rejects_invalid_header_variants();
    test_parse_request_head_rejects_invalid_http_version_variants();
    test_validate_request_head_size_rejects_oversized_request_head();
    test_head_reader_accepts_fragmented_request_head();
    test_head_reader_returns_incomplete_on_peer_close_before_boundary();
    test_head_reader_accepts_exact_limit_request_head();
    test_head_reader_rejects_limit_plus_one_request_head();
    test_property_generated_valid_request_heads_parse();
    test_property_generated_invalid_request_lines_reject();
    test_property_generated_invalid_headers_reject();
    test_property_generated_partial_request_heads_never_complete();
    test_property_generated_near_boundary_size_classification();
  }
}
