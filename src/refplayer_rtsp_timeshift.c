#include "refplayer_rtsp_timeshift.h"

#include "configuration.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REFPLAYER_RTSP_OBSERVATION_CAPACITY 64
#define REFPLAYER_RTSP_ACTIVE_CAPACITY 64
#define REFPLAYER_RTSP_MAX_WINDOW_SECONDS INT64_C(315360000)

typedef struct {
  char source_id[M3U_CATALOG_ID_SIZE];
  char session_id[REFPLAYER_RTSP_SESSION_ID_SIZE];
  refplayer_rtsp_observation_t range;
  uint64_t sequence;
} refplayer_rtsp_active_fact_t;

typedef struct {
  refplayer_rtsp_observation_t observations[REFPLAYER_RTSP_OBSERVATION_CAPACITY];
  refplayer_rtsp_active_fact_t active[REFPLAYER_RTSP_ACTIVE_CAPACITY];
  size_t next_slot;
  size_t next_active_slot;
  uint64_t nonce;
  uint64_t counter;
  int nonce_ready;
} refplayer_rtsp_cache_t;

static refplayer_rtsp_cache_t cache;

static int exact_timeshift_environment(void) {
  const char *value = getenv("RTP2HTTPD_REFPLAYER_TIMESHIFT");
  return value && strcmp(value, "1") == 0;
}

int refplayer_rtsp_timeshift_enabled(void) {
  return exact_timeshift_environment() && config.workers == 1 && config.r2h_token && config.r2h_token[0] != '\0';
}

static int random_nonce(uint64_t *nonce) {
  int fd;
  size_t offset = 0;
  unsigned char *bytes = (unsigned char *)nonce;

  if (!nonce)
    return -1;
  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return -1;
  while (offset < sizeof(*nonce)) {
    ssize_t count = read(fd, bytes + offset, sizeof(*nonce) - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      close(fd);
      return -1;
    }
    offset += (size_t)count;
  }
  close(fd);
  return *nonce == 0 ? -1 : 0;
}

int refplayer_rtsp_timeshift_session_id(char output[REFPLAYER_RTSP_SESSION_ID_SIZE]) {
  uint64_t first;
  uint64_t second;
  if (!output || random_nonce(&first) != 0 || random_nonce(&second) != 0)
    return -1;
  snprintf(output, REFPLAYER_RTSP_SESSION_ID_SIZE, "%016llx%016llx", (unsigned long long)first,
           (unsigned long long)second);
  return 0;
}

void refplayer_rtsp_timeshift_clear(void) {
  uint64_t nonce = 0;
  memset(&cache, 0, sizeof(cache));
  if (random_nonce(&nonce) == 0) {
    cache.nonce = nonce;
    cache.nonce_ready = 1;
  }
}

int refplayer_rtsp_parse_source_id(const char *value) {
  if (!value || strlen(value) != M3U_CATALOG_ID_SIZE - 1)
    return -1;
  for (size_t index = 0; index < M3U_CATALOG_ID_SIZE - 1; index++)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f')))
      return -1;
  return 0;
}

int refplayer_rtsp_parse_observation_id(const char *value) {
  if (!value || strlen(value) != REFPLAYER_RTSP_OBSERVATION_ID_SIZE - 1)
    return -1;
  for (size_t index = 0; index < REFPLAYER_RTSP_OBSERVATION_ID_SIZE - 1; index++)
    if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f')))
      return -1;
  return 0;
}

static int decimal_is_canonical(const char *value) {
  const char *dot = NULL;
  size_t length;

  if (!value || !(length = strlen(value)))
    return 0;
  if (value[0] == '0' && length > 1 && value[1] != '.')
    return 0;
  for (size_t index = 0; index < length; index++) {
    if (value[index] == '.') {
      if (dot || index == 0 || index + 1 == length)
        return 0;
      dot = value + index;
    } else if (!isdigit((unsigned char)value[index])) {
      return 0;
    }
  }
  if (dot && value[length - 1] == '0')
    return 0;
  return 1;
}

static int upstream_decimal_is_valid(const char *value) {
  int saw_digit = 0;
  int saw_dot = 0;
  if (!value || !value[0])
    return 0;
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
    if (isdigit(*cursor)) {
      saw_digit = 1;
    } else if (*cursor == '.' && !saw_dot) {
      saw_dot = 1;
    } else {
      return 0;
    }
  }
  return saw_digit;
}

int refplayer_rtsp_parse_npt_target(const char *value, double *target) {
  char *end = NULL;
  double parsed;
  if (!target || !decimal_is_canonical(value))
    return -1;
  errno = 0;
  parsed = strtod(value, &end);
  if (errno != 0 || !end || *end != '\0' || !isfinite(parsed) || parsed < 0)
    return -1;
  *target = parsed;
  return 0;
}

int refplayer_rtsp_parse_clock_target(const char *value, int64_t *target) {
  unsigned long long parsed;
  char *end = NULL;
  size_t length;
  if (!value || !target || !(length = strlen(value)) || length > 12 ||
      (value[0] == '0' && length > 1))
    return -1;
  for (size_t index = 0; index < length; index++)
    if (!isdigit((unsigned char)value[index]))
      return -1;
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || parsed > UINT64_C(253402300799))
    return -1;
  *target = (int64_t)parsed;
  return 0;
}

int refplayer_rtsp_format_npt(double value, char *output, size_t output_size) {
  int written;
  if (!output || output_size == 0 || !isfinite(value) || value < 0)
    return -1;
  written = snprintf(output, output_size, "%.9f", value);
  if (written < 0 || (size_t)written >= output_size)
    return -1;
  char *end = output + strlen(output);
  while (end > output && end[-1] == '0')
    *--end = '\0';
  if (end > output && end[-1] == '.')
    *--end = '\0';
  return decimal_is_canonical(output) ? 0 : -1;
}

int refplayer_rtsp_format_clock(int64_t epoch, char *output, size_t output_size) {
  struct tm value;
  time_t timestamp;
  if (!output || output_size < 17 || epoch < 0)
    return -1;
  timestamp = (time_t)epoch;
  if ((int64_t)timestamp != epoch || !gmtime_r(&timestamp, &value))
    return -1;
  if (value.tm_year + 1900 < 1970 || value.tm_year + 1900 > 9999)
    return -1;
  return strftime(output, output_size, "%Y%m%dT%H%M%SZ", &value) == 16 ? 0 : -1;
}

static int calendar_round_trips(const struct tm *input, time_t timestamp) {
  struct tm normalized;
  return input && gmtime_r(&timestamp, &normalized) && normalized.tm_year == input->tm_year &&
         normalized.tm_mon == input->tm_mon && normalized.tm_mday == input->tm_mday &&
         normalized.tm_hour == input->tm_hour && normalized.tm_min == input->tm_min &&
         normalized.tm_sec == input->tm_sec;
}

static int parse_compact_clock(const char *value, size_t *consumed, int64_t *epoch) {
  struct tm parsed;
  struct tm original;
  char buffer[17];
  char *end;
  time_t result;
  size_t length;
  size_t z_index = 15;
  if (!value || strlen(value) < 16 || value[8] != 'T')
    return -1;
  for (size_t index = 0; index < 15; index++)
    if (index != 8 && !isdigit((unsigned char)value[index]))
      return -1;
  if (value[15] == '.') {
    z_index = 16;
    while (z_index < 25 && isdigit((unsigned char)value[z_index]))
      z_index++;
    if (z_index == 16)
      return -1;
  }
  if (value[z_index] != 'Z')
    return -1;
  length = z_index + 1;
  memcpy(buffer, value, 15);
  buffer[15] = 'Z';
  buffer[16] = '\0';
  memset(&parsed, 0, sizeof(parsed));
  end = strptime(buffer, "%Y%m%dT%H%M%SZ", &parsed);
  if (!end || *end != '\0')
    return -1;
  original = parsed;
  result = timegm(&parsed);
  if (result < 0 || (uint64_t)result > UINT64_C(253402300799) || !calendar_round_trips(&original, result))
    return -1;
  *consumed = length;
  *epoch = (int64_t)result;
  return 0;
}

static int parse_extended_clock(const char *value, size_t *consumed, int64_t *epoch) {
  size_t length;
  char buffer[32];
  struct tm parsed;
  struct tm original;
  char *end;
  int offset = 0;
  time_t result;

  if (!value)
    return -1;
  if (strlen(value) >= 20 && value[19] == 'Z') {
    length = 20;
  } else if (strlen(value) >= 25 && (value[19] == '+' || value[19] == '-') && value[22] == ':') {
    length = 25;
    int hours = (value[20] - '0') * 10 + (value[21] - '0');
    int minutes = (value[23] - '0') * 10 + (value[24] - '0');
    if (!isdigit((unsigned char)value[20]) || !isdigit((unsigned char)value[21]) ||
        !isdigit((unsigned char)value[23]) || !isdigit((unsigned char)value[24]) || hours > 14 || minutes > 59)
      return -1;
    if (hours == 14 && minutes != 0)
      return -1;
    offset = (hours * 3600 + minutes * 60) * (value[19] == '+' ? 1 : -1);
  } else {
    return -1;
  }
  memcpy(buffer, value, length);
  buffer[length] = '\0';
  buffer[19] = '\0';
  memset(&parsed, 0, sizeof(parsed));
  end = strptime(buffer, "%Y-%m-%dT%H:%M:%S", &parsed);
  if (!end || *end != '\0')
    return -1;
  original = parsed;
  result = timegm(&parsed);
  if (result < 0 || !calendar_round_trips(&original, result))
    return -1;
  *consumed = length;
  *epoch = (int64_t)result - offset;
  if (*epoch < 0 || *epoch > INT64_C(253402300799))
    return -1;
  return 0;
}

static int parse_clock_endpoint(const char *value, size_t *consumed, int64_t *epoch) {
  if (parse_compact_clock(value, consumed, epoch) == 0)
    return 0;
  return parse_extended_clock(value, consumed, epoch);
}

static int parse_upstream_npt_endpoint(const char *value, double *seconds) {
  char *end = NULL;
  double parsed;
  const char *first_colon;

  if (!value || !seconds || value[0] == '\0')
    return -1;
  first_colon = strchr(value, ':');
  if (!first_colon) {
    if (!upstream_decimal_is_valid(value))
      return -1;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed) || parsed < 0)
      return -1;
    *seconds = parsed;
    return 0;
  }
  const char *second_colon = strchr(first_colon + 1, ':');
  if (!second_colon || strchr(second_colon + 1, ':') || first_colon == value || second_colon == first_colon + 1)
    return -1;
  for (const char *cursor = value; cursor < first_colon; cursor++)
    if (!isdigit((unsigned char)*cursor))
      return -1;
  char hours_text[32];
  char minutes_text[3];
  size_t hours_length = (size_t)(first_colon - value);
  size_t minutes_length = (size_t)(second_colon - first_colon - 1);
  if (hours_length >= sizeof(hours_text) || minutes_length == 0 || minutes_length > 2)
    return -1;
  memcpy(hours_text, value, hours_length);
  hours_text[hours_length] = '\0';
  memcpy(minutes_text, first_colon + 1, minutes_length);
  minutes_text[minutes_length] = '\0';
  for (size_t index = 0; index < minutes_length; index++)
    if (!isdigit((unsigned char)minutes_text[index]))
      return -1;
  errno = 0;
  unsigned long long hours = strtoull(hours_text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || hours > 1000000)
    return -1;
  unsigned long minutes = strtoul(minutes_text, &end, 10);
  if (!end || *end != '\0' || minutes > 59)
    return -1;
  errno = 0;
  if (!upstream_decimal_is_valid(second_colon + 1))
    return -1;
  double trailing_seconds = strtod(second_colon + 1, &end);
  if (errno != 0 || end == second_colon + 1 || *end != '\0' || !isfinite(trailing_seconds) ||
      trailing_seconds < 0 || trailing_seconds >= 60)
    return -1;
  parsed = (double)hours * 3600 + (double)minutes * 60 + trailing_seconds;
  if (!isfinite(parsed))
    return -1;
  *seconds = parsed;
  return 0;
}

int refplayer_rtsp_parse_upstream_npt_target(const char *value, double *target) {
  return parse_upstream_npt_endpoint(value, target);
}

int refplayer_rtsp_parse_play_range(const char *value, refplayer_rtsp_observation_t *range) {
  const char *separator;
  char start[64];
  char end[64];

  if (!value || !range)
    return -1;
  memset(range, 0, sizeof(*range));
  while (*value == ' ' || *value == '\t')
    value++;
  if (strncmp(value, "npt=", 4) == 0) {
    separator = strchr(value + 4, '-');
    if (!separator || separator == value + 4 || separator[1] == '\0' || strchr(separator + 1, '-'))
      return -1;
    if ((size_t)(separator - (value + 4)) >= sizeof(start) || strlen(separator + 1) >= sizeof(end))
      return -1;
    memcpy(start, value + 4, (size_t)(separator - (value + 4)));
    start[separator - (value + 4)] = '\0';
    snprintf(end, sizeof(end), "%s", separator + 1);
    if (parse_upstream_npt_endpoint(start, &range->npt_start) != 0 ||
        parse_upstream_npt_endpoint(end, &range->npt_end) != 0 || range->npt_end <= range->npt_start ||
        range->npt_end - range->npt_start > (double)REFPLAYER_RTSP_MAX_WINDOW_SECONDS)
      return -1;
    range->kind = REFPLAYER_RTSP_RANGE_NPT;
    return 0;
  }
  if (strncmp(value, "clock=", 6) == 0) {
    size_t first_length;
    size_t second_length;
    const char *first = value + 6;
    if (parse_clock_endpoint(first, &first_length, &range->clock_start_epoch) != 0 || first[first_length] != '-')
      return -1;
    if (parse_clock_endpoint(first + first_length + 1, &second_length, &range->clock_end_epoch) != 0 ||
        first[first_length + 1 + second_length] != '\0' || range->clock_end_epoch <= range->clock_start_epoch ||
        range->clock_end_epoch - range->clock_start_epoch > REFPLAYER_RTSP_MAX_WINDOW_SECONDS)
      return -1;
    range->kind = REFPLAYER_RTSP_RANGE_CLOCK;
    return 0;
  }
  return -1;
}

int refplayer_rtsp_parse_open_clock_range(const char *value,
                                          refplayer_rtsp_observation_t *range) {
  if (!value || !range)
    return -1;
  while (*value == ' ' || *value == '\t')
    value++;
  if (strcmp(value, "clock=0-") != 0)
    return -1;
  memset(range, 0, sizeof(*range));
  range->kind = REFPLAYER_RTSP_RANGE_CLOCK;
  range->clock_open_ended = 1;
  return 0;
}

int refplayer_rtsp_parse_response_range(const char *value, refplayer_rtsp_observation_t *range,
                                        int *open_ended) {
  const char *separator;
  char start[64];
  size_t consumed;

  if (!value || !range || !open_ended)
    return -1;
  *open_ended = 0;
  if (refplayer_rtsp_parse_play_range(value, range) == 0)
    return 0;

  memset(range, 0, sizeof(*range));
  while (*value == ' ' || *value == '\t')
    value++;
  if (strncmp(value, "npt=", 4) == 0) {
    separator = strchr(value + 4, '-');
    if (!separator || separator == value + 4 || separator[1] != '\0' ||
        (size_t)(separator - (value + 4)) >= sizeof(start))
      return -1;
    memcpy(start, value + 4, (size_t)(separator - (value + 4)));
    start[separator - (value + 4)] = '\0';
    if (parse_upstream_npt_endpoint(start, &range->npt_start) != 0)
      return -1;
    range->kind = REFPLAYER_RTSP_RANGE_NPT;
    *open_ended = 1;
    return 0;
  }
  if (strncmp(value, "clock=", 6) == 0 &&
      parse_clock_endpoint(value + 6, &consumed, &range->clock_start_epoch) == 0 &&
      value[6 + consumed] == '-' && value[7 + consumed] == '\0') {
    range->kind = REFPLAYER_RTSP_RANGE_CLOCK;
    *open_ended = 1;
    return 0;
  }
  return -1;
}

static int observation_is_live(const refplayer_rtsp_observation_t *observation, int64_t now_monotonic_ms) {
  return observation && observation->kind != REFPLAYER_RTSP_RANGE_NONE &&
         observation->expires_at_monotonic_ms > now_monotonic_ms;
}

int refplayer_rtsp_timeshift_publish(const char *source_id, const refplayer_rtsp_observation_t *range,
                                     refplayer_rtsp_observation_t *published) {
  refplayer_rtsp_observation_t observation;
  int64_t now = (int64_t)time(NULL);

  if (!refplayer_rtsp_timeshift_enabled() || refplayer_rtsp_parse_source_id(source_id) != 0 || !range ||
      range->kind == REFPLAYER_RTSP_RANGE_NONE)
    return -1;
  if (!cache.nonce_ready)
    refplayer_rtsp_timeshift_clear();
  if (!cache.nonce_ready || cache.counter == UINT64_MAX)
    return -1;
  observation = *range;
  snprintf(observation.source_id, sizeof(observation.source_id), "%s", source_id);
  cache.counter++;
  snprintf(observation.observation_id, sizeof(observation.observation_id), "%016llx%016llx",
           (unsigned long long)cache.nonce, (unsigned long long)cache.counter);
  observation.observed_at_epoch = now;
  observation.expires_at_epoch = now + REFPLAYER_RTSP_OBSERVATION_TTL_SECONDS;
  observation.expires_at_monotonic_ms =
      get_time_ms() + (int64_t)REFPLAYER_RTSP_OBSERVATION_TTL_SECONDS * 1000;
  cache.observations[cache.next_slot] = observation;
  cache.next_slot = (cache.next_slot + 1) % REFPLAYER_RTSP_OBSERVATION_CAPACITY;
  if (published)
    *published = observation;
  return 0;
}

int refplayer_rtsp_timeshift_activate(const char *source_id, const char *session_id,
                                      const refplayer_rtsp_observation_t *range) {
  refplayer_rtsp_active_fact_t *fact;
  if (!refplayer_rtsp_timeshift_enabled() || refplayer_rtsp_parse_source_id(source_id) != 0 ||
      refplayer_rtsp_parse_observation_id(session_id) != 0 || !range || range->kind == REFPLAYER_RTSP_RANGE_NONE)
    return -1;
  if (!cache.nonce_ready)
    refplayer_rtsp_timeshift_clear();
  if (!cache.nonce_ready || cache.counter == UINT64_MAX)
    return -1;
  for (size_t index = 0; index < REFPLAYER_RTSP_ACTIVE_CAPACITY; index++)
    if (strcmp(cache.active[index].source_id, source_id) == 0 &&
        strcmp(cache.active[index].session_id, session_id) == 0) {
      cache.active[index].range = *range;
      cache.active[index].sequence = ++cache.counter;
      return 0;
    }
  fact = &cache.active[cache.next_active_slot];
  cache.next_active_slot = (cache.next_active_slot + 1) % REFPLAYER_RTSP_ACTIVE_CAPACITY;
  memset(fact, 0, sizeof(*fact));
  snprintf(fact->source_id, sizeof(fact->source_id), "%s", source_id);
  snprintf(fact->session_id, sizeof(fact->session_id), "%s", session_id);
  fact->range = *range;
  fact->sequence = ++cache.counter;
  return 0;
}

void refplayer_rtsp_timeshift_deactivate(const char *source_id, const char *session_id) {
  if (!source_id || !session_id)
    return;
  for (size_t index = 0; index < REFPLAYER_RTSP_ACTIVE_CAPACITY; index++)
    if (strcmp(cache.active[index].source_id, source_id) == 0 &&
        strcmp(cache.active[index].session_id, session_id) == 0) {
      memset(&cache.active[index], 0, sizeof(cache.active[index]));
      return;
    }
}

int refplayer_rtsp_timeshift_discover(const char *source_id, refplayer_rtsp_observation_t *observation) {
  const refplayer_rtsp_active_fact_t *newest = NULL;
  const refplayer_rtsp_observation_t *learned = NULL;
  if (!refplayer_rtsp_timeshift_enabled() || refplayer_rtsp_parse_source_id(source_id) != 0 || !observation)
    return -1;
  for (size_t index = 0; index < REFPLAYER_RTSP_ACTIVE_CAPACITY; index++)
    if (strcmp(cache.active[index].source_id, source_id) == 0 &&
        (!newest || cache.active[index].sequence > newest->sequence))
      newest = &cache.active[index];
  if (!newest)
    for (size_t offset = 0; offset < REFPLAYER_RTSP_OBSERVATION_CAPACITY; offset++) {
      size_t index = (cache.next_slot + REFPLAYER_RTSP_OBSERVATION_CAPACITY - 1 - offset) %
                     REFPLAYER_RTSP_OBSERVATION_CAPACITY;
      if (strcmp(cache.observations[index].source_id, source_id) == 0 &&
          cache.observations[index].kind != REFPLAYER_RTSP_RANGE_NONE) {
        learned = &cache.observations[index];
        break;
      }
    }
  /* A successful live open establishes source capability for the lifetime of
   * this isolated helper instance. Archive playback replaces that live RTSP
   * connection, so no active-session fact remains, but subsequent seeks must
   * still be able to mint a fresh bounded observation from the learned range. */
  if (!newest && !learned)
    return 0;
  return refplayer_rtsp_timeshift_publish(source_id, newest ? &newest->range : learned,
                                          observation) == 0
             ? 1
             : -1;
}

int refplayer_rtsp_timeshift_latest(const char *source_id, refplayer_rtsp_observation_t *observation) {
  int64_t now = get_time_ms();
  if (!refplayer_rtsp_timeshift_enabled() || refplayer_rtsp_parse_source_id(source_id) != 0 || !observation)
    return -1;
  for (size_t offset = 0; offset < REFPLAYER_RTSP_OBSERVATION_CAPACITY; offset++) {
    size_t index = (cache.next_slot + REFPLAYER_RTSP_OBSERVATION_CAPACITY - 1 - offset) %
                   REFPLAYER_RTSP_OBSERVATION_CAPACITY;
    if (strcmp(cache.observations[index].source_id, source_id) == 0 &&
        observation_is_live(&cache.observations[index], now)) {
      *observation = cache.observations[index];
      return 1;
    }
  }
  return 0;
}

int refplayer_rtsp_timeshift_find(const char *source_id, const char *observation_id,
                                  refplayer_rtsp_observation_t *observation) {
  int64_t now = get_time_ms();
  if (!refplayer_rtsp_timeshift_enabled() || refplayer_rtsp_parse_source_id(source_id) != 0 ||
      refplayer_rtsp_parse_observation_id(observation_id) != 0 || !observation)
    return -1;
  for (size_t index = 0; index < REFPLAYER_RTSP_OBSERVATION_CAPACITY; index++) {
    if (strcmp(cache.observations[index].source_id, source_id) == 0 &&
        strcmp(cache.observations[index].observation_id, observation_id) == 0 &&
        observation_is_live(&cache.observations[index], now)) {
      *observation = cache.observations[index];
      return 1;
    }
  }
  return 0;
}

int refplayer_rtsp_npt_target_is_valid(const refplayer_rtsp_observation_t *observation, double target) {
  return observation && observation->kind == REFPLAYER_RTSP_RANGE_NPT && isfinite(target) &&
         target >= observation->npt_start && target <= observation->npt_end;
}

int refplayer_rtsp_clock_target_is_valid(const refplayer_rtsp_observation_t *observation, int64_t target) {
  if (!observation || observation->kind != REFPLAYER_RTSP_RANGE_CLOCK)
    return 0;
  if (observation->clock_open_ended)
    return target >= observation->observed_at_epoch - REFPLAYER_RTSP_OPEN_CLOCK_LOOKBACK_SECONDS &&
           target <= observation->observed_at_epoch;
  return target >= observation->clock_start_epoch && target <= observation->clock_end_epoch;
}
