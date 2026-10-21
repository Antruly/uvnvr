# Deployment

## Packages

Release packages install under `/opt/uvnvr` and bundle the application, static LGPL FFmpeg, web assets and required shared libraries.

### Linux

- Debian/Ubuntu: `sudo dpkg -i uvnvr_0.18.0_amd64.deb`
- RHEL/Fedora: `sudo rpm -i uvnvr-0.18.0-1.x86_64.rpm`
- Portable: extract the tar.gz and run `/opt/uvnvr/bin/uvnvr`.

The systemd unit runs as `uvnvr`, stores data in `/var/lib/uvnvr`, binds HTTP 8080 and RTSP 554 (with `CAP_NET_BIND_SERVICE`). Open `http://<host>:8080` and complete first-run setup.

### Windows

**The Inno Setup installer** installs under `Program Files\UvNVR`, data under `%ProgramData%\UvNVR` (or `<install>\data` when installed on another drive), adds a firewall rule and offers a **Startup** task page. With **Register UvNVR as a Windows service** selected (the default, recommended for production) the installer creates the `UvNVR` service with automatic startup, starts it, and the single desktop shortcut opens the web UI. Without it, the desktop shortcut launches the console process with `--open`, which also opens the browser. The uninstaller stops and deletes the service, removes the firewall rule and preserves the data directory. When the executable is installed under Program Files and no `--data-dir` is given, UvNVR defaults to `%ProgramData%\UvNVR`; a portable copy outside Program Files defaults to the `data` directory next to it (for `bin\uvnvr.exe`, that is `<root>\data`). Run a portable copy with `bin\uvnvr.exe --data-dir <dir>` (extracting under Program Files is not writable for normal users). `ffmpeg.exe` must remain next to the executable or be named by `UVNVR_FFMPEG`.

The service can also be managed after installation from **Settings → System service**:
register it (administrator rights required), or unregister it. Both actions
restart the recorder; after unregistering, UvNVR returns as a console process
and the page reconnects. The service runs as `LocalSystem` and logs to
`%ProgramData%\UvNVR\logs\service.log`.

An **x64 MSI** is also published. It is per-machine and accepts port properties:

```bat
msiexec /i uvnvr-0.7.0-windows-x64.msi HTTPPORT=9090 RTSPPORT=9554 /qn
```

The MSI registers the same Windows service (it is a machine-wide package, so
there is no opt-out), creates the same desktop/Start-menu shortcuts and firewall
rule, writes `%ProgramData%\UvNVR\uvnvr.json` (or `<install>\data` for another
drive), and preserves the config/data on uninstall. It is unsigned, so Windows
SmartScreen may warn. Arm64 and portable installs use the Inno Setup installer/zip.

### macOS

The pkg installs under `/opt/uvnvr`, a launchd daemon `com.antruly.uvnvr` and data under `/Library/Application Support/UvNVR`. The dmg/zip are unsigned or ad-hoc signed; Gatekeeper may require a right-click Open or `xattr` adjustment. Provide a Developer ID for notarization in CI secrets when available.

## Configuration

Ports, bind address, the data directory and TLS are read from a JSON file at
startup. Linux packages create `/etc/uvnvr/uvnvr.json`; the systemd unit and the
macOS launchd job pass it with `--config`. The Windows installer writes
`%ProgramData%\UvNVR\uvnvr.json` after asking for the HTTP and RTSP ports, and
creates a **UvNVR Configuration** Start-menu shortcut for editing it.

Precedence is command-line flags, then environment variables, then the file,
then built-in defaults. `uvnvr --print-config` prints the merged settings. See
[`configuration.md`](configuration.md) for the schema, search paths and
validation rules. Restart the service after editing the file.

## Manual run

```bash
UVNVR_FFMPEG=/opt/uvnvr/bin/ffmpeg \
  /opt/uvnvr/bin/uvnvr --config /etc/uvnvr/uvnvr.json
```

Or pass the values directly:

```bash
/opt/uvnvr/bin/uvnvr --port 8080 --rtsp-port 554 --data-dir /var/lib/uvnvr
```

`--open` opens the web UI in the default browser once the server is listening;
the Windows console shortcuts use it. `--service` is the Windows SCM entry point
and is only meaningful when the process was started by the service manager.

## System service

**Settings → System service** shows whether the platform service is registered,
running and enabled at boot, and offers **Register as system service** /
**Unregister service**. Both operations restart the recorder and need
administrator (Windows) or root (Linux) rights.

- Windows: registers the `UvNVR` service with `sc.exe` (`start= auto`,
  `obj= LocalSystem`). The installers do the same; the Inno Setup wizard lets you
  skip it for a console-only test installation.
- Linux: writes `/etc/systemd/system/uvnvr.service` with the current executable,
  configuration and data directory, enables it, and starts it as root. Release
  packages instead ship `/lib/systemd/system/uvnvr.service`, which runs as the
  dedicated `uvnvr` user with `CAP_NET_BIND_SERVICE`.
- macOS: registration is owned by the pkg (launchd); the web UI reports the
  state but directs changes to the package or `launchctl`.

Unregistering stops the service and starts a console instance again (with the
browser opened) so the web UI stays reachable. The service logs to
`%ProgramData%\UvNVR\logs\service.log` on Windows and to the journal
(`journalctl -u uvnvr`) on Linux.

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
