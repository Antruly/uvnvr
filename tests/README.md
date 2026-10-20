# Tests

Planned layout:

- `unit/` — ring buffer, timestamp normaliser, segment index, plan evaluator, RBAC, RTSP/SDP.
- `integration/` — SQLite migrations, fake RTSP camera, ONVIF SOAP fixtures, disk-full and
  crash-recovery tests.
- `e2e/` — Playwright: first-run setup, login, channel add, preview, playback.
- `fixtures/` — ONVIF XML, MPEG-TS samples, MediaMTX test configuration.

CI runs unit/integration tests from M1 onward. E2E runs on Linux with a synthetic stream.
