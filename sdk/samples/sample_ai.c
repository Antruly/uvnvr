/* Subscribes to AI plate/face events and prints them for 30 seconds.
 *
 *   sample_ai 127.0.0.1 1515 admin password [channel] [seconds]
 *
 * `channel` 0 means every channel. The recorder must have AI enabled and a
 * channel whose frames produce recognitions; the subscription itself is
 * independent of the AI settings, so the sample also works as a smoke test
 * when no event happens (it then prints "no AI events").
 */
#include "uvnvr_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

static volatile int g_events = 0;

static void on_ai_event(uvnvr_sdk_t* sdk, int handle,
                        const uvnvr_ai_event_t* event, void* user) {
  (void)sdk;
  (void)user;
  ++g_events;
  if (event->type == UVNVR_AI_EVENT_PLATE) {
    printf("[%d] plate id=%lld channel=%lld (%s) ts=%lld plate=%s norm=%s "
           "confidence=%.3f color=%s box=%d,%d,%dx%d engine=%s alert=%d\n",
           handle, (long long)event->id, (long long)event->channel_id,
           event->channel_name, (long long)event->ts, event->plate,
           event->plate_norm, event->confidence, event->color, event->box.x,
           event->box.y, event->box.w, event->box.h, event->engine,
           event->alert);
  } else {
    printf("[%d] face id=%lld channel=%lld (%s) ts=%lld quality=%.3f "
           "confidence=%.3f matched=%lld score=%.3f name=%s alert=%d\n",
           handle, (long long)event->id, (long long)event->channel_id,
           event->channel_name, (long long)event->ts, event->quality,
           event->confidence, (long long)event->matched_face_id,
           event->matched_score, event->matched_name, event->alert);
  }
  if (event->image_url != NULL && event->image_url[0] != '\0')
    printf("    image=%s crop=%s\n", event->image_url, event->crop_url);
  fflush(stdout);
}

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? atoi(argv[2]) : 1515;
  const char* user = argc > 3 ? argv[3] : "admin";
  const char* password = argc > 4 ? argv[4] : "";
  const int channel_id = argc > 5 ? atoi(argv[5]) : 0;
  const int seconds = argc > 6 ? atoi(argv[6]) : 30;

  uvnvr_options_t options;
  uvnvr_options_init(&options);
  {
    const char* transport = getenv("UVNVR_SDK_TRANSPORT");
    if (transport != NULL &&
        (strcmp(transport, "tcp") == 0 || strcmp(transport, "TCP") == 0))
      options.prefer_quic = 0;
  }
  uvnvr_sdk_t* sdk = uvnvr_create(&options);
  if (sdk == NULL) {
    fprintf(stderr, "create failed: %s\n", uvnvr_last_error(NULL));
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

  const int handle =
      uvnvr_ai_subscribe(sdk, channel_id,
                         UVNVR_AI_EVENT_PLATE | UVNVR_AI_EVENT_FACE,
                         on_ai_event, NULL);
  if (handle < 0) {
    fprintf(stderr, "AI subscribe failed: %s\n", uvnvr_last_error(sdk));
    uvnvr_destroy(sdk);
    return 1;
  }
  printf("AI subscription %d on transport %s (channel=%d, %d s)\n", handle,
         uvnvr_transport_name(sdk), channel_id, seconds);
  fflush(stdout);

  sleep_ms(seconds * 1000);
  uvnvr_ai_unsubscribe(sdk, handle);
  printf("events=%d\n", g_events);
  uvnvr_logout(sdk);
  uvnvr_destroy(sdk);
  return 0;
}
