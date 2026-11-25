// Unit tests for the GB/T 28181 SIP message layer: URI/address parsing,
// header lookup, message framing, Digest authentication and backoff.

#include "gb28181/sip.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace uvnvr::gb28181;

int failures = 0;

void Expect(const std::string& name, const std::string& actual, const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectView(const std::string& name, std::string_view actual, std::string_view expected) {
  Expect(name, std::string(actual), std::string(expected));
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectSize(const std::string& name, std::size_t actual, std::size_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %zu, expected %zu\n", name.c_str(), actual, expected);
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), haystack.c_str(),
               needle.c_str());
  ++failures;
}

void TestUri() {
  SipUri uri;
  std::string error;
  ExpectBool("uri/simple parse",
             ParseSipUri("sip:34020000001320000001@3402000000", &uri, &error), true);
  Expect("uri/scheme", uri.scheme, "sip");
  Expect("uri/user", uri.user, "34020000001320000001");
  Expect("uri/host", uri.host, "3402000000");
  ExpectSize("uri/port", uri.port, 0);
  Expect("uri/format", FormatSipUri(uri), "sip:34020000001320000001@3402000000");

  SipUri complex;
  ExpectBool("uri/complex parse",
             ParseSipUri("sip:alice@[2001:db8::1]:5070;transport=tcp;lr?subject=test", &complex,
                         &error),
             true);
  Expect("uri/ipv6 host", complex.host, "2001:db8::1");
  ExpectSize("uri/port", complex.port, 5070);
  Expect("uri/transport param", complex.Parameter("TRANSPORT"), "tcp");
  Expect("uri/lr param", complex.Parameter("lr"), "");
  Expect("uri/header", complex.Header("subject"), "test");
  Expect("uri/format complex", FormatSipUri(complex),
         "sip:alice@[2001:db8::1]:5070;transport=tcp;lr?subject=test");

  ExpectBool("uri/reject http", ParseSipUri("http://example.com", &uri, &error), false);
  ExpectBool("uri/reject empty", ParseSipUri("sip:", &uri, &error), false);
  ExpectBool("uri/reject bad port", ParseSipUri("sip:a@b:70000", &uri, &error), false);
  ExpectBool("uri/reject unbracketed v6", ParseSipUri("sip:a@2001:db8::1", &uri, &error), false);
}

void TestAddress() {
  SipAddress address;
  std::string error;
  ExpectBool("address/name-addr",
             ParseSipAddress("\"Camera 1\" <sip:34020000001320000001@3402000000>;tag=abc123",
                             &address, &error),
             true);
  Expect("address/display", address.display_name, "Camera 1");
  Expect("address/user", address.uri.user, "34020000001320000001");
  Expect("address/tag", address.Parameter("tag"), "abc123");
  Expect("address/format", FormatSipAddress(address),
         "\"Camera 1\" <sip:34020000001320000001@3402000000>;tag=abc123");

  SipAddress bare;
  ExpectBool("address/bare", ParseSipAddress("sip:user@host;tag=xyz;transport=udp", &bare, &error),
             true);
  Expect("address/bare tag", bare.Parameter("tag"), "xyz");
  Expect("address/bare transport", bare.Parameter("transport"), "udp");
}

void TestMessage() {
  const std::string wire =
      "REGISTER sip:34020000002000000001@3402000000 SIP/2.0\r\n"
      "v: SIP/2.0/UDP 192.168.1.10:5060;rport;branch=z9hG4bK12345\r\n"
      "f: <sip:34020000001320000001@3402000000>;tag=fromtag\r\n"
      "t: <sip:34020000001320000001@3402000000>\r\n"
      "i: 1234567890@192.168.1.10\r\n"
      "CSeq: 42 REGISTER\r\n"
      "m: <sip:34020000001320000001@192.168.1.10:5060;transport=udp>\r\n"
      "Max-Forwards: 70\r\n"
      "Expires: 3600\r\n"
      "l: 0\r\n"
      "\r\n";

  SipMessage message;
  std::size_t consumed = 0;
  std::string error;
  ExpectBool("message/parse",
             SipMessage::Parse(wire, &message, &consumed, &error) == SipParseStatus::kComplete,
             true);
  ExpectSize("message/consumed", consumed, wire.size());
  ExpectBool("message/is request", message.is_request, true);
  ExpectBool("message/method", message.method == SipMethod::kRegister, true);
  Expect("message/request uri", FormatSipUri(message.request_uri),
         "sip:34020000002000000001@3402000000");
  Expect("message/callid compact", message.CallId(), "1234567890@192.168.1.10");
  ExpectSize("message/cseq", message.CSeqNumber(), 42);
  ExpectBool("message/cseq method", message.CSeqMethod() == SipMethod::kRegister, true);
  Expect("message/from tag", message.FromTag(), "fromtag");
  Expect("message/via via compact", message.GetHeader("Via"),
         "SIP/2.0/UDP 192.168.1.10:5060;rport;branch=z9hG4bK12345");
  Expect("message/contact", message.GetHeader("Contact"),
         "<sip:34020000001320000001@192.168.1.10:5060;transport=udp>");
  ExpectSize("message/body", message.body.size(), 0);

  const std::string canonical = message.Serialize();
  SipMessage reparsed;
  ExpectBool("message/roundtrip parse",
             SipMessage::Parse(canonical, &reparsed, &consumed, &error) ==
                 SipParseStatus::kComplete,
             true);
  Expect("message/roundtrip callid", reparsed.CallId(), message.CallId());
  ExpectSize("message/roundtrip cseq", reparsed.CSeqNumber(), 42);
  Expect("message/roundtrip uri", FormatSipUri(reparsed.request_uri),
         FormatSipUri(message.request_uri));
}

void TestResponse() {
  const std::string wire =
      "SIP/2.0 401 Unauthorized\r\n"
      "Via: SIP/2.0/UDP 192.168.1.10:5060;rport=5060;received=192.168.1.10;branch=z9hG4bK1\r\n"
      "From: <sip:34020000001320000001@3402000000>;tag=fromtag\r\n"
      "To: <sip:34020000001320000001@3402000000>;tag=servertag\r\n"
      "Call-ID: abc@192.168.1.10\r\n"
      "CSeq: 42 REGISTER\r\n"
      "WWW-Authenticate: Digest realm=\"3402000000\", nonce=\"deadbeef\", "
      "algorithm=MD5, qop=\"auth\"\r\n"
      "Content-Length: 0\r\n"
      "\r\n";

  SipMessage message;
  std::string error;
  ExpectBool("response/parse",
             SipMessage::Parse(wire, &message, nullptr, &error) == SipParseStatus::kComplete, true);
  ExpectBool("response/is response", message.is_request, false);
  ExpectSize("response/status", static_cast<std::size_t>(message.status_code), 401);
  Expect("response/reason", message.reason_phrase, "Unauthorized");
  Expect("response/to tag", message.ToTag(), "servertag");
  ExpectSize("response/cseq", message.CSeqNumber(), 42);

  SipDigestChallenge challenge;
  ExpectBool("response/challenge parse",
             ParseSipDigestChallenge(message.GetHeader("WWW-Authenticate"), &challenge, &error),
             true);
  Expect("response/realm", challenge.realm, "3402000000");
  Expect("response/nonce", challenge.nonce, "deadbeef");
  Expect("response/algorithm", challenge.algorithm, "MD5");
  ExpectSize("response/qop count", challenge.qop.size(), 1);
  Expect("response/qop", challenge.qop.empty() ? std::string() : challenge.qop.front(), "auth");
}

void TestFraming() {
  const std::string first =
      "MESSAGE sip:34020000002000000001@3402000000 SIP/2.0\r\n"
      "Via: SIP/2.0/TCP 127.0.0.1:40000;branch=z9hG4bK1\r\n"
      "From: <sip:1@d>;tag=a\r\n"
      "To: <sip:2@d>\r\n"
      "Call-ID: c1@127.0.0.1\r\n"
      "CSeq: 7 MESSAGE\r\n"
      "Content-Type: Application/MANSCDP+xml\r\n"
      "Content-Length: 11\r\n"
      "\r\n"
      "<Notify/>\r\n";
  const std::string second =
      "SIP/2.0 200 OK\r\n"
      "Via: SIP/2.0/TCP 127.0.0.1:40000;branch=z9hG4bK1\r\n"
      "From: <sip:1@d>;tag=a\r\n"
      "To: <sip:2@d>;tag=b\r\n"
      "Call-ID: c1@127.0.0.1\r\n"
      "CSeq: 7 MESSAGE\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  const std::string stream = first + second;

  SipMessage message;
  std::size_t consumed = 0;
  std::string error;
  ExpectBool("framing/first",
             SipMessage::Parse(stream, &message, &consumed, &error) == SipParseStatus::kComplete,
             true);
  ExpectSize("framing/first consumed", consumed, first.size());
  Expect("framing/first body", message.body, "<Notify/>\r\n");

  SipMessage response;
  const std::string_view rest(stream.data() + consumed, stream.size() - consumed);
  ExpectBool("framing/second",
             SipMessage::Parse(rest, &response, &consumed, &error) == SipParseStatus::kComplete,
             true);
  ExpectSize("framing/second consumed", consumed, second.size());
  ExpectSize("framing/second status", static_cast<std::size_t>(response.status_code), 200);
}

void TestIncompleteAndInvalid() {
  SipMessage message;
  std::string error;
  ExpectBool("parse/incomplete headers",
             SipMessage::Parse("REGISTER sip:x SIP/2.0\r\nVia: a\r\n", &message, nullptr, &error) ==
                 SipParseStatus::kIncomplete,
             true);
  const std::string short_body =
      "MESSAGE sip:x SIP/2.0\r\nContent-Length: 10\r\n\r\nabc";
  ExpectBool("parse/incomplete body",
             SipMessage::Parse(short_body, &message, nullptr, &error) ==
                 SipParseStatus::kIncomplete,
             true);
  ExpectBool("parse/invalid request",
             SipMessage::Parse("WHAT sip:x\r\n\r\n", &message, nullptr, &error) ==
                 SipParseStatus::kInvalid,
             true);
  ExpectBool("parse/invalid status",
             SipMessage::Parse("SIP/2.0 XX Bad\r\n\r\n", &message, nullptr, &error) ==
                 SipParseStatus::kInvalid,
             true);
  ExpectBool("parse/invalid content-length",
             SipMessage::Parse("MESSAGE sip:x SIP/2.0\r\nContent-Length: abc\r\n\r\n", &message,
                               nullptr, &error) == SipParseStatus::kInvalid,
             true);
}

void TestHeaderFoldingAndNames() {
  const std::string wire =
      "OPTIONS sip:platform@3402000000 SIP/2.0\r\n"
      "Via: SIP/2.0/UDP 127.0.0.1:5060\r\n"
      "  ;branch=z9hG4bKfolded\r\n"
      "From: <sip:34020000002000000001@3402000000>;tag=t\r\n"
      "To: <sip:34020000001320000001@3402000000>\r\n"
      "Call-ID: fold@host\r\n"
      "CSeq: 1 OPTIONS\r\n"
      "Content-Length: 0\r\n"
      "\r\n";
  SipMessage message;
  std::string error;
  ExpectBool("folding/parse",
             SipMessage::Parse(wire, &message, nullptr, &error) == SipParseStatus::kComplete, true);
  Expect("folding/via", message.GetHeader("Via"), "SIP/2.0/UDP 127.0.0.1:5060 ;branch=z9hG4bKfolded");

  ExpectBool("header/compact v", HeaderNameEquals("v", "Via"), true);
  ExpectBool("header/compact i", HeaderNameEquals("i", "Call-ID"), true);
  ExpectBool("header/compact l", HeaderNameEquals("l", "Content-Length"), true);
  ExpectBool("header/case", HeaderNameEquals("content-length", "Content-Length"), true);
  Expect("header/canonical", CanonicalHeaderName("m"), "Contact");
  Expect("header/canonical unknown", CanonicalHeaderName("X-Custom"), "X-Custom");
}

void TestDigest() {
  Expect("digest/md5 empty", SipDigestMd5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
  Expect("digest/md5 abc", SipDigestMd5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");

  SipDigestChallenge challenge;
  std::string error;
  ExpectBool(
      "digest/challenge",
      ParseSipDigestChallenge(
          "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
          "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"",
          &challenge, &error),
      true);
  Expect("digest/realm", challenge.realm, "testrealm@host.com");
  Expect("digest/nonce", challenge.nonce, "dcd98b7102dd2f0e8b11d0f600bfb0c093");
  ExpectSize("digest/qop count", challenge.qop.size(), 2);

  // RFC 2617 section 3.5 example.
  SipDigestCredentials credentials{"Mufasa", "Circle Of Life"};
  std::string authorization;
  ExpectBool("digest/build",
             BuildSipDigestAuthorization("GET", "/dir/index.html", challenge, credentials,
                                         "0a4f113b", 1, &authorization, &error),
             true);
  ExpectContains("digest/rfc2617 response", authorization,
                 "response=\"6629fae49393a05397450978507c4ef1\"");
  ExpectContains("digest/qop output", authorization, "qop=auth");
  ExpectContains("digest/nc output", authorization, "nc=00000001");
  ExpectContains("digest/opaque output", authorization,
                 "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"");

  SipDigestChallenge md5sess;
  ExpectBool("digest/md5-sess challenge",
             ParseSipDigestChallenge(
                 "Digest realm=\"r\", nonce=\"n\", algorithm=MD5-sess, qop=\"auth\"", &md5sess,
                 &error),
             true);
  ExpectBool("digest/md5-sess build",
             BuildSipDigestAuthorization("REGISTER", "sip:r", md5sess, credentials, "cn", 1,
                                         &authorization, &error),
             true);
  ExpectContains("digest/md5-sess algorithm", authorization, "algorithm=MD5-sess");

  SipDigestChallenge unsupported;
  ExpectBool("digest/unsupported challenge",
             ParseSipDigestChallenge("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-256",
                                     &unsupported, &error),
             true);
  ExpectBool("digest/unsupported build",
             BuildSipDigestAuthorization("REGISTER", "sip:r", unsupported, credentials, "cn", 1,
                                         &authorization, &error),
             false);
}

void TestBackoff() {
  ExpectSize("backoff/attempt1", ComputeRegistrationBackoffMs(1, 500, 8000), 500);
  ExpectSize("backoff/attempt2", ComputeRegistrationBackoffMs(2, 500, 8000), 1000);
  ExpectSize("backoff/attempt3", ComputeRegistrationBackoffMs(3, 500, 8000), 2000);
  ExpectSize("backoff/attempt4", ComputeRegistrationBackoffMs(4, 500, 8000), 4000);
  ExpectSize("backoff/attempt5", ComputeRegistrationBackoffMs(5, 500, 8000), 8000);
  ExpectSize("backoff/cap", ComputeRegistrationBackoffMs(32, 500, 8000), 8000);
  ExpectSize("backoff/zero attempt", ComputeRegistrationBackoffMs(0, 500, 8000), 500);
}

}  // namespace

int main() {
  TestUri();
  TestAddress();
  TestMessage();
  TestResponse();
  TestFraming();
  TestIncompleteAndInvalid();
  TestHeaderFoldingAndNames();
  TestDigest();
  TestBackoff();

  if (failures == 0) {
    std::printf("sip_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "sip_test: %d check(s) failed\n", failures);
  return 1;
}
