# UvNVR 0.7.0

Brand-camera fallback, MSI packaging and smoother codec transitions.

## Highlights

- **Hikvision/Dahua without ONVIF.** A new HTTP Digest client talks to
  Hikvision ISAPI and Dahua CGI. **Auto detect** now tries ONVIF first and then
  these vendor APIs (or the selected brand first), filling the brand, name,
  model and stream template. Camera time sync uses the same fallback, so
  Hikvision/Dahua clocks can be set even when ONVIF is disabled.
- **Per-channel HTTP port.** Channels store an `httpPort` (default 80) separately
  from the RTSP port; it is used for ONVIF/brand probing, event subscriptions
  and time synchronization. The channel form exposes the field.
- **x64 MSI.** A WiX MSI is built next to the NSIS setup. It supports
  `msiexec /i ... HTTPPORT=9090 RTSPPORT=9554`, creates desktop and Start-menu
  shortcuts, the boot scheduled task and firewall rule, writes a UTF-8 JSON
  config, and removes only its own artifacts on uninstall (data and config are
  preserved). The release workflow installs and uninstalls it as a smoke test.
- **Codec-boundary continuation.** Playback automatically starts the next
  segment with a fresh player when H.264/H.265 changes, so the operator does not
  have to seek manually.

## API

`POST /api/v1/channels/detect` with `{host, httpPort, username, password, brand?}`
returns `{protocol, name, model, brand, mainUrl, subUrl, subEnabled,
eventsEnabled, httpPort}`. `brand` may be `hikvision` or `dahua` to try the
vendor API first.

## Upgrade notes

- Existing channels default to `httpPort = 80`; set it if the camera's HTTP
  service is on another port.
- The MSI is per-machine and x64. Arm64 and portable installs continue to use the
  NSIS setup and portable zip. The MSI is unsigned, so SmartScreen may warn.
- The MSI preserves `%ProgramData%\UvNVR\uvnvr.json` and the data directory on
  upgrade and uninstall.
- The brand HTTP time formats are vendor best-effort: Hikvision receives local
  time with an offset, Dahua receives local time. Some firmware may require a
  time zone setting in its own UI.

## Known limitations

- Pure RTSP cameras have no control channel; time sync and auto-detect require
  ONVIF, Hikvision ISAPI or Dahua CGI.
- Reverse playback remains timed backward stepping; smooth reverse would need
  decode/re-encode or GOP reordering.
- MSE cannot change codec inside one player; the new automatic continuation has a
  short re-buffer at the boundary.
- The MSI is x64 only and unsigned. Proprietary Hikvision SADP/Dahua discovery
  multicast is not implemented; use ONVIF discovery or the IP-based detect form.
