// Command-line probe for the GM/T 0024 adapter.
//
//   gm_tls_cli --print-capabilities
//   gm_tls_cli --mode server --protocol tlcp --port 14433 \
//              --cert sign.pem --key sign-key.pem \
//              --enc-cert enc.pem --enc-key enc-key.pem --ca ca.pem
//   gm_tls_cli --mode client --protocol tlcp --host 127.0.0.1 --port 14433 \
//              --ca ca.pem --sni localhost --message hello
//
// The tool exists so a handshake can be exercised and captured without
// starting the full NVR. It is not part of the server binary.

#include "gb35114/tls_gm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using uvnvr::gb35114::GmTlsCredentials;
using uvnvr::gb35114::GmTlsOptions;
using uvnvr::gb35114::GmTlsProtocol;
using uvnvr::gb35114::GmTlsSession;
using uvnvr::gb35114::GmTlsStatus;
using uvnvr::gb35114::ToString;

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void CloseSocket(Socket socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

struct Options {
  std::string mode = "client";
  std::string protocol = "tlcp";
  std::string host = "127.0.0.1";
  int port = 443;
  std::string cert;
  std::string key;
  std::string enc_cert;
  std::string enc_key;
  std::string chain;
  std::string enc_chain;
  std::string ca;
  std::string sni;
  std::string cipher;
  std::string message = "ping from uvnvr gm tls client";
  bool insecure = false;
  bool require_client_cert = false;
  bool print_capabilities = false;
};

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void PrintCapabilities() {
  const auto capabilities = uvnvr::gb35114::QueryGmTlsCapabilities();
  std::printf("backend=%s version=%s tlcp=%s gm_tls13=%s\n", capabilities.backend.c_str(),
              capabilities.version.c_str(), capabilities.tlcp ? "yes" : "no",
              capabilities.gm_tls13 ? "yes" : "no");
  std::printf("tlcp_ciphers=");
  for (std::size_t i = 0; i < capabilities.tlcp_cipher_suites.size(); ++i)
    std::printf("%s%s", i ? "," : "", capabilities.tlcp_cipher_suites[i].c_str());
  std::printf("\ntls13_ciphers=");
  for (std::size_t i = 0; i < capabilities.tls13_cipher_suites.size(); ++i)
    std::printf("%s%s", i ? "," : "", capabilities.tls13_cipher_suites[i].c_str());
  std::printf("\n");
}

void Usage() {
  std::puts(
      "usage: gm_tls_cli [options]\n"
      "  --print-capabilities        print the linked backend capabilities\n"
      "  --mode server|client        default client\n"
      "  --protocol tlcp|tls13       default tlcp (GM/T 0024 / GB/T 38636)\n"
      "  --host HOST                 client peer, default 127.0.0.1\n"
      "  --port PORT                 default 443\n"
      "  --cert FILE --key FILE      signing certificate/key (PEM)\n"
      "  --enc-cert FILE --enc-key FILE  TLCP encryption certificate/key\n"
      "  --chain FILE                signing chain (PEM)\n"
      "  --enc-chain FILE            encryption chain (PEM)\n"
      "  --ca FILE                   trust anchors (PEM), enables verification\n"
      "  --sni NAME                  TLS SNI and certificate hostname\n"
      "  --cipher LIST               colon-separated suites\n"
      "  --require-client-cert       server asks for a client certificate\n"
      "  --insecure                  do not verify the peer chain\n"
      "  --message TEXT              client message, default ping\n");
}

bool Parse(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&](std::string* out) {
      if (i + 1 >= argc)
        return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--help") {
      Usage();
      return false;
    }
    if (arg == "--print-capabilities") {
      options->print_capabilities = true;
    } else if (arg == "--mode") {
      if (!value(&options->mode))
        return false;
    } else if (arg == "--protocol") {
      if (!value(&options->protocol))
        return false;
    } else if (arg == "--host") {
      if (!value(&options->host))
        return false;
    } else if (arg == "--port") {
      std::string text;
      if (!value(&text))
        return false;
      options->port = std::atoi(text.c_str());
    } else if (arg == "--cert") {
      if (!value(&options->cert))
        return false;
    } else if (arg == "--key") {
      if (!value(&options->key))
        return false;
    } else if (arg == "--enc-cert") {
      if (!value(&options->enc_cert))
        return false;
    } else if (arg == "--enc-key") {
      if (!value(&options->enc_key))
        return false;
    } else if (arg == "--chain") {
      if (!value(&options->chain))
        return false;
    } else if (arg == "--enc-chain") {
      if (!value(&options->enc_chain))
        return false;
    } else if (arg == "--ca") {
      if (!value(&options->ca))
        return false;
    } else if (arg == "--sni") {
      if (!value(&options->sni))
        return false;
    } else if (arg == "--cipher") {
      if (!value(&options->cipher))
        return false;
    } else if (arg == "--message") {
      if (!value(&options->message))
        return false;
    } else if (arg == "--insecure") {
      options->insecure = true;
    } else if (arg == "--require-client-cert") {
      options->require_client_cert = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

bool LoadCredentials(const Options& options, GmTlsCredentials* credentials, std::string* error) {
  if (!options.cert.empty()) {
    credentials->sign_certificate_pem = ReadFile(options.cert);
    if (credentials->sign_certificate_pem.empty()) {
      *error = "cannot read " + options.cert;
      return false;
    }
  }
  if (!options.key.empty()) {
    credentials->sign_private_key_pem = ReadFile(options.key);
    if (credentials->sign_private_key_pem.empty()) {
      *error = "cannot read " + options.key;
      return false;
    }
  }
  if (!options.enc_cert.empty()) {
    credentials->enc_certificate_pem = ReadFile(options.enc_cert);
    if (credentials->enc_certificate_pem.empty()) {
      *error = "cannot read " + options.enc_cert;
      return false;
    }
  }
  if (!options.enc_key.empty()) {
    credentials->enc_private_key_pem = ReadFile(options.enc_key);
    if (credentials->enc_private_key_pem.empty()) {
      *error = "cannot read " + options.enc_key;
      return false;
    }
  }
  if (!options.chain.empty()) {
    credentials->sign_chain_pem = ReadFile(options.chain);
    if (credentials->sign_chain_pem.empty()) {
      *error = "cannot read " + options.chain;
      return false;
    }
  }
  if (!options.enc_chain.empty()) {
    credentials->enc_chain_pem = ReadFile(options.enc_chain);
    if (credentials->enc_chain_pem.empty()) {
      *error = "cannot read " + options.enc_chain;
      return false;
    }
  }
  if (!options.ca.empty()) {
    credentials->ca_pem = ReadFile(options.ca);
    if (credentials->ca_pem.empty()) {
      *error = "cannot read " + options.ca;
      return false;
    }
  }
  credentials->verify_peer = !options.insecure;
  return true;
}

GmTlsOptions BuildOptions(const Options& options) {
  GmTlsOptions tls;
  tls.protocol = options.protocol == "tls13" ? GmTlsProtocol::GmTls13 : GmTlsProtocol::Tlcp;
  tls.cipher_suites = options.cipher;
  tls.server_name = options.sni;
  tls.require_client_cert = options.require_client_cert;
  return tls;
}

void PrintSession(const GmTlsSession& session) {
  std::printf("handshake=ok protocol=%s cipher=%s peer_verified=%s\n",
              session.NegotiatedProtocol().c_str(), session.NegotiatedCipher().c_str(),
              session.PeerVerified() ? "yes" : "no");
  const std::string subject = session.PeerSubject();
  const std::string issuer = session.PeerIssuer();
  if (!subject.empty())
    std::printf("peer_subject=%s\n", subject.c_str());
  if (!issuer.empty())
    std::printf("peer_issuer=%s\n", issuer.c_str());
  std::fflush(stdout);
}

int ReadMessage(GmTlsSession& session, std::string* message) {
  char buffer[4096];
  for (;;) {
    std::size_t received = 0;
    std::string error;
    const GmTlsStatus status = session.Read(buffer, sizeof(buffer), &received, &error);
    if (status == GmTlsStatus::Ok) {
      message->append(buffer, received);
      if (message->find('\n') != std::string::npos)
        return 0;
      continue;
    }
    if (status == GmTlsStatus::Closed)
      return message->empty() ? -1 : 0;
    if (status == GmTlsStatus::Error) {
      std::fprintf(stderr, "read: %s\n", error.c_str());
      return -1;
    }
  }
}

int SendMessage(GmTlsSession& session, std::string message) {
  if (message.empty() || message.back() != '\n')
    message.push_back('\n');
  std::size_t total = 0;
  while (total < message.size()) {
    std::size_t sent = 0;
    std::string error;
    const GmTlsStatus status =
        session.Write(message.data() + total, message.size() - total, &sent, &error);
    if (status == GmTlsStatus::Ok) {
      total += sent;
      continue;
    }
    if (status == GmTlsStatus::Error) {
      std::fprintf(stderr, "write: %s\n", error.c_str());
      return -1;
    }
  }
  return 0;
}

int RunServer(const Options& options) {
  std::string error;
  GmTlsCredentials credentials;
  if (!LoadCredentials(options, &credentials, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto context =
      uvnvr::gb35114::GmTlsContext::CreateServer(credentials, BuildOptions(options), &error);
  if (!context) {
    std::fprintf(stderr, "server context: %s\n", error.c_str());
    return 1;
  }

#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif

  Socket listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == kInvalidSocket) {
    std::perror("socket");
    return 1;
  }
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<unsigned short>(options.port));
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::perror("bind");
    CloseSocket(listener);
    return 1;
  }
  if (listen(listener, 1) != 0) {
    std::perror("listen");
    CloseSocket(listener);
    return 1;
  }
  std::printf("listening port=%d protocol=%s\n", options.port, options.protocol.c_str());
  std::fflush(stdout);

  Socket peer = accept(listener, nullptr, nullptr);
  CloseSocket(listener);
  if (peer == kInvalidSocket) {
    std::perror("accept");
    return 1;
  }
#ifdef SO_RCVTIMEO
  timeval timeout{};
  timeout.tv_sec = 10;
  setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
#endif

  GmTlsSession session = GmTlsSession::Accept(context, static_cast<int>(peer), &error);
  if (!error.empty()) {
    std::fprintf(stderr, "accept: %s\n", error.c_str());
    CloseSocket(peer);
    return 1;
  }
  const GmTlsStatus status = uvnvr::gb35114::HandshakeBlocking(session, &error);
  if (status != GmTlsStatus::Ok) {
    std::fprintf(stderr, "handshake: %s (%s)\n", ToString(status), error.c_str());
    CloseSocket(peer);
    return 1;
  }
  PrintSession(session);

  std::string message;
  if (ReadMessage(session, &message) == 0) {
    std::printf("received=%s", message.c_str());
    if (message.empty() || message.back() != '\n')
      std::printf("\n");
    std::fflush(stdout);
    SendMessage(session, "gm-tls-server: " + message);
  }
  session.Shutdown(&error);
  CloseSocket(peer);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

int RunClient(const Options& options) {
  std::string error;
  GmTlsCredentials credentials;
  if (!LoadCredentials(options, &credentials, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto context = uvnvr::gb35114::GmTlsContext::CreateClient(credentials, BuildOptions(options),
                                                            &error);
  if (!context) {
    std::fprintf(stderr, "client context: %s\n", error.c_str());
    return 1;
  }

#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    std::fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string port = std::to_string(options.port);
  if (getaddrinfo(options.host.c_str(), port.c_str(), &hints, &results) != 0) {
    std::fprintf(stderr, "cannot resolve %s\n", options.host.c_str());
    return 1;
  }

  Socket socket_handle = kInvalidSocket;
  for (addrinfo* entry = results; entry; entry = entry->ai_next) {
    socket_handle = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket_handle == kInvalidSocket)
      continue;
    if (connect(socket_handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0)
      break;
    CloseSocket(socket_handle);
    socket_handle = kInvalidSocket;
  }
  freeaddrinfo(results);
  if (socket_handle == kInvalidSocket) {
    std::fprintf(stderr, "cannot connect to %s:%d\n", options.host.c_str(), options.port);
    return 1;
  }
#ifdef SO_RCVTIMEO
  timeval timeout{};
  timeout.tv_sec = 10;
  setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
#endif

  GmTlsSession session = GmTlsSession::Connect(context, static_cast<int>(socket_handle), &error);
  if (!error.empty()) {
    std::fprintf(stderr, "connect: %s\n", error.c_str());
    CloseSocket(socket_handle);
    return 1;
  }
  const GmTlsStatus status = uvnvr::gb35114::HandshakeBlocking(session, &error);
  if (status != GmTlsStatus::Ok) {
    std::fprintf(stderr, "handshake: %s (%s)\n", ToString(status), error.c_str());
    CloseSocket(socket_handle);
    return 1;
  }
  PrintSession(session);

  if (SendMessage(session, options.message) != 0) {
    CloseSocket(socket_handle);
    return 1;
  }
  std::string reply;
  if (ReadMessage(session, &reply) == 0) {
    std::printf("reply=%s", reply.c_str());
    if (reply.empty() || reply.back() != '\n')
      std::printf("\n");
  }
  session.Shutdown(&error);
  CloseSocket(socket_handle);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
  Options options;
  if (!Parse(argc, argv, &options))
    return 1;
  if (options.print_capabilities) {
    PrintCapabilities();
    return 0;
  }
  if (options.mode == "server")
    return RunServer(options);
  if (options.mode == "client")
    return RunClient(options);
  std::fprintf(stderr, "unknown mode: %s\n", options.mode.c_str());
  return 1;
}
