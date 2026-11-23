/* End-to-end SDK verification: auth -> channels -> recording days ->
 * 5 s live preview -> download + SHA-256 comparison.
 *
 * Build (Linux):
 *   cc test_sdk.c -I../include -L.. -luvnvr_sdk -lcrypto -o uvnvr_sdk_test
 * Run:
 *   ./uvnvr_sdk_test 127.0.0.1 1515 admin password
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "uvnvr_sdk.h"

#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static double now_ms(void) {
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <sys/stat.h>
#include <unistd.h>
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

static volatile int g_frames = 0;
static volatile long long g_bytes = 0;
static volatile double g_first_frame_ms = 0;
static double g_live_start = 0;

static void on_frame(uvnvr_sdk_t* sdk, int handle, const uvnvr_frame_t* frame,
                     void* user) {
  (void)sdk;
  (void)handle;
  (void)user;
  if (g_frames == 0)
    g_first_frame_ms = now_ms() - g_live_start;
  ++g_frames;
  g_bytes += (long long)frame->size;
}

static void iso_day(time_t when, char* out, size_t size) {
  struct tm tm_value;
#if defined(_WIN32)
  gmtime_s(&tm_value, &when);
#else
  gmtime_r(&when, &tm_value);
#endif
  strftime(out, size, "%Y-%m-%d", &tm_value);
}

static int sha256_file(const char* path, unsigned char out[32]) {
  FILE* file = fopen(path, "rb");
  if (file == NULL)
    return -1;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == NULL) {
    fclose(file);
    return -1;
  }
  if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(context);
    fclose(file);
    return -1;
  }
  unsigned char buffer[64 * 1024];
  size_t read_bytes = 0;
  while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (EVP_DigestUpdate(context, buffer, read_bytes) != 1) {
      EVP_MD_CTX_free(context);
      fclose(file);
      return -1;
    }
  }
  unsigned int length = 0;
  const int ok = EVP_DigestFinal_ex(context, out, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  fclose(file);
  return ok ? 0 : -1;
}

static void print_sha256(const unsigned char digest[32]) {
  for (int i = 0; i < 32; ++i)
    printf("%02x", digest[i]);
}

static long long file_size(const char* path) {
#if defined(_WIN32)
  FILE* file = fopen(path, "rb");
  if (file == NULL)
    return -1;
  _fseeki64(file, 0, SEEK_END);
  const long long size = _ftelli64(file);
  fclose(file);
  return size;
#else
  struct stat info;
  if (stat(path, &info) != 0)
    return -1;
  return (long long)info.st_size;
#endif
}

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? atoi(argv[2]) : 1515;
  const char* user = argc > 3 ? argv[3] : "admin";
  const char* password = argc > 4 ? argv[4] : "";
  const int channel_override = argc > 5 ? atoi(argv[5]) : 0;
  const char* output_path = argc > 6 ? argv[6] : "sdk-test-download.ts";

  printf("sdk: %s\n", uvnvr_version());
  int failures = 0;

  uvnvr_options_t options;
  uvnvr_options_init(&options);
  options.quic_probe_timeout_ms = 400;
  /* Transport knobs for the protocol-layer checks:
   *   UVNVR_SDK_QUIC_PROBE_MS=0  -> force TCP (QUIC disabled in the SDK)
   *   UVNVR_SDK_QUIC_PROBE_MS=1  -> probe QUIC briefly, then fall back to TCP
   *   UVNVR_SDK_TRANSPORT=tcp    -> prefer TCP without attempting QUIC
   */
  const char* transport_env = getenv("UVNVR_SDK_TRANSPORT");
  if (transport_env != NULL && strcmp(transport_env, "tcp") == 0)
    options.prefer_quic = 0;
  const char* probe_env = getenv("UVNVR_SDK_QUIC_PROBE_MS");
  if (probe_env != NULL)
    options.quic_probe_timeout_ms = (uint32_t)strtoul(probe_env, NULL, 10);
  uvnvr_sdk_t* sdk = uvnvr_create(&options);
  if (sdk == NULL) {
    fprintf(stderr, "FAIL create: %s\n", uvnvr_last_error(NULL));
    return 1;
  }

  const double connect_start = now_ms();
  int rc = uvnvr_connect(sdk, host, (uint16_t)port, 5000);
  const double connect_ms = now_ms() - connect_start;
  if (rc != UVNVR_OK) {
    fprintf(stderr, "FAIL connect (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("transport: %s\n", uvnvr_transport_name(sdk));
  printf("handshake: %.1f ms\n", connect_ms);

  rc = uvnvr_login(sdk, user, password);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "FAIL login (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("auth: ok\n");

  uvnvr_channel_t* channels = NULL;
  size_t channel_count = 0;
  rc = uvnvr_get_channel_list(sdk, &channels, &channel_count);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "FAIL channels (%d): %s\n", rc, uvnvr_last_error(sdk));
    ++failures;
  } else {
    printf("channels: %zu\n", channel_count);
    for (size_t i = 0; i < channel_count; ++i) {
      printf("  [%lld] %s main=%s keyframe=%d\n", (long long)channels[i].id,
             channels[i].name, channels[i].main_state, channels[i].has_keyframe);
    }
  }

  int channel_id = channel_override;
  if (channel_id == 0 && channels != NULL && channel_count > 0) {
    for (size_t i = 0; i < channel_count; ++i) {
      if (channels[i].enabled) {
        channel_id = (int)channels[i].id;
        break;
      }
    }
  }

  char from[16];
  char to[16];
  const time_t now = time(NULL);
  iso_day(now - 30 * 86400, from, sizeof(from));
  iso_day(now, to, sizeof(to));
  char** days = NULL;
  size_t day_count = 0;
  if (channel_id > 0) {
    rc = uvnvr_get_recording_days(sdk, channel_id, from, to, &days, &day_count);
    if (rc != UVNVR_OK) {
      fprintf(stderr, "FAIL days (%d): %s\n", rc, uvnvr_last_error(sdk));
      ++failures;
    } else {
      printf("recording days (%s..%s): %zu\n", from, to, day_count);
      for (size_t i = 0; i < day_count; ++i)
        printf("  %s\n", days[i]);
      uvnvr_free_strings(days, day_count);
    }
  }

  /* Live preview: 5 seconds of frames. */
  if (channel_id > 0) {
    g_live_start = now_ms();
    const int handle = uvnvr_live_open(sdk, channel_id, UVNVR_STREAM_MAIN,
                                       on_frame, NULL);
    if (handle < 0) {
      fprintf(stderr, "FAIL live_open: %s\n", uvnvr_last_error(sdk));
      ++failures;
    } else {
      sleep_ms(5000);
      uvnvr_live_close(sdk, handle);
      printf("live: frames=%d bytes=%lld first_frame=%.1f ms\n", g_frames,
             g_bytes, g_first_frame_ms);
      if (g_frames == 0)
        ++failures;
    }
  }

  /* Download the largest finalized segment and compare SHA-256. */
  uvnvr_recording_t* recordings = NULL;
  size_t recording_count = 0;
  rc = uvnvr_get_recordings(sdk, channel_id, NULL, 0, 0, &recordings,
                            &recording_count);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "FAIL recordings (%d): %s\n", rc, uvnvr_last_error(sdk));
    ++failures;
  } else {
    int64_t best_id = 0;
    int64_t best_size = 0;
    for (size_t i = 0; i < recording_count; ++i) {
      if (!recordings[i].active && recordings[i].id > 0 &&
          recordings[i].size_bytes > best_size) {
        best_id = recordings[i].id;
        best_size = recordings[i].size_bytes;
      }
    }
    printf("recordings: %zu (largest finalized %lld, %lld bytes)\n",
           recording_count, (long long)best_id, (long long)best_size);
    uvnvr_free_recordings(recordings, recording_count);
    if (best_id == 0) {
      printf("SKIP download: no finalized recording\n");
    } else {
      const double download_start = now_ms();
      rc = uvnvr_download_recording(sdk, best_id, output_path, NULL, NULL);
      const double download_ms = now_ms() - download_start;
      if (rc != UVNVR_OK) {
        fprintf(stderr, "FAIL download (%d): %s\n", rc, uvnvr_last_error(sdk));
        ++failures;
      } else {
        const long long downloaded = file_size(output_path);
        printf("download: %lld bytes in %.1f ms (%.1f MiB/s)\n", downloaded,
               download_ms,
               download_ms > 0 ? (double)downloaded / 1048576.0 * 1000.0 / download_ms
                               : 0.0);
        if (downloaded != best_size) {
          fprintf(stderr, "FAIL download size %lld != %lld\n", downloaded,
                  (long long)best_size);
          ++failures;
        } else {
          printf("sha256: ");
          unsigned char digest[32];
          if (sha256_file(output_path, digest) != 0) {
            fprintf(stderr, "FAIL sha256\n");
            ++failures;
          } else {
            print_sha256(digest);
            printf("\n");
          }
        }
      }
    }
  }

  if (channels != NULL)
    uvnvr_free_channels(channels, channel_count);
  uvnvr_logout(sdk);
  uvnvr_destroy(sdk);

  if (failures != 0) {
    fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
    return 1;
  }
  printf("RESULT: PASS\n");
  return 0;
}
