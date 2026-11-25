# GB/T 28181 PS/RTP media and RTCP dialogs (P6.2c)

This stage implements the media half of the lower-device role: H.264/H.265
elementary streams are wrapped in the MPEG-2 program stream described by
GB/T 28181 Appendix C, carried as RTP payload type 96 over UDP, TCP active or
TCP passive, and controlled through RTCP. SDP build/parse plus a media dialog
manager bind the stream to SIP INVITE/ACK/BYE.

The module is opt-in and is not linked into the server yet:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DUVNVR_ENABLE_GB28181=ON -DUVNVR_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Targets:

| Target | Purpose |
| --- | --- |
| `uvnvr_gb28181_ps` | static library: PS, RTP, RTCP and the media dialog manager |
| `gb28181_ps_cli` | send a raw Annex B file as PS/RTP |
| `gb28181_session_cli` | answer one SDP offer, stream, consume RTCP, close on BYE |
| `gb28181_ps_test` | PS header, RTP header, splitter and SDP unit tests |
| `gb28181_rtcp_test` | RTCP SR/RR/SDES/BYE/PLI/FIR/NACK layout tests |
| `gb28181_media_session_test` | real-socket UDP/TCP active/passive lifecycle tests |

No GPL/LGPL library is used. The implementation needs only the C++ standard
library and the platform sockets API (Winsock on Windows).

## PS layout (Appendix C)

`PsMuxer::MuxFrame` emits one PS pack per video access unit:

```
offset  bytes            field
0       00 00 01 BA      pack_start_code (MPEG-2 pack header, 14 bytes)
14      00 00 01 BB      system_header, present on the first frame and key frames
29      00 00 01 BC      program_stream_map, same repetition rule
...     00 00 01 E0      video PES, one per 64 KiB of elementary stream
```

The system header declares one video stream. The PSM carries one entry:

| Field | H.264 | H.265 |
| --- | --- | --- |
| `stream_type` | `0x1B` | `0x24` |
| `elementary_stream_id` | `0xE0` | `0xE0` |
| PSM CRC-32/MPEG-2 | `0x32141109` for the fixed one-stream PSM | `0xCA811C3A` |

The CRC uses polynomial `0x04C11DB7`, initial value `0xFFFFFFFF`, no final xor
and MSB-first input. `ParsePsPack` recomputes it, so a corrupt PSM fails the
unit test.

Example from `build/tests/gb28181_ps_test`:

```
PS header (first 64 bytes):
0000  00 00 01 ba 44 00 04 00 04 01 00 5f 6b f8 00 00  |....D......_k...|
0010  01 bb 00 09 80 2f b5 00 e1 7f e0 e0 00 00 00 01  |...../..........|
0020  bc 00 0e e0 ff 00 00 00 04 1b e0 00 00 32 14 11  |.............2..|
0030  09 00 00 01 e0 00 26 84 80 05 21 00 01 00 01 00  |......&...!.....|
pack: offset=0 size=14 scr=0 mux_rate=6106
system header: offset=14 length=9 stream_id=0xE0
PSM: offset=29 length=14 stream_type=0x1B es_id=0xE0 crc=0x32141109 valid=1
PES: offset=49 stream_id=0xE0 pts=0 alignment=1 payload=30
```

Field mapping:

* `44 00 04 00 04 01 00 5f 6b f8` = `'01'` marker, SCR base 0, SCR extension
  0, `program_mux_rate` 6106, reserved bits, zero stuffing.
* `00 09` = system header length 9 (six fixed bytes plus one three-byte stream
  entry); `80 2f b5 00 e1 7f` are the rate/audio/video flags and
  `e0 e0 00` is stream id `0xE0` with `buffer_bound_scale=1`.
* `00 0e` = PSM length 14 (10-byte body plus the 4-byte CRC);
  `e0 ff 00 00 00 04 1b e0 00 00` is the one H.264 entry;
  `32 14 11 09` is the CRC.
* `00 26` = PES packet length, `84` = `'10'` marker + data alignment,
  `80` = PTS only, `05` = five-byte timestamp, `21 00 01 00 01` = PTS 0.

PTS/DTS use the MPEG-2 33-bit form. Frames larger than 65,000 bytes are split
into several PES packets inside the same pack; only the first PES carries
PTS/DTS, and only the first PES has `data_alignment_indicator=1`.

## RTP

Each RTP packet is the standard 12-byte header followed directly by a slice of
the PS pack (there is no RFC 2250 MPEG payload header). `RtpPacketizer`:

* payload type 96 (`a=rtpmap:96 PS/90000`), version 2;
* one sequence number per packet, wrapping at 65536;
* the frame PTS in the 90 kHz clock in every packet of that frame;
* the configured SSRC in every packet;
* marker bit 1 only on the last packet of the access unit, so the last packet
  of an I frame is marked and no delta-frame packet is.

IP fragmentation is avoided by the default 1,400-byte payload limit. TCP media
uses a two-byte big-endian length before every RTP packet (RFC 4571), which is
the framing GB/T 28181 platforms use for both TCP modes.

## SDP

UDP:

```text
v=0
o=34020000001320000001 0 0 IN IP4 192.168.1.10
s=Play
c=IN IP4 192.168.1.10
t=0 0
m=video 5004 RTP/AVP 96
a=sendonly
a=rtpmap:96 PS/90000
y=0000000001
```

TCP passive (the usual device role):

```text
m=video 6000 TCP/RTP/AVP 96
a=sendonly
a=rtpmap:96 PS/90000
a=setup:passive
a=connection:new
y=0000000001
```

`BuildSdpAnswer` mirrors the setup role for an INVITE: an offer with
`a=setup:active` is answered with `a=setup:passive`, and the reverse. The
device always answers `a=sendonly` for real-time video. `ParseSdp` accepts
CRLF or LF, global or media-level `c=`, `y=`, `a=setup`, `a=connection:new`
and the direction attributes, and rejects offers that are not PS/90000.

## RTCP

`RtpSessionConfig::enable_rtcp` opens the control channel. It is off by
default for the PS/RTP-only path, so enabling RTCP never changes an existing
caller's sockets.

* UDP: a second socket binds to the RTP port + 1 (RFC 3550 convention). When
  the caller asks for an ephemeral port, the session retries until it obtains
  an even RTP port with a free RTCP port above it. The platform sends RR/PLI/
  FIR to `<answer rtp_port> + 1`.
* TCP active/passive: RTCP is framed on the same media connection with the
  two-byte RFC 4571 length prefix. There is no extra port; a frame is RTCP
  when its payload type byte is 200..207, otherwise it is ignored as RTP.
* `Tick(now_ms)` emits a compound Sender Report + SDES CNAME every
  `sender_report_interval_ms` (default 5 s). The SR carries the negotiated
  SSRC, NTP timestamp, last RTP timestamp, packet count and payload octet
  count.
* Received Receiver Reports update fraction lost, cumulative lost, jitter,
  highest sequence and (when LSR/DLSR match the last SR) RTT.
* A PLI (PSFB FMT 1) or FIR (PSFB FMT 4) whose media SSRC is 0 or ours raises
  one `on_keyframe_request` callback. FIR sequence numbers are preserved.
  Generic NACK (RTPFB FMT 1) is parsed for diagnostics.
* An inbound BYE closes the media session; `OnBye`/`Close` sends a BYE with
  the session SSRC when the device leaves first.

## INVITE / ACK / BYE binding

`MediaSessionManager` owns the dialog state:

```
INVITE (offer) -> kWaiting -> ACK -> kStreaming -> BYE/RTCP BYE -> kClosed
                      |                  |
                      +-- ACK timeout ---+
```

* `OnInvite` parses the offer, allocates an SSRC and an even RTP port, opens
  UDP/TCP active/TCP passive, and returns the 200 OK SDP answer. A SIP
  retransmission (same Call-ID + CSeq + body) returns the cached answer
  without opening a second socket. A re-INVITE replaces the old session.
* TCP passive binds the listener during `OnInvite`; ACK and `Tick` retry a
  non-blocking `accept()` so the platform may connect before or after ACK.
* `OnAck` moves the dialog to streaming. Frames are refused before that.
* `OnBye` sends RTCP BYE, closes the socket and keeps a closed snapshot for
  diagnostics. An inbound RTCP BYE follows the same close path.
* `HandleSipRequest("INVITE"/"ACK"/"BYE", ...)` is the adapter for the SIP
  user agent. The SIP stack itself only needs to supply Call-ID, CSeq, content
  type and the SDP body.

Key-frame requests are advisory. `ConsumeKeyframeRequest` lets a pumping loop
notice a PLI/FIR and skip ahead to the next IDR; `SendFrame(..., keyframe=true)`
then records the request as honoured. A live encoder integration should call
the encoder's force-IDR instead of dropping delta frames.

## CLI and verification

Generate a short elementary stream and send it over UDP:

```sh
ffmpeg -f lavfi -i testsrc=size=320x240:rate=25 -t 2 \
  -c:v libx264 -preset ultrafast -x264-params keyint=25:scenecut=0:aud=1 \
  -f h264 /tmp/es.h264

build/gb28181_ps_cli --input /tmp/es.h264 --codec h264 \
  --transport udp --remote 127.0.0.1 --port 5004 \
  --ps-out /tmp/received.ps --sdp-out /tmp/stream.sdp --verbose
```

TCP passive:

```sh
build/gb28181_ps_cli --input /tmp/es.h264 --codec h264 \
  --transport tcp-passive --local 127.0.0.1 --local-port 6000 \
  --ps-out /tmp/received.ps
```

The integration script runs both codecs over both required transports, receives
the RTP packets, validates the header fields, reassembles the PS, and decodes it
with ffprobe/ffmpeg:

```sh
python3 tests/integration/gb28181_ps_rtp.py --cli build/gb28181_ps_cli
```

Example result:

```
== h264 over udp ==
sender: frames=50 keyframes=2 ps_bytes=41887 rtp_packets=59 marker_packets=50 ...
receiver: rtp_packets=59 frames=50 keyframes=2 marker_packets=50 seq=0..58 ts=0..176400 delta=3600
I-frame timestamps with marker=1: 0, 90000
PS: sent_sha256=4c5b... received_sha256=4c5b... match=yes
ffprobe: codec_name=h264 width=320 height=240 avg_frame_rate=25/1
ffmpeg: decoded_frames=50 first_frame_md5=7c2c361a836f0c406a2701e957fe43e7
```

FFmpeg does not have a PS/90000 RTP depacketizer (its RTP payload table covers
MP2T, H264, H265 and others, but not PS). The integration receiver therefore
does the RTP de-encapsulation and passes the reconstructed PS to `ffmpeg -f
mpegps`; ffprobe and ffmpeg then prove that the PS structure and timestamps are
decodable. A platform with a GB/T 28181 PS depacketizer consumes the wire
packets directly.

### Dialog and RTCP check

`gb28181_session_cli` takes an SDP offer, answers it, sends ACK, streams and
handles RTCP until the platform sends BYE:

```sh
build/gb28181_session_cli --input /tmp/es.h264 --offer /tmp/offer.sdp \
  --call-id lab-1 --advertise 127.0.0.1 --sender-report-ms 500 --duration 6
```

The platform-side script drives UDP, TCP passive (platform connects) and TCP
active (device connects), sends RR, PLI and FIR, validates key-frame reactions,
sends RTCP BYE and decodes the received PS:

```sh
python3 tests/integration/gb28181_media_session.py \
    --cli build/gb28181_session_cli --codec h264 --transport all
```

Example result for the UDP case:

```
answer: mode=udp setup= rtp_port=34000 rtcp_port=34001 ssrc=0xba404785
receiver: rtp_packets=23 frames=9 keyframes=3 seq=0..22 ts=0..183600
          deltas=[3600, 3600, 3600, 3600, 75600, 3600, 86400, 3600]
RTCP: sr_sender=0xba404785 sr_packets=16 sr_octets=16145 sr_rtp_ts=93600
events: sent RR; sent PLI; keyframe after PLI at packet 10; sent FIR;
        keyframe after FIR at packet 17; sent RTCP BYE
summary: frames_sent=9 frames_skipped=43 keyframes=3 rtp_packets=23
         rtcp_sr_sent=4 rtcp_rr_recv=1 rtcp_pli=1 rtcp_fir=1
         keyframe_requests=2 honored=2 state=closed
ffprobe: codec_name=h264 width=320 height=240 avg_frame_rate=25/1
ffmpeg: decoded_frames=9
```

The timestamp deltas stay positive; the two large deltas are the deliberate
"skip deltas until the next IDR" response to PLI and FIR. All six
codec/transport combinations pass (H.264 and H.265 over UDP, TCP passive and
TCP active).

## Current limits

* The module is not linked into the server binary or the SIP user agent yet.
  The transport classes use blocking sockets; a server integration should
  drive them from the libuv loop or a dedicated media thread. The dialog
  manager's INVITE/ACK/BYE entry points are ready for that hook-up.
* Only video is muxed. Audio PS (G.711/G.722/AAC) is a later stage.
* There is no RTP retransmission (NACK is parsed but not answered) and no
  SSRC collision handling; SSRC is allocated per dialog by the media session.
* Key-frame requests are honoured by skipping to the next key frame in the
  file-driven CLI. A camera integration must force an encoder IDR instead;
  dropping deltas is only a harness behaviour.
* IPv4 only, matching the lab/GB deployment targets.
* The PS muxer expects an Annex B access unit and trusts the caller's key-frame
  flag; `SplitAnnexBAccessUnits` derives that flag from H.264 IDR / H.265 IRAP
  NAL types.
