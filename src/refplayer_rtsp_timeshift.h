#ifndef REFPLAYER_RTSP_TIMESHIFT_H
#define REFPLAYER_RTSP_TIMESHIFT_H

#include <stddef.h>
#include <stdint.h>

#include "m3u.h"

#define REFPLAYER_RTSP_OBSERVATION_ID_SIZE 33
#define REFPLAYER_RTSP_OBSERVATION_TTL_SECONDS 600
#define REFPLAYER_RTSP_SESSION_ID_SIZE 33
#define REFPLAYER_RTSP_OPEN_CLOCK_LOOKBACK_SECONDS 604800

typedef enum {
  REFPLAYER_RTSP_RANGE_NONE = 0,
  REFPLAYER_RTSP_RANGE_NPT,
  REFPLAYER_RTSP_RANGE_CLOCK
} refplayer_rtsp_range_kind_t;

typedef struct {
  char source_id[M3U_CATALOG_ID_SIZE];
  char observation_id[REFPLAYER_RTSP_OBSERVATION_ID_SIZE];
  refplayer_rtsp_range_kind_t kind;
  double npt_start;
  double npt_end;
  int64_t clock_start_epoch;
  int64_t clock_end_epoch;
  /* The upstream proved that the live session accepts the RTSP clock
   * coordinate, but did not advertise a finite retention window. RefPlayer
   * bounds requests to the product lookback above without presenting it as
   * source-proven retention. */
  int clock_open_ended;
  int64_t observed_at_epoch;
  int64_t expires_at_epoch;
  /* Authority for expiry; the wall-clock fields above are display only. */
  int64_t expires_at_monotonic_ms;
} refplayer_rtsp_observation_t;

/* The private capability surface is deliberately unavailable to ordinary
 * rtp2httpd deployments. */
int refplayer_rtsp_timeshift_enabled(void);
void refplayer_rtsp_timeshift_clear(void);

/* Parse an upstream PLAY Range value. Only complete, finite ranges are
 * accepted. Clock endpoints must carry an explicit timezone. */
int refplayer_rtsp_parse_play_range(const char *value, refplayer_rtsp_observation_t *range);

/* Recognize the open live-clock form used by IPTV servers (notably
 * ``clock=0-``). It proves a coordinate only, never a retention window. */
int refplayer_rtsp_parse_open_clock_range(const char *value,
                                          refplayer_rtsp_observation_t *range);

/* Archive PLAY replies are acknowledgements, not source-window evidence. A
 * server may acknowledge the requested seek as either a finite range or an
 * open-ended ``target-`` range. ``open_ended`` distinguishes the latter; its
 * parsed start is returned in the matching start field. */
int refplayer_rtsp_parse_archive_ack(const char *value, refplayer_rtsp_observation_t *range,
                                     int *open_ended);

/* Publish only after the caller has jointly proved PLAY success and three
 * MPEG-TS packets. The cache stores facts, never upstream URLs or credentials. */
int refplayer_rtsp_timeshift_publish(const char *source_id, const refplayer_rtsp_observation_t *range,
                                     refplayer_rtsp_observation_t *published);

/* A confirmed live session keeps typed facts renewable. The session ID is a
 * process-incarnation random value, so cleanup removes only its own fact. */
int refplayer_rtsp_timeshift_session_id(char output[REFPLAYER_RTSP_SESSION_ID_SIZE]);
int refplayer_rtsp_timeshift_activate(const char *source_id, const char *session_id,
                                      const refplayer_rtsp_observation_t *range);
void refplayer_rtsp_timeshift_deactivate(const char *source_id, const char *session_id);

/* 1 = found, 0 = unavailable/expired, -1 = invalid input. */
int refplayer_rtsp_timeshift_latest(const char *source_id, refplayer_rtsp_observation_t *observation);
int refplayer_rtsp_timeshift_find(const char *source_id, const char *observation_id,
                                  refplayer_rtsp_observation_t *observation);

/* Mint a fresh immutable observation from the newest currently active live
 * session. 1=minted, 0=no active fact, -1=invalid/unavailable. */
int refplayer_rtsp_timeshift_discover(const char *source_id, refplayer_rtsp_observation_t *observation);

int refplayer_rtsp_npt_target_is_valid(const refplayer_rtsp_observation_t *observation, double target);
int refplayer_rtsp_clock_target_is_valid(const refplayer_rtsp_observation_t *observation, int64_t target);

/* Strict canonical wire grammars used by both the resolver API and archive
 * media GET. They reject signs, exponents, non-finite values and aliases. */
int refplayer_rtsp_parse_source_id(const char *value);
int refplayer_rtsp_parse_observation_id(const char *value);
int refplayer_rtsp_parse_npt_target(const char *value, double *target);
int refplayer_rtsp_parse_upstream_npt_target(const char *value, double *target);
int refplayer_rtsp_parse_clock_target(const char *value, int64_t *target);
int refplayer_rtsp_format_npt(double value, char *output, size_t output_size);
int refplayer_rtsp_format_clock(int64_t epoch, char *output, size_t output_size);

#endif
