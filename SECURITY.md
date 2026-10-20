# Security policy

## Supported versions

UvNVR is pre-release. Security fixes are applied to the `main` branch; release branches are
listed here once `0.1.0` ships.

| Version | Supported |
|---|---|
| `main` (pre-release) | Best effort |
| `0.1.x` | When released |

## Reporting a vulnerability

**Do not open a public issue for a vulnerability.**

Use GitHub's private vulnerability reporting:

<https://github.com/Antruly/uvnvr/security/advisories/new>

Include:

- affected version/commit and platform,
- a description of the issue and its impact,
- reproduction steps or a proof of concept,
- any suggested fix or mitigation.

You will receive an acknowledgement when the report is reviewed. Please allow time for a fix
before public disclosure; coordinated disclosure is appreciated.

## Scope

Especially relevant areas:

- authentication, session handling, CSRF and API tokens,
- role/permission bypass (Web, API, RTSP, download, snapshot),
- lockout bypass or denial of service against the login path,
- path traversal, arbitrary file read/write, camera-credential disclosure,
- command injection in media/ffmpeg/disk operations,
- storage format/delete guard bypass (maintenance-mode protections),
- RTSP/RTP parsing and stream isolation between clients,
- TLS/certificate handling and sensitive data in logs.

## Security expectations for deployments

- Change the admin password at first run; do not expose the service directly to the public
  internet without TLS and a firewall.
- RTSP output uses Basic/Digest authentication; use a trusted network or a VPN, because
  RTSP is not encrypted by default.
- Keep camera, FFmpeg, libuvcpp, OpenSSL and OS packages current.
- Restrict access to the data directory: it contains encrypted camera credentials and the
  key file used to decrypt them.
- Run the service as an unprivileged account where possible; grant only the capability needed
  for RTSP port 554 (`CAP_NET_BIND_SERVICE`).
- Use maintenance mode only while performing an approved operation, then restart without it.
