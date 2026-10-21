# Startup configuration

UvNVR reads startup settings (HTTP/RTSP ports, bind address, data directory,
TLS and FFmpeg path) from a JSON file. Command-line flags override the file,
and the file overrides built-in defaults.

## Precedence

1. Command-line flags (`--port`, `--rtsp-port`, `--data-dir`, ...)
2. Environment variables (`UVNVR_PORT`, `UVNVR_RTSP_PORT`, ...)
3. The JSON configuration file
4. Built-in defaults

`--print-config` shows the merged result, including which file was loaded:

```bash
uvnvr --print-config
uvnvr --config /etc/uvnvr/uvnvr.json --print-config
```

## File lookup

An explicitly requested file is used as-is. Without `--config`, UvNVR checks
`UVNVR_CONFIG`, then the first existing file in this order:

| Platform | Search order |
|---|---|
| All | `<executable directory>/uvnvr.json` |
| All | `<parent of executable directory>/uvnvr.json` |
| Windows | `%ProgramData%\UvNVR\uvnvr.json` |
| macOS | `/Library/Application Support/UvNVR/uvnvr.json` |
| Linux | `/etc/uvnvr/uvnvr.json` |

The Windows installer writes the ProgramData config file and starts the service
with `--config` pointing at it. The config's `dataDir` is `%ProgramData%\UvNVR`
for a C: install and `<install>\data` when UvNVR is installed on another drive;
the uninstaller preserves the data directory. Linux packages use `/etc/uvnvr/uvnvr.json`; the
macOS pkg uses the Application Support file. Portable Windows zips contain
`uvnvr.json` next to `bin\`.

## Schema

All keys are optional.

```json
{
  "http": {
    "port": 8080,
    "host": "0.0.0.0",
    "tls": {
      "cert": "/etc/uvnvr/tls/server.crt",
      "key": "/etc/uvnvr/tls/server.key",
      "selfSigned": false
    }
  },
  "rtsp": {
    "port": 554
  },
  "dataDir": "/var/lib/uvnvr",
  "maintenance": false,
  "ffmpeg": {
    "path": "/opt/uvnvr/bin/ffmpeg"
  },
  "logging": {
    "level": "info"
  },
  "storage": {
    "cacheMb": 128,
    "readaheadMb": 4
  }
}
```

| Key | Default | Notes |
|---|---|---|
| `http.port` | `8080` | Web UI and REST API. Must differ from `rtsp.port`. |
| `http.host` | `0.0.0.0` | HTTP bind address. Use `127.0.0.1` behind a reverse proxy. |
| `http.tls.cert` / `http.tls.key` | empty | Both must be set to enable HTTPS. |
| `http.tls.selfSigned` | `false` | Generates a localhost certificate when no cert/key is set. |
| `rtsp.port` | `554` | Falls back to 8554/8555 when the port cannot be bound. |
| `dataDir` | `./data` | Database, secret key, alerts and default recordings root. Windows installs under Program Files default to `%ProgramData%\UvNVR`; installs on another drive default to `<install>\data`. |
| `maintenance` | `false` | Enables administrator-only destructive operations (recording deletion, audit purge, serial/display-version changes, volume formatting). Prefer `--maintenance` for a temporary operations session. |
| `ffmpeg.path` | auto-detected | FFmpeg executable; `UVNVR_FFMPEG` overrides it. |
| `logging.level` | `info` | `trace`, `debug`, `info`, `warn` or `error`. |
| `storage.cacheMb` | `128` | LRU playback cache in MiB; `0` disables caching. |
| `storage.readaheadMb` | `4` | Bytes of the next segment preloaded while the current one plays; `0` disables read-ahead. |

Flat aliases are accepted for hand-written files: `port`, `rtspPort`,
`dataDir`, `host`, `ffmpegPath`, `logLevel`, `maintenance`, `tlsCert`,
`tlsKey` and `tlsSelfSigned`. The nested form wins when both are present.
Unknown keys are ignored.

## Environment variables

| Variable | Overrides |
|---|---|
| `UVNVR_CONFIG` | Configuration file path |
| `UVNVR_PORT` | `http.port` |
| `UVNVR_HOST` | `http.host` |
| `UVNVR_RTSP_PORT` | `rtsp.port` |
| `UVNVR_DATA_DIR` | `dataDir` |
| `UVNVR_FFMPEG` | `ffmpeg.path` |
| `UVNVR_LOG_LEVEL` | `logging.level` |
| `UVNVR_CACHE_MB` | `storage.cacheMb` |
| `UVNVR_READAHEAD_MB` | `storage.readaheadMb` |
| `UVNVR_TLS_CERT` / `UVNVR_TLS_KEY` | TLS certificate and key |

## Applying changes

Ports, bind address, the data directory and TLS settings are applied at
startup. Edit the file and restart the service (or the container):

```bash
# Linux
sudo systemctl restart uvnvr

# Windows (elevated)
schtasks /End /TN UvNVR
schtasks /Run /TN UvNVR
```

The System page shows the effective ports, bind address, data directory and
configuration path under **Startup configuration**.

Changing the data directory does not move an existing database. Copy
`uvnvr.db`, `secret.key` and recordings to the new location first.

## Validation

The service refuses to start (and `--print-config` exits non-zero) when:

- a port is outside 1–65535, or HTTP and RTSP use the same port;
- only one of `tls.cert` / `tls.key` is set;
- `logging.level` is not one of the accepted values;
- the JSON is invalid or a value has the wrong type.
