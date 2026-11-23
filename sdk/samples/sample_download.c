/* Downloads the largest finalized recording to a file.
 *
 *   sample_download 127.0.0.1 1515 admin password /tmp/segment.ts
 */
#include "uvnvr_sdk.h"

#include <stdio.h>
#include <stdlib.h>

static long long g_last_percent = -1;

static void on_progress(uvnvr_sdk_t* sdk, int64_t received, int64_t total,
                        void* user) {
  (void)sdk;
  (void)user;
  if (total <= 0)
    return;
  const long long percent = (long long)((received * 100) / total);
  if (percent / 10 != g_last_percent / 10) {
    g_last_percent = percent;
    printf("  %lld%% (%lld / %lld bytes)\n", percent, (long long)received,
           (long long)total);
  }
}

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? atoi(argv[2]) : 1515;
  const char* user = argc > 3 ? argv[3] : "admin";
  const char* password = argc > 4 ? argv[4] : "";
  const char* path = argc > 5 ? argv[5] : "recording.ts";

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

  uvnvr_recording_t* recordings = NULL;
  size_t count = 0;
  rc = uvnvr_get_recordings(sdk, 0, NULL, 0, 0, &recordings, &count);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "recording list failed: %s\n", uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  int64_t best_id = 0;
  int64_t best_size = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!recordings[i].active && recordings[i].id > 0 &&
        recordings[i].size_bytes > best_size) {
      best_id = recordings[i].id;
      best_size = recordings[i].size_bytes;
    }
  }
  uvnvr_free_recordings(recordings, count);
  if (best_id == 0) {
    fprintf(stderr, "no finalized recording is available\n");
    uvnvr_destroy(sdk);
    return 2;
  }

  printf("downloading recording %lld (%lld bytes) to %s over %s\n",
         (long long)best_id, (long long)best_size, path, uvnvr_transport_name(sdk));
  rc = uvnvr_download_recording(sdk, best_id, path, on_progress, NULL);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "download failed (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("download complete\n");
  uvnvr_logout(sdk);
  uvnvr_destroy(sdk);
  return 0;
}
