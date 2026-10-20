# FAQ

## Why is the camera offline?

Test the RTSP URL with VLC or `ffplay` and check the channel's last error. ONVIF probing only supports `http://` device services in 0.1.0.

## Why is there no recording?

Recording is plan-driven. Confirm the plan covers the current weekday/time, the channel is enabled and online, and an enabled storage volume exists.

## Why does H.265 not play in the browser?

MSE H.265 support depends on the browser and hardware. Use the camera's H.264 sub stream for multi-grid preview.

## RTSP port 554 fails

The service needs `CAP_NET_BIND_SERVICE` or root. It falls back to 8554/8555 and prints the active port in the startup log.

## Can I delete recordings?

Manual deletion is intentionally unavailable. Only maintenance mode opens deletion/format operations; retention/loop deletion is automatic policy.

## Email test fails with QQ Mail

Use an authorization code, not the account password, and keep TLS certificate verification enabled.

## Where are logs and data?

The audit log is stored in the SQLite data directory. Application logs go to stdout/journal. Configure the data directory with `--data-dir`.
