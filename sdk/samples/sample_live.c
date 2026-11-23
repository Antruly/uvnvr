/* Opens a live preview and prints frame statistics for ~5 seconds.
 *
 *   sample_live 127.0.0.1 1515 admin password 1
 */
#include "uvnvr_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

static volatile int g_frames = 0;
static volatile long long g_bytes = 0;
static volatile int g_first = 0;

static void on_frame(uvnvr_sdk_t* sdk, int handle, const uvnvr_frame_t* frame,
                     void* user) {
  (void)sdk;
  (void)handle;
  (void)user;
  if (!g_first) {
    g_first = 1;
    printf("first frame: %zu bytes codec=%d\n", frame->size, frame->codec);
  }
  ++g_frames;
  g_bytes += (long long)frame->size;
}

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? atoi(argv[2]) : 1515;
  const char* user = argc > 3 ? argv[3] : "admin";
  const char* password = argc > 4 ? argv[4] : "";
  const int channel_id = argc > 5 ? atoi(argv[5]) : 1;

  uvnvr_options_t options;
  uvnvr_options_init(&options);
  uvnvr_sdk_t* sdk = uvnvr_create(&options);
  if (sdk == NULL) {
    fprintf(stderr, "create failed\n");
    return 1;
  }
  int rc = uvnvr_connect(sdk, host, (uint16_t)port, 5000);
  if (rc == UVNVR_OK)
    rc = uvnvr_login(sdk, user, password);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "connect/login failed (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }

  const int handle = uvnvr_live_open(sdk, channel_id, UVNVR_STREAM_MAIN,
                                     on_frame, NULL);
  if (handle < 0) {
    fprintf(stderr, "live open failed: %s\n", uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("live handle %d on transport %s\n", handle, uvnvr_transport_name(sdk));

  sleep_ms(5000);
  uvnvr_live_close(sdk, handle);
  printf("frames=%d bytes=%lld\n", g_frames, g_bytes);
  uvnvr_logout(sdk);
  uvnvr_destroy(sdk);
  return g_frames > 0 ? 0 : 2;
}
