/* Logs in, lists channels and recording metadata.
 *
 * Linux/macOS:
 *   cc sample_basic.c -I../include -L. -luvnvr_sdk -o sample_basic
 *   ./sample_basic 127.0.0.1 1515 admin 'password'
 *
 * Windows (x64, Developer Command Prompt):
 *   cl sample_basic.c /I..\include uvnvr_sdk.lib
 *   sample_basic.exe 127.0.0.1 1515 admin password
 */
#include "uvnvr_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void iso_day(time_t when, char* out, size_t size) {
  struct tm tm_value;
#if defined(_WIN32)
  gmtime_s(&tm_value, &when);
#else
  gmtime_r(&when, &tm_value);
#endif
  strftime(out, size, "%Y-%m-%d", &tm_value);
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
    fprintf(stderr, "create failed: %s\n", uvnvr_last_error(NULL));
    return 1;
  }

  int rc = uvnvr_connect(sdk, host, (uint16_t)port, 5000);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "connect failed (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("connected via %s\n", uvnvr_transport_name(sdk));

  rc = uvnvr_login(sdk, user, password);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "login failed (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }

  uvnvr_channel_t* channels = NULL;
  size_t channel_count = 0;
  rc = uvnvr_get_channel_list(sdk, &channels, &channel_count);
  if (rc != UVNVR_OK) {
    fprintf(stderr, "channel list failed (%d): %s\n", rc, uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("channels: %zu\n", channel_count);
  for (size_t i = 0; i < channel_count; ++i) {
    printf("  [%lld] %-24s %-6s %s:%d main=%s keyframe=%d\n",
           (long long)channels[i].id, channels[i].name, channels[i].protocol,
           channels[i].host, channels[i].port, channels[i].main_state,
           channels[i].has_keyframe);
  }
  if (channel_count == 0 && channels != NULL) {
    /* Empty list: the allocation is still valid and must be released. */
  }

  char from[16];
  char to[16];
  const time_t now = time(NULL);
  iso_day(now - 30 * 86400, from, sizeof(from));
  iso_day(now, to, sizeof(to));
  char** days = NULL;
  size_t day_count = 0;
  rc = uvnvr_get_recording_days(sdk, channel_id, from, to, &days, &day_count);
  if (rc == UVNVR_OK) {
    printf("recording days for channel %d (%s..%s): %zu\n", channel_id, from, to,
           day_count);
    for (size_t i = 0; i < day_count; ++i)
      printf("  %s\n", days[i]);
    uvnvr_free_strings(days, day_count);
  } else {
    fprintf(stderr, "recording days failed (%d): %s\n", rc, uvnvr_last_error(sdk));
  }

  uvnvr_recording_t* recordings = NULL;
  size_t recording_count = 0;
  rc = uvnvr_get_recordings(sdk, channel_id, NULL, 0, 0, &recordings,
                            &recording_count);
  if (rc == UVNVR_OK) {
    printf("recordings for channel %d: %zu\n", channel_id, recording_count);
    for (size_t i = 0; i < recording_count && i < 20; ++i) {
      printf("  id=%lld type=%s codec=%s %lld bytes\n",
             (long long)recordings[i].id, recordings[i].record_type,
             recordings[i].codec, (long long)recordings[i].size_bytes);
    }
    uvnvr_free_recordings(recordings, recording_count);
  } else {
    fprintf(stderr, "recording list failed (%d): %s\n", rc, uvnvr_last_error(sdk));
  }

  uvnvr_free_channels(channels, channel_count);
  uvnvr_logout(sdk);
  uvnvr_destroy(sdk);
  return 0;
}
