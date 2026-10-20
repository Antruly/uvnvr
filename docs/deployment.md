# Deployment

## Packages

Release packages install under `/opt/uvnvr` and bundle the application, static LGPL FFmpeg, web assets and required shared libraries.

### Linux

- Debian/Ubuntu: `sudo dpkg -i uvnvr_0.1.0_amd64.deb`
- RHEL/Fedora: `sudo rpm -i uvnvr-0.1.0-1.x86_64.rpm`
- Portable: extract the tar.gz and run `/opt/uvnvr/bin/uvnvr`.

The systemd unit runs as `uvnvr`, stores data in `/var/lib/uvnvr`, binds HTTP 8080 and RTSP 554 (with `CAP_NET_BIND_SERVICE`). Open `http://<host>:8080` and complete first-run setup.

### Windows

The NSIS installer installs under `Program Files\UvNVR`, data under `%ProgramData%\UvNVR`, adds a firewall rule and a startup scheduled task. When the executable is installed under Program Files and no `--data-dir` is given, UvNVR defaults to `%ProgramData%\UvNVR`; a portable copy extracted outside Program Files keeps the historical `.\data` default. Run a portable copy with `bin\uvnvr.exe --data-dir <dir>` (extracting under Program Files is not writable for normal users). `ffmpeg.exe` must remain next to the executable or be named by `UVNVR_FFMPEG`.

### macOS

The pkg installs under `/opt/uvnvr`, a launchd daemon `com.antruly.uvnvr` and data under `/Library/Application Support/UvNVR`. The dmg/zip are unsigned or ad-hoc signed; Gatekeeper may require a right-click Open or `xattr` adjustment. Provide a Developer ID for notarization in CI secrets when available.

## Manual run

```bash
UVNVR_FFMPEG=/opt/uvnvr/bin/ffmpeg \
  /opt/uvnvr/bin/uvnvr --port 8080 --rtsp-port 554 --data-dir /var/lib/uvnvr
```

## Ports

| Port | Purpose |
|---|---|
| 8080 | HTTP web UI/API |
| 8443 | Optional HTTPS (self-signed in development) |
| 554 / 8554 / 8555 | RTSP output fallback chain |

## Data and backup

`<data-dir>/uvnvr.db` contains configuration, index, users and audit data. `<data-dir>/secret.key` decrypts camera/SMTP credentials; back it up with the database. Recordings live under the configured storage volume; deleting them manually requires maintenance mode.

## Maintenance mode

Start the service with `--maintenance` to expose destructive operations (video/log deletion, serial/version changes, disk formatting). These operations are audited and should only be used with an approved change window.
