#include "refplayer_direct.h"
#include "configuration.h"
#include "http.h"
#include "m3u.h"
#include "md5.h"
#include "service.h"
#include "url_template.h"
#include "utils.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DIRECT_CATALOG_COMMAND "--refplayer-direct-catalog"
#define DIRECT_RESOLVE_COMMAND "--refplayer-direct-resolve"
#define DIRECT_CATALOG_MAGIC "RTP2HTTPD-REFPLAYER-CATALOG/1"
#define DIRECT_RESOLVE_MAGIC "RTP2HTTPD-REFPLAYER-RESOLVE/1"
#define DIRECT_MAX_INPUT (10 * 1024 * 1024)
#define DIRECT_MAX_FRAME_OVERHEAD 16384
#define DIRECT_MAX_OUTPUT (16 * 1024 * 1024)
#define DIRECT_MAX_LINE 4096
#define DIRECT_MAX_URL 4096
#define DIRECT_MAX_FIELD 1024
#define DIRECT_MAX_SOURCES 20000
#define DIRECT_ID_SIZE 33

enum direct_exit_code_e {
  DIRECT_EXIT_OK = 0,
  DIRECT_EXIT_USAGE = 64,
  DIRECT_EXIT_DATA = 65,
  DIRECT_EXIT_INTERNAL = 70,
};

typedef enum {
  DIRECT_URL_HTTP = 0,
  DIRECT_URL_NATIVE,
  DIRECT_URL_RTSP_CANDIDATE,
  DIRECT_URL_INVALID,
} direct_url_kind_t;

typedef struct direct_group_s {
  char *value;
  struct direct_group_s *next;
} direct_group_t;

typedef struct direct_source_s {
  char id[DIRECT_ID_SIZE];
  char *label;
  char *live_url;
  direct_url_kind_t live_kind;
  char *catchup_template;
  char *catchup_mode;
  direct_url_kind_t catchup_kind;
  double catchup_retention_seconds;
  struct direct_source_s *next;
} direct_source_t;

typedef struct direct_channel_s {
  char id[DIRECT_ID_SIZE];
  char *title;
  char *logo_url;
  char *tvg_id;
  char *tvg_name;
  direct_group_t *groups;
  direct_source_t *sources;
  direct_source_t *sources_tail;
  struct direct_channel_s *next;
} direct_channel_t;

typedef struct {
  char *epg_url;
  direct_channel_t *channels;
  direct_channel_t *channels_tail;
  size_t channel_count;
  size_t source_count;
  int has_rtsp_candidates;
  const char *rtsp_candidate_reason;
} direct_catalog_t;

typedef struct {
  char *data;
  size_t used;
  size_t size;
} direct_json_t;

typedef struct {
  char *storage;
  char *base_url;
  char *source_id;
  long long start_epoch;
  long long end_epoch;
  int timezone_offset_seconds;
  char *m3u;
} direct_frame_t;

typedef struct {
  char title[DIRECT_MAX_FIELD];
  char logo[DIRECT_MAX_URL];
  char tvg_id[DIRECT_MAX_FIELD];
  char tvg_name[DIRECT_MAX_FIELD];
  char catchup_mode[128];
  char catchup_source[DIRECT_MAX_URL];
  double catchup_retention_seconds;
  direct_group_t *groups;
  int valid;
} direct_extinf_t;

typedef struct {
  char catchup_mode[128];
  char catchup_source[DIRECT_MAX_URL];
  double catchup_retention_seconds;
} direct_defaults_t;

static int direct_json_append_proxy_m3u(direct_json_t *json, const direct_catalog_t *catalog);

static int direct_url_has_unsafe_byte(const char *url);

static void direct_group_free(direct_group_t *group) {
  while (group) {
    direct_group_t *next = group->next;
    free(group->value);
    free(group);
    group = next;
  }
}

static void direct_catalog_free(direct_catalog_t *catalog) {
  direct_channel_t *channel;

  if (!catalog)
    return;
  free(catalog->epg_url);
  channel = catalog->channels;
  while (channel) {
    direct_channel_t *next_channel = channel->next;
    direct_source_t *source = channel->sources;
    while (source) {
      direct_source_t *next_source = source->next;
      free(source->label);
      free(source->live_url);
      free(source->catchup_template);
      free(source->catchup_mode);
      free(source);
      source = next_source;
    }
    free(channel->title);
    free(channel->logo_url);
    free(channel->tvg_id);
    free(channel->tvg_name);
    direct_group_free(channel->groups);
    free(channel);
    channel = next_channel;
  }
  memset(catalog, 0, sizeof(*catalog));
}

static void direct_frame_free(direct_frame_t *frame) {
  if (!frame)
    return;
  free(frame->storage);
  memset(frame, 0, sizeof(*frame));
}

static int direct_write_json(const char *json) {
  size_t length;

  if (!json)
    return -1;
  length = strlen(json);
  if (fwrite(json, 1, length, stdout) != length || fputc('\n', stdout) == EOF || fflush(stdout) != 0)
    return -1;
  return 0;
}

static int direct_error(int exit_code, const char *code, const char *message) {
  direct_json_t json = {0};
  char *escaped_code = json_escape_string(code ? code : "internal_error");
  char *escaped_message = json_escape_string(message ? message : "The one-shot operation failed.");

  if (escaped_code && escaped_message) {
    size_t needed = strlen(escaped_code) + strlen(escaped_message) + 80;
    json.data = malloc(needed);
    if (json.data) {
      snprintf(json.data, needed, "{\"schema_version\":2,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", escaped_code,
               escaped_message);
      (void)direct_write_json(json.data);
    }
  }
  free(escaped_code);
  free(escaped_message);
  free(json.data);
  return exit_code;
}

static int direct_json_reserve(direct_json_t *json, size_t additional) {
  size_t required;
  size_t new_size;
  char *new_data;

  if (!json || json->used >= DIRECT_MAX_OUTPUT || additional >= DIRECT_MAX_OUTPUT ||
      additional > DIRECT_MAX_OUTPUT - json->used - 1)
    return -1;
  required = json->used + additional + 1;
  if (required <= json->size)
    return 0;
  new_size = json->size ? json->size : 4096;
  while (new_size < required) {
    if (new_size >= DIRECT_MAX_OUTPUT / 2) {
      new_size = DIRECT_MAX_OUTPUT;
      break;
    }
    new_size *= 2;
  }
  if (new_size < required || new_size > DIRECT_MAX_OUTPUT)
    return -1;
  new_data = realloc(json->data, new_size);
  if (!new_data)
    return -1;
  json->data = new_data;
  json->size = new_size;
  return 0;
}

static int direct_json_append(direct_json_t *json, const char *value) {
  size_t length;

  if (!json || !value)
    return -1;
  length = strlen(value);
  if (direct_json_reserve(json, length) != 0)
    return -1;
  memcpy(json->data + json->used, value, length);
  json->used += length;
  json->data[json->used] = '\0';
  return 0;
}

static int direct_json_escape(direct_json_t *json, const char *value) {
  char *escaped = json_escape_string(value ? value : "");
  int result;

  if (!escaped)
    return -1;
  result = direct_json_append(json, escaped);
  free(escaped);
  return result;
}

static int direct_read_stdin(char **output, size_t *output_size) {
  size_t capacity = 8192;
  size_t used = 0;
  char *buffer;

  if (!output || !output_size)
    return -1;
  buffer = malloc(capacity + 1);
  if (!buffer)
    return -2;

  for (;;) {
    size_t available;
    size_t count;

    if (used == capacity) {
      size_t new_capacity;
      char *new_buffer;
      if (capacity >= DIRECT_MAX_INPUT + DIRECT_MAX_FRAME_OVERHEAD) {
        free(buffer);
        return -1;
      }
      new_capacity = capacity * 2;
      if (new_capacity > DIRECT_MAX_INPUT + DIRECT_MAX_FRAME_OVERHEAD)
        new_capacity = DIRECT_MAX_INPUT + DIRECT_MAX_FRAME_OVERHEAD;
      new_buffer = realloc(buffer, new_capacity + 1);
      if (!new_buffer) {
        free(buffer);
        return -2;
      }
      buffer = new_buffer;
      capacity = new_capacity;
    }

    available = capacity - used;
    count = fread(buffer + used, 1, available, stdin);
    used += count;
    if (count < available) {
      if (ferror(stdin)) {
        free(buffer);
        return -1;
      }
      break;
    }
  }

  if (memchr(buffer, '\0', used)) {
    free(buffer);
    return -1;
  }
  buffer[used] = '\0';
  *output = buffer;
  *output_size = used;
  return 0;
}

static char *direct_next_frame_line(char **cursor) {
  char *line;
  char *newline;
  size_t length;

  if (!cursor || !*cursor)
    return NULL;
  line = *cursor;
  newline = strchr(line, '\n');
  if (!newline)
    return NULL;
  *newline = '\0';
  *cursor = newline + 1;
  length = strlen(line);
  if (length > 0 && line[length - 1] == '\r')
    line[length - 1] = '\0';
  return line;
}

static int direct_parse_integer(const char *value, long long minimum, long long maximum, long long *output) {
  char *end = NULL;
  long long parsed;

  if (!value || !value[0] || !output)
    return -1;
  errno = 0;
  parsed = strtoll(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || parsed < minimum || parsed > maximum)
    return -1;
  *output = parsed;
  return 0;
}

static int direct_is_source_id(const char *value) {
  if (!value || strlen(value) != DIRECT_ID_SIZE - 1)
    return 0;
  for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++)
    if (!isxdigit(*cursor))
      return 0;
  return 1;
}

static int direct_parse_frame(int resolve, direct_frame_t *frame) {
  size_t input_size = 0;
  char *cursor;
  char *line;
  long long timezone_offset;
  int read_result;

  if (!frame)
    return -1;
  memset(frame, 0, sizeof(*frame));
  read_result = direct_read_stdin(&frame->storage, &input_size);
  if (read_result != 0)
    return read_result;
  cursor = frame->storage;

  line = direct_next_frame_line(&cursor);
  if (!line || strcmp(line, resolve ? DIRECT_RESOLVE_MAGIC : DIRECT_CATALOG_MAGIC) != 0)
    return -1;
  frame->base_url = direct_next_frame_line(&cursor);
  if (!frame->base_url || !frame->base_url[0] || strlen(frame->base_url) >= DIRECT_MAX_URL)
    return -1;

  if (resolve) {
    frame->source_id = direct_next_frame_line(&cursor);
    line = direct_next_frame_line(&cursor);
    if (!direct_is_source_id(frame->source_id) || !line ||
        direct_parse_integer(line, 0, INT64_MAX, &frame->start_epoch) != 0)
      return -1;
    line = direct_next_frame_line(&cursor);
    if (!line || direct_parse_integer(line, 0, INT64_MAX, &frame->end_epoch) != 0 ||
        frame->end_epoch <= frame->start_epoch)
      return -1;
    line = direct_next_frame_line(&cursor);
    if (!line || direct_parse_integer(line, -43200, 50400, &timezone_offset) != 0)
      return -1;
    frame->timezone_offset_seconds = (int)timezone_offset;
  }

  frame->m3u = cursor;
  if (!frame->m3u || !frame->m3u[0] || strlen(frame->m3u) > DIRECT_MAX_INPUT)
    return -1;
  return 0;
}

static int direct_url_has_unsafe_byte(const char *url) {
  const unsigned char *p = (const unsigned char *)url;
  if (!url || !url[0] || strlen(url) >= DIRECT_MAX_URL)
    return 1;
  while (*p) {
    if (*p <= 0x20 || *p == 0x7f)
      return 1;
    p++;
  }
  return 0;
}

static int direct_is_http_url(const char *url) {
  return url && (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0);
}

static int direct_is_native_url(const char *url) {
  return url && (strncasecmp(url, "rtp://", 6) == 0 || strncasecmp(url, "udp://", 6) == 0);
}

static int direct_is_rtsp_url(const char *url) { return url && strncasecmp(url, "rtsp://", 7) == 0; }

static int direct_validate_absolute_rtsp_url(const char *url) {
  const char *authority;
  const char *authority_end;
  const char *port = NULL;

  /* The URL is later embedded in a private synthetic M3U/config snapshot.
   * Reject delimiter bytes instead of trying to apply JSON escaping to an M3U
   * grammar; this keeps the snapshot a single opaque service entry. */
  if (!direct_is_rtsp_url(url) || direct_url_has_unsafe_byte(url) || strpbrk(url, "\"\\") != NULL)
    return -1;
  authority = url + 7;
  authority_end = strpbrk(authority, "/?#");
  if (!authority_end)
    authority_end = authority + strlen(authority);
  if (authority_end == authority || memchr(authority, '@', (size_t)(authority_end - authority)))
    return -1;
  if (*authority == '[') {
    const char *bracket = memchr(authority, ']', (size_t)(authority_end - authority));
    if (!bracket || bracket == authority + 1)
      return -1;
    if (bracket + 1 < authority_end) {
      if (bracket[1] != ':')
        return -1;
      port = bracket + 2;
    }
  } else {
    const char *colon = memchr(authority, ':', (size_t)(authority_end - authority));
    if (colon) {
      if (colon == authority || memchr(colon + 1, ':', (size_t)(authority_end - colon - 1)))
        return -1;
      port = colon + 1;
    }
  }
  if (port) {
    if (port == authority_end)
      return -1;
    for (const char *digit = port; digit < authority_end; digit++)
      if (!isdigit((unsigned char)*digit))
        return -1;
  }
  return 0;
}

static int direct_validate_absolute_http_url(const char *url) {
  const char *scheme_end;
  const char *authority;
  const char *authority_end;
  const char *at_sign;
  const char *port = NULL;

  if (!direct_is_http_url(url) || direct_url_has_unsafe_byte(url))
    return -1;
  scheme_end = strstr(url, "://");
  if (!scheme_end)
    return -1;
  authority = scheme_end + 3;
  authority_end = strpbrk(authority, "/?#");
  if (!authority_end)
    authority_end = authority + strlen(authority);
  if (authority_end == authority)
    return -1;

  /* The direct protocol deliberately does not transport authority userinfo.
   * Apart from avoiding ambiguous parsing, this keeps credentials out of the
   * catalog JSON that crosses the helper boundary. */
  at_sign = memchr(authority, '@', (size_t)(authority_end - authority));
  if (at_sign)
    return -1;

  if (*authority == '[') {
    const char *bracket = memchr(authority, ']', (size_t)(authority_end - authority));
    if (!bracket || bracket == authority + 1)
      return -1;
    if (bracket + 1 < authority_end) {
      if (bracket[1] != ':')
        return -1;
      port = bracket + 2;
    }
  } else {
    const char *colon = memchr(authority, ':', (size_t)(authority_end - authority));
    if (colon) {
      if (memchr(colon + 1, ':', (size_t)(authority_end - colon - 1)))
        return -1;
      if (colon == authority)
        return -1;
      port = colon + 1;
    }
  }
  if (port) {
    if (port == authority_end)
      return -1;
    for (const char *digit = port; digit < authority_end; digit++)
      if (!isdigit((unsigned char)*digit))
        return -1;
  }
  return 0;
}

static int direct_normalize_absolute_http_url(const char *url, char *output, size_t output_size) {
  const char *scheme_end;
  const char *authority;
  const char *path;
  const char *path_end;
  const char *suffix;
  size_t origin_length;
  size_t used;
  size_t rewind_positions[DIRECT_MAX_URL / 2];
  size_t component_count = 0;
  int trailing_directory = 0;

  if (!output || output_size == 0 || direct_validate_absolute_http_url(url) != 0)
    return -1;
  scheme_end = strstr(url, "://");
  authority = scheme_end + 3;
  path = strpbrk(authority, "/?#");
  if (!path)
    path = url + strlen(url);
  origin_length = (size_t)(path - url);
  if (origin_length + 2 > output_size)
    return -1;
  memcpy(output, url, origin_length);
  used = origin_length;

  if (*path != '/') {
    output[used++] = '/';
    suffix = path;
  } else {
    const char *cursor;
    path_end = strpbrk(path, "?#");
    if (!path_end)
      path_end = url + strlen(url);
    suffix = path_end;
    cursor = path + 1;
    output[used++] = '/';

    while (cursor <= path_end) {
      const char *separator = memchr(cursor, '/', (size_t)(path_end - cursor));
      const char *component_end = separator ? separator : path_end;
      size_t component_length = (size_t)(component_end - cursor);

      if (component_length == 1 && cursor[0] == '.') {
        trailing_directory = 1;
      } else if (component_length == 2 && cursor[0] == '.' && cursor[1] == '.') {
        if (component_count > 0)
          used = rewind_positions[--component_count];
        trailing_directory = 1;
      } else if (component_length > 0) {
        if (component_count >= sizeof(rewind_positions) / sizeof(rewind_positions[0]))
          return -1;
        rewind_positions[component_count++] = used;
        if (used > origin_length + 1)
          output[used++] = '/';
        if (used + component_length + 1 > output_size)
          return -1;
        memcpy(output + used, cursor, component_length);
        used += component_length;
        trailing_directory = separator && component_end + 1 == path_end;
      } else if (separator && component_end + 1 == path_end) {
        trailing_directory = 1;
      }

      if (!separator)
        break;
      cursor = separator + 1;
    }
    if (trailing_directory && used > origin_length + 1) {
      if (used + 2 > output_size)
        return -1;
      output[used++] = '/';
    }
  }

  if (used + strlen(suffix) + 1 > output_size)
    return -1;
  memcpy(output + used, suffix, strlen(suffix) + 1);
  return direct_validate_absolute_http_url(output);
}

static int direct_base_parts(const char *base_url, char *scheme, size_t scheme_size, char *origin, size_t origin_size,
                             char *base_without_query, size_t base_without_query_size) {
  const char *scheme_end;
  const char *authority;
  const char *authority_end;
  const char *suffix_end;
  size_t scheme_len;
  size_t origin_len;
  size_t base_len;

  if (direct_validate_absolute_http_url(base_url) != 0)
    return -1;
  scheme_end = strstr(base_url, "://");
  scheme_len = (size_t)(scheme_end - base_url);
  if (scheme_len + 1 > scheme_size)
    return -1;
  memcpy(scheme, base_url, scheme_len);
  scheme[scheme_len] = '\0';

  authority = scheme_end + 3;
  authority_end = strpbrk(authority, "/?#");
  if (!authority_end)
    authority_end = authority + strlen(authority);
  origin_len = (size_t)(authority_end - base_url);
  if (origin_len + 1 > origin_size)
    return -1;
  memcpy(origin, base_url, origin_len);
  origin[origin_len] = '\0';

  suffix_end = strpbrk(authority_end, "?#");
  if (!suffix_end)
    suffix_end = base_url + strlen(base_url);
  base_len = (size_t)(suffix_end - base_url);
  if (base_len + 1 > base_without_query_size)
    return -1;
  memcpy(base_without_query, base_url, base_len);
  base_without_query[base_len] = '\0';
  if (authority_end == suffix_end) {
    if (base_len + 2 > base_without_query_size)
      return -1;
    base_without_query[base_len++] = '/';
    base_without_query[base_len] = '\0';
  }
  return 0;
}

static direct_url_kind_t direct_resolve_url(const char *reference, const char *base_url, char **output) {
  char scheme[16];
  char origin[DIRECT_MAX_URL];
  char base_path[DIRECT_MAX_URL];
  char resolved[DIRECT_MAX_URL];
  char normalized[DIRECT_MAX_URL];
  const char *last_slash;
  size_t directory_len;
  int written;

  if (output)
    *output = NULL;
  if (!reference || !reference[0] || direct_url_has_unsafe_byte(reference))
    return DIRECT_URL_INVALID;

  if (direct_is_native_url(reference) || direct_is_rtsp_url(reference)) {
    if (direct_is_rtsp_url(reference) && direct_validate_absolute_rtsp_url(reference) != 0)
      return DIRECT_URL_INVALID;
    if (output)
      *output = strdup(reference);
    if (output && !*output)
      return DIRECT_URL_INVALID;
    return direct_is_rtsp_url(reference) ? DIRECT_URL_RTSP_CANDIDATE : DIRECT_URL_NATIVE;
  }
  if (direct_is_http_url(reference)) {
    if (direct_normalize_absolute_http_url(reference, normalized, sizeof(normalized)) != 0)
      return DIRECT_URL_INVALID;
    if (output)
      *output = strdup(normalized);
    return !output || *output ? DIRECT_URL_HTTP : DIRECT_URL_INVALID;
  }
  if (strstr(reference, "://") ||
      (strchr(reference, ':') && (!strchr(reference, '/') || strchr(reference, ':') < strchr(reference, '/'))))
    return DIRECT_URL_INVALID;
  if (direct_base_parts(base_url, scheme, sizeof(scheme), origin, sizeof(origin), base_path, sizeof(base_path)) != 0)
    return DIRECT_URL_INVALID;

  if (reference[0] == '/' && reference[1] == '/') {
    written = snprintf(resolved, sizeof(resolved), "%s:%s", scheme, reference);
  } else if (reference[0] == '/') {
    written = snprintf(resolved, sizeof(resolved), "%s%s", origin, reference);
  } else if (reference[0] == '?') {
    written = snprintf(resolved, sizeof(resolved), "%s%s", base_path, reference);
  } else if (reference[0] == '#') {
    size_t base_length = strcspn(base_url, "#");
    written = snprintf(resolved, sizeof(resolved), "%.*s%s", (int)base_length, base_url, reference);
  } else {
    last_slash = strrchr(base_path, '/');
    if (!last_slash || last_slash < strstr(base_path, "://") + 3)
      return DIRECT_URL_INVALID;
    directory_len = (size_t)(last_slash - base_path + 1);
    written = snprintf(resolved, sizeof(resolved), "%.*s%s", (int)directory_len, base_path, reference);
  }
  if (written < 0 || (size_t)written >= sizeof(resolved) ||
      direct_normalize_absolute_http_url(resolved, normalized, sizeof(normalized)) != 0)
    return DIRECT_URL_INVALID;
  if (output)
    *output = strdup(normalized);
  return !output || *output ? DIRECT_URL_HTTP : DIRECT_URL_INVALID;
}

static void direct_hash_part(MD5Context *context, const char *value) {
  uint8_t separator = 0;
  if (value)
    md5Update(context, (uint8_t *)(uintptr_t)value, strlen(value));
  md5Update(context, &separator, 1);
}

static void direct_hash_groups(MD5Context *context, const direct_group_t *groups) {
  for (const direct_group_t *group = groups; group; group = group->next)
    direct_hash_part(context, group->value);
}

static void direct_channel_id(const char *title, const direct_group_t *groups, char output[DIRECT_ID_SIZE]) {
  MD5Context context;
  md5Init(&context);
  direct_hash_part(&context, "refplayer-direct-channel-v1");
  direct_hash_groups(&context, groups);
  direct_hash_part(&context, title);
  md5Finalize(&context);
  md5_to_hex(context.digest, output);
}

static void direct_source_base_id(const direct_channel_t *channel, const char *live_url, const char *label,
                                  const char *catchup_template, const char *catchup_mode, char output[DIRECT_ID_SIZE]) {
  MD5Context context;
  md5Init(&context);
  direct_hash_part(&context, "refplayer-direct-source-v1");
  direct_hash_part(&context, channel->id);
  direct_hash_part(&context, live_url);
  direct_hash_part(&context, label);
  direct_hash_part(&context, catchup_template);
  direct_hash_part(&context, catchup_mode);
  md5Finalize(&context);
  md5_to_hex(context.digest, output);
}

static int direct_source_id_exists(const direct_catalog_t *catalog, const char *source_id) {
  for (const direct_channel_t *channel = catalog->channels; channel; channel = channel->next)
    for (const direct_source_t *source = channel->sources; source; source = source->next)
      if (strcmp(source->id, source_id) == 0)
        return 1;
  return 0;
}

static void direct_unique_source_id(const direct_catalog_t *catalog, const char *base_id, char output[DIRECT_ID_SIZE]) {
  unsigned int occurrence = 1;
  strncpy(output, base_id, DIRECT_ID_SIZE);
  while (direct_source_id_exists(catalog, output)) {
    MD5Context context;
    char occurrence_text[32];
    occurrence++;
    snprintf(occurrence_text, sizeof(occurrence_text), "%u", occurrence);
    md5Init(&context);
    direct_hash_part(&context, "refplayer-direct-source-duplicate-v1");
    direct_hash_part(&context, base_id);
    direct_hash_part(&context, occurrence_text);
    md5Finalize(&context);
    md5_to_hex(context.digest, output);
  }
}

static int direct_group_contains(const direct_group_t *groups, const char *value) {
  for (const direct_group_t *group = groups; group; group = group->next)
    if (strcmp(group->value, value) == 0)
      return 1;
  return 0;
}

static int direct_group_append(direct_group_t **groups, const char *value) {
  direct_group_t **tail;
  direct_group_t *group;

  if (!groups || !value || !value[0] || direct_group_contains(*groups, value))
    return 0;
  group = calloc(1, sizeof(*group));
  if (!group)
    return -1;
  group->value = strdup(value);
  if (!group->value) {
    free(group);
    return -1;
  }
  tail = groups;
  while (*tail)
    tail = &(*tail)->next;
  *tail = group;
  return 0;
}

static direct_group_t *direct_group_clone(const direct_group_t *source) {
  direct_group_t *result = NULL;
  for (const direct_group_t *group = source; group; group = group->next) {
    if (direct_group_append(&result, group->value) != 0) {
      direct_group_free(result);
      return NULL;
    }
  }
  return result;
}

static int direct_groups_equal(const direct_group_t *left, const direct_group_t *right) {
  while (left && right) {
    if (strcmp(left->value, right->value) != 0)
      return 0;
    left = left->next;
    right = right->next;
  }
  return !left && !right;
}

static char *direct_trim(char *value) {
  char *end;
  while (*value && isspace((unsigned char)*value))
    value++;
  end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1]))
    *--end = '\0';
  return value;
}

static int direct_parse_groups(const char *line, direct_group_t **groups) {
  char value[DIRECT_MAX_FIELD];

  for (size_t occurrence = 0; m3u_extract_attribute_at(line, "group-title", occurrence, value, sizeof(value)) == 0;
       occurrence++) {
    char *cursor = value;
    for (;;) {
      char *separator = strchr(cursor, ';');
      char *trimmed;
      if (separator)
        *separator = '\0';
      trimmed = direct_trim(cursor);
      if (trimmed[0] && direct_group_append(groups, trimmed) != 0)
        return -1;
      if (!separator)
        break;
      cursor = separator + 1;
    }
  }
  return 0;
}

static double direct_parse_retention(const char *value) {
  char *end = NULL;
  double days;

  if (!value || !value[0])
    return 0;
  errno = 0;
  days = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || !isfinite(days) || days <= 0 || days > 3650)
    return 0;
  return days * 86400;
}

static int direct_groups_and_title_match(const direct_channel_t *channel, const direct_extinf_t *extinf) {
  return strcmp(channel->title, extinf->title) == 0 && direct_groups_equal(channel->groups, extinf->groups);
}

static direct_channel_t *direct_find_channel(direct_catalog_t *catalog, const direct_extinf_t *extinf) {
  for (direct_channel_t *channel = catalog->channels; channel; channel = channel->next)
    if (direct_groups_and_title_match(channel, extinf))
      return channel;
  return NULL;
}

static char *direct_optional_duplicate(const char *value) { return value && value[0] ? strdup(value) : NULL; }

static direct_channel_t *direct_add_channel(direct_catalog_t *catalog, const direct_extinf_t *extinf,
                                            const char *base_url) {
  direct_channel_t *channel = calloc(1, sizeof(*channel));
  char *resolved_logo = NULL;

  if (!channel)
    return NULL;
  channel->title = strdup(extinf->title);
  channel->tvg_id = direct_optional_duplicate(extinf->tvg_id);
  channel->tvg_name = direct_optional_duplicate(extinf->tvg_name);
  if (extinf->groups) {
    channel->groups = direct_group_clone(extinf->groups);
    if (!channel->groups)
      goto fail;
  }
  if (extinf->logo[0] && direct_resolve_url(extinf->logo, base_url, &resolved_logo) == DIRECT_URL_HTTP)
    channel->logo_url = resolved_logo;
  if (!channel->title || (extinf->tvg_id[0] && !channel->tvg_id) || (extinf->tvg_name[0] && !channel->tvg_name))
    goto fail;
  direct_channel_id(channel->title, channel->groups, channel->id);

  if (catalog->channels_tail)
    catalog->channels_tail->next = channel;
  else
    catalog->channels = channel;
  catalog->channels_tail = channel;
  catalog->channel_count++;
  return channel;

fail:
  free(channel->title);
  free(channel->logo_url);
  free(channel->tvg_id);
  free(channel->tvg_name);
  direct_group_free(channel->groups);
  free(channel);
  return NULL;
}

static void direct_fill_missing_metadata(direct_channel_t *channel, const direct_extinf_t *extinf,
                                         const char *base_url) {
  char *resolved_logo = NULL;
  if (!channel->logo_url && extinf->logo[0] &&
      direct_resolve_url(extinf->logo, base_url, &resolved_logo) == DIRECT_URL_HTTP)
    channel->logo_url = resolved_logo;
  if (!channel->tvg_id && extinf->tvg_id[0])
    channel->tvg_id = strdup(extinf->tvg_id);
  if (!channel->tvg_name && extinf->tvg_name[0])
    channel->tvg_name = strdup(extinf->tvg_name);
}

static char *direct_append_catchup(const char *live_url, const char *catchup_source) {
  size_t live_length;
  size_t source_length;
  char *result;

  if (!live_url || !catchup_source)
    return NULL;
  live_length = strlen(live_url);
  source_length = strlen(catchup_source);
  if (live_length + source_length >= DIRECT_MAX_URL)
    return NULL;
  result = malloc(live_length + source_length + 1);
  if (!result)
    return NULL;
  memcpy(result, live_url, live_length);
  memcpy(result + live_length, catchup_source, source_length + 1);
  return result;
}

static int direct_add_source(direct_catalog_t *catalog, direct_channel_t *channel, const direct_extinf_t *extinf,
                             const char *raw_live_url, const char *label, const char *base_url) {
  direct_source_t *source;
  direct_url_kind_t live_kind;
  char *live_url = NULL;
  char *catchup_template = NULL;
  direct_url_kind_t catchup_kind = DIRECT_URL_INVALID;
  const char *catchup_mode = extinf->catchup_mode[0] ? extinf->catchup_mode : "default";
  int has_declared_catchup = extinf->catchup_source[0] != '\0';
  char base_source_id[DIRECT_ID_SIZE];

  live_kind = direct_resolve_url(raw_live_url, base_url, &live_url);
  if (live_kind == DIRECT_URL_INVALID)
    return 0;
  if (has_declared_catchup) {
    if (direct_is_rtsp_url(extinf->catchup_source)) {
      if (direct_validate_absolute_rtsp_url(extinf->catchup_source) == 0) {
        catchup_template = strdup(extinf->catchup_source);
        catchup_kind = catchup_template ? DIRECT_URL_RTSP_CANDIDATE : DIRECT_URL_INVALID;
      }
    } else if (direct_is_native_url(extinf->catchup_source)) {
      catchup_template = strdup(extinf->catchup_source);
      catchup_kind = catchup_template ? DIRECT_URL_NATIVE : DIRECT_URL_INVALID;
    } else if (strcmp(catchup_mode, "append") == 0) {
      catchup_template = direct_append_catchup(live_url, extinf->catchup_source);
      if (catchup_template && direct_validate_absolute_http_url(catchup_template) == 0) {
        catchup_kind = DIRECT_URL_HTTP;
      } else if (catchup_template && direct_validate_absolute_rtsp_url(catchup_template) == 0) {
        catchup_kind = DIRECT_URL_RTSP_CANDIDATE;
      } else if (catchup_template && direct_is_native_url(catchup_template) &&
                 !direct_url_has_unsafe_byte(catchup_template)) {
        catchup_kind = DIRECT_URL_NATIVE;
      }
    } else {
      catchup_kind = direct_resolve_url(extinf->catchup_source, base_url, &catchup_template);
    }

    if (catchup_kind == DIRECT_URL_RTSP_CANDIDATE) {
      catalog->has_rtsp_candidates = 1;
      if (!catalog->rtsp_candidate_reason)
        catalog->rtsp_candidate_reason = "catchup_source_rtsp_candidate";
    } else if (catchup_kind == DIRECT_URL_INVALID) {
      free(catchup_template);
      catchup_template = NULL;
    }
  }

  source = calloc(1, sizeof(*source));
  if (!source) {
    free(live_url);
    free(catchup_template);
    return -1;
  }
  source->label = direct_optional_duplicate(label);
  source->live_url = live_url;
  source->live_kind = live_kind;
  source->catchup_template = catchup_template;
  source->catchup_mode = catchup_template ? strdup(catchup_mode) : NULL;
  source->catchup_kind = catchup_kind;
  source->catchup_retention_seconds = extinf->catchup_retention_seconds;
  if ((label && label[0] && !source->label) || (catchup_template && !source->catchup_mode)) {
    free(source->label);
    free(source->live_url);
    free(source->catchup_template);
    free(source);
    return -1;
  }
  direct_source_base_id(channel, source->live_url, source->label, source->catchup_template, source->catchup_mode,
                        base_source_id);
  direct_unique_source_id(catalog, base_source_id, source->id);

  if (channel->sources_tail)
    channel->sources_tail->next = source;
  else
    channel->sources = source;
  channel->sources_tail = source;
  catalog->source_count++;
  return 1;
}

static void direct_extinf_reset(direct_extinf_t *extinf) {
  if (!extinf)
    return;
  direct_group_free(extinf->groups);
  memset(extinf, 0, sizeof(*extinf));
}

static int direct_parse_extinf(const char *line, const direct_defaults_t *defaults, direct_extinf_t *extinf) {
  char retention[64];

  direct_extinf_reset(extinf);
  if (m3u_extract_service_name(line, extinf->title, sizeof(extinf->title)) != 0)
    return 0;
  if (direct_parse_groups(line, &extinf->groups) != 0)
    return -1;
  (void)m3u_extract_attribute(line, "tvg-logo", extinf->logo, sizeof(extinf->logo));
  (void)m3u_extract_attribute(line, "tvg-id", extinf->tvg_id, sizeof(extinf->tvg_id));
  (void)m3u_extract_attribute(line, "tvg-name", extinf->tvg_name, sizeof(extinf->tvg_name));
  if (m3u_extract_attribute(line, "catchup", extinf->catchup_mode, sizeof(extinf->catchup_mode)) != 0)
    strncpy(extinf->catchup_mode, defaults->catchup_mode, sizeof(extinf->catchup_mode) - 1);
  if (m3u_extract_attribute(line, "catchup-source", extinf->catchup_source, sizeof(extinf->catchup_source)) != 0)
    strncpy(extinf->catchup_source, defaults->catchup_source, sizeof(extinf->catchup_source) - 1);
  if (m3u_extract_attribute(line, "catchup-days", retention, sizeof(retention)) == 0)
    extinf->catchup_retention_seconds = direct_parse_retention(retention);
  else
    extinf->catchup_retention_seconds = defaults->catchup_retention_seconds;
  extinf->valid = 1;
  return 1;
}

static void direct_parse_header_defaults(const char *line, direct_defaults_t *defaults) {
  char retention[64];
  (void)m3u_extract_attribute(line, "catchup", defaults->catchup_mode, sizeof(defaults->catchup_mode));
  (void)m3u_extract_attribute(line, "catchup-source", defaults->catchup_source, sizeof(defaults->catchup_source));
  if (m3u_extract_attribute(line, "catchup-days", retention, sizeof(retention)) == 0)
    defaults->catchup_retention_seconds = direct_parse_retention(retention);
}

static int direct_catalog_parse(char *content, const char *base_url, direct_catalog_t *catalog) {
  char *cursor = content;
  direct_extinf_t extinf = {0};
  direct_defaults_t defaults = {0};

  if (!content || !base_url || !catalog || direct_validate_absolute_http_url(base_url) != 0)
    return -1;
  if (strncmp(cursor, "\xEF\xBB\xBF", 3) == 0)
    cursor += 3;

  while (*cursor) {
    char *line = cursor;
    char *newline = strchr(cursor, '\n');
    size_t raw_length;
    char *trimmed;

    if (newline) {
      raw_length = (size_t)(newline - line);
      *newline = '\0';
      cursor = newline + 1;
    } else {
      raw_length = strlen(line);
      cursor += raw_length;
    }
    if (raw_length >= DIRECT_MAX_LINE) {
      direct_extinf_reset(&extinf);
      return -1;
    }
    trimmed = direct_trim(line);
    if (!trimmed[0])
      continue;

    if (m3u_is_header(trimmed)) {
      char *tvg_url = m3u_extract_tvg_url(trimmed);
      direct_parse_header_defaults(trimmed, &defaults);
      if (tvg_url) {
        char *resolved = NULL;
        if (!catalog->epg_url && direct_resolve_url(tvg_url, base_url, &resolved) == DIRECT_URL_HTTP)
          catalog->epg_url = resolved;
        else
          free(resolved);
        free(tvg_url);
      }
      continue;
    }
    if (strncmp(trimmed, "#EXTINF:", 8) == 0) {
      int result = direct_parse_extinf(trimmed, &defaults, &extinf);
      if (result < 0)
        return -2;
      continue;
    }
    if (trimmed[0] == '#')
      continue;
    if (extinf.valid) {
      const char *label_start = http_find_url_label(trimmed);
      char raw_live_url[DIRECT_MAX_URL];
      char label[DIRECT_MAX_FIELD] = {0};
      size_t live_length = label_start ? (size_t)(label_start - trimmed) : strlen(trimmed);
      direct_channel_t *channel;
      int add_result;

      if (live_length >= sizeof(raw_live_url)) {
        direct_extinf_reset(&extinf);
        return -1;
      }
      memcpy(raw_live_url, trimmed, live_length);
      raw_live_url[live_length] = '\0';
      if (label_start) {
        if (strlen(label_start + 1) >= sizeof(label)) {
          direct_extinf_reset(&extinf);
          return -1;
        }
        strncpy(label, label_start + 1, sizeof(label) - 1);
      }

      /* Reject malformed entries before adding their Web Player merge key to
       * the catalog. This keeps a bad EXTINF/URL pair from leaving an empty
       * channel that could corrupt subsequent tail insertion. */
      if (direct_resolve_url(raw_live_url, base_url, NULL) == DIRECT_URL_INVALID) {
        direct_extinf_reset(&extinf);
        continue;
      }

      channel = direct_find_channel(catalog, &extinf);
      if (!channel) {
        channel = direct_add_channel(catalog, &extinf, base_url);
        if (!channel) {
          direct_extinf_reset(&extinf);
          return -2;
        }
      } else {
        direct_fill_missing_metadata(channel, &extinf, base_url);
      }
      add_result = direct_add_source(catalog, channel, &extinf, raw_live_url, label, base_url);
      if (add_result < 0) {
        direct_extinf_reset(&extinf);
        return -2;
      }
      direct_extinf_reset(&extinf);
      if (catalog->source_count > DIRECT_MAX_SOURCES)
        return -1;
    }
  }

  direct_extinf_reset(&extinf);
  return catalog->source_count > 0 ? 0 : -1;
}

static int direct_json_optional_string(direct_json_t *json, const char *value) {
  if (!value)
    return direct_json_append(json, "null");
  return direct_json_append(json, "\"") != 0 || direct_json_escape(json, value) != 0 ||
                 direct_json_append(json, "\"") != 0
             ? -1
             : 0;
}

static int direct_catalog_json(const direct_catalog_t *catalog, direct_json_t *json) {
  int first_channel = 1;

#define DIRECT_APPEND(value)                                                                                           \
  do {                                                                                                                 \
    if (direct_json_append(json, (value)) != 0)                                                                        \
      return -1;                                                                                                       \
  } while (0)
#define DIRECT_ESCAPE(value)                                                                                           \
  do {                                                                                                                 \
    if (direct_json_escape(json, (value)) != 0)                                                                        \
      return -1;                                                                                                       \
  } while (0)

  DIRECT_APPEND("{\"schema_version\":2,\"helper_version\":\"");
  DIRECT_ESCAPE(VERSION);
  DIRECT_APPEND("\",\"mode\":\"");
  DIRECT_APPEND(catalog->has_rtsp_candidates ? "mixed" : "direct");
  DIRECT_APPEND("\",\"reason\":");
  if (direct_json_optional_string(json, catalog->rtsp_candidate_reason) != 0)
    return -1;
  DIRECT_APPEND(",\"proxy_m3u\":");
  if (direct_json_append_proxy_m3u(json, catalog) != 0)
    return -1;
  DIRECT_APPEND(",\"epg_url\":");
  if (direct_json_optional_string(json, catalog->epg_url) != 0)
    return -1;
  DIRECT_APPEND(",\"channels\":[");

  for (const direct_channel_t *channel = catalog->channels; channel; channel = channel->next) {
    int first_group = 1;
    int first_source = 1;
    if (!channel->sources)
      continue;
    if (!first_channel)
      DIRECT_APPEND(",");
    first_channel = 0;
    DIRECT_APPEND("{\"id\":\"");
    DIRECT_ESCAPE(channel->id);
    DIRECT_APPEND("\",\"title\":\"");
    DIRECT_ESCAPE(channel->title);
    DIRECT_APPEND("\",\"group_titles\":[");
    for (const direct_group_t *group = channel->groups; group; group = group->next) {
      if (!first_group)
        DIRECT_APPEND(",");
      first_group = 0;
      DIRECT_APPEND("\"");
      DIRECT_ESCAPE(group->value);
      DIRECT_APPEND("\"");
    }
    DIRECT_APPEND("],\"tvg_id\":");
    if (direct_json_optional_string(json, channel->tvg_id) != 0)
      return -1;
    DIRECT_APPEND(",\"tvg_name\":");
    if (direct_json_optional_string(json, channel->tvg_name) != 0)
      return -1;
    DIRECT_APPEND(",\"logo_url\":");
    if (direct_json_optional_string(json, channel->logo_url) != 0)
      return -1;
    DIRECT_APPEND(",\"sources\":[");
    for (const direct_source_t *source = channel->sources; source; source = source->next) {
      char retention[64];
      if (!first_source)
        DIRECT_APPEND(",");
      first_source = 0;
      DIRECT_APPEND("{\"id\":\"");
      DIRECT_ESCAPE(source->id);
      DIRECT_APPEND("\",\"label\":");
      if (direct_json_optional_string(json, source->label) != 0)
        return -1;
      DIRECT_APPEND(",\"live_url\":\"");
      DIRECT_ESCAPE(source->live_url);
      DIRECT_APPEND("\",\"live_route\":\"");
      DIRECT_APPEND("direct");
      DIRECT_APPEND("\",\"catchup_route\":");
      if (source->catchup_kind == DIRECT_URL_RTSP_CANDIDATE) {
        DIRECT_APPEND("\"rtsp_helper_candidate\"");
      } else if (source->catchup_template) {
        DIRECT_APPEND("\"direct\"");
      } else {
        DIRECT_APPEND("null");
      }
      DIRECT_APPEND(",\"catchup\":");
      if (!source->catchup_template && source->catchup_kind != DIRECT_URL_RTSP_CANDIDATE) {
        DIRECT_APPEND("null");
      } else {
        DIRECT_APPEND("{\"source_id\":\"");
        DIRECT_ESCAPE(source->id);
        DIRECT_APPEND("\",\"mode\":\"");
        DIRECT_ESCAPE(source->catchup_mode ? source->catchup_mode : "default");
        DIRECT_APPEND("\",\"retention_seconds\":");
        if (source->catchup_retention_seconds > 0) {
          int written = snprintf(retention, sizeof(retention), "%.17g", source->catchup_retention_seconds);
          if (written < 0 || (size_t)written >= sizeof(retention))
            return -1;
          DIRECT_APPEND(retention);
        } else {
          DIRECT_APPEND("null");
        }
        DIRECT_APPEND("}");
      }
      DIRECT_APPEND("}");
    }
    DIRECT_APPEND("]}");
  }
  DIRECT_APPEND("]}");

#undef DIRECT_APPEND
#undef DIRECT_ESCAPE
  return 0;
}

static int direct_proxy_m3u_append_extinf(direct_json_t *json, const direct_source_t *source) {
  if (direct_json_append(json, "#EXTINF:-1 refplayer-source-id=\\\"") != 0 ||
      direct_json_escape(json, source->id) != 0 || direct_json_append(json, "\\\"") != 0)
    return -1;
  if (source->catchup_kind == DIRECT_URL_RTSP_CANDIDATE) {
    /* The Host uses this only as a private routing snapshot. Do not copy the
     * playlist's untrusted catchup-mode token into config/M3U syntax. */
    if (direct_json_append(json, " catchup=\\\"default\\\" catchup-source=\\\"") != 0 ||
        direct_json_escape(json, source->catchup_template) != 0 || direct_json_append(json, "\\\"") != 0)
      return -1;
    if (source->catchup_retention_seconds > 0) {
      char retention_days[64];
      int written = snprintf(retention_days, sizeof(retention_days), "%.17g",
                             source->catchup_retention_seconds / 86400.0);
      if (written < 0 || (size_t)written >= sizeof(retention_days) ||
          direct_json_append(json, " catchup-days=\\\"") != 0 || direct_json_append(json, retention_days) != 0 ||
          direct_json_append(json, "\\\"") != 0)
        return -1;
    }
  }
  /* Server-side metadata is discarded by the Host. Use only the validated
   * opaque ID here so playlist titles/groups cannot become M3U or config
   * syntax when the snapshot is embedded in the helper's private config. */
  return direct_json_append(json, ",refplayer-rtsp\\n");
}

static int direct_json_append_proxy_m3u(direct_json_t *json, const direct_catalog_t *catalog) {
  int has_candidate = 0;

  for (const direct_channel_t *channel = catalog->channels; channel; channel = channel->next)
    for (const direct_source_t *source = channel->sources; source; source = source->next)
      if (source->catchup_kind == DIRECT_URL_RTSP_CANDIDATE)
        has_candidate = 1;
  if (!has_candidate)
    return direct_json_append(json, "null");
  if (direct_json_append(json, "\"#EXTM3U\\n") != 0)
    return -1;

  for (const direct_channel_t *channel = catalog->channels; channel; channel = channel->next) {
    for (const direct_source_t *source = channel->sources; source; source = source->next) {
      const char *proxy_live_url;
      if (source->catchup_kind != DIRECT_URL_RTSP_CANDIDATE)
        continue;
      proxy_live_url = source->catchup_template;
      if (!proxy_live_url || direct_validate_absolute_rtsp_url(proxy_live_url) != 0 ||
          (source->catchup_kind == DIRECT_URL_RTSP_CANDIDATE &&
           (!source->catchup_template || direct_validate_absolute_rtsp_url(source->catchup_template) != 0)) ||
          direct_proxy_m3u_append_extinf(json, source) != 0 || direct_json_escape(json, proxy_live_url) != 0 ||
          direct_json_append(json, "\\n") != 0)
        return -1;
    }
  }
  return direct_json_append(json, "\"");
}

static direct_source_t *direct_find_source(direct_catalog_t *catalog, const char *source_id) {
  for (direct_channel_t *channel = catalog->channels; channel; channel = channel->next)
    for (direct_source_t *source = channel->sources; source; source = source->next)
      if (strcmp(source->id, source_id) == 0)
        return source;
  return NULL;
}

static int direct_strip_seek_controls(char *url, int *begin_offset_seconds, int *end_offset_seconds) {
  static const char *control_names[] = {"r2h-seek-offset", "r2h-seek-name", "r2h-seek-mode", NULL};
  char *query;
  int saw_offset = 0;

  if (!url || !begin_offset_seconds || !end_offset_seconds)
    return -1;
  *begin_offset_seconds = 0;
  *end_offset_seconds = 0;
  query = strchr(url, '?');
  if (!query)
    return 0;

  for (;;) {
    char *fragment = strchr(query, '#');
    char *limit = fragment ? fragment : url + strlen(url);
    char *parameter = query + 1;
    int removed = 0;

    while (parameter < limit) {
      char *parameter_end = memchr(parameter, '&', (size_t)(limit - parameter));
      char *equals;
      size_t parameter_length;
      if (!parameter_end)
        parameter_end = limit;
      equals = memchr(parameter, '=', (size_t)(parameter_end - parameter));
      parameter_length = equals ? (size_t)(equals - parameter) : (size_t)(parameter_end - parameter);

      for (size_t index = 0; control_names[index]; index++) {
        const char *name = control_names[index];
        if (strlen(name) != parameter_length || strncasecmp(parameter, name, parameter_length) != 0)
          continue;

        if (index == 0 && !saw_offset) {
          char value[32];
          size_t value_length = equals ? (size_t)(parameter_end - equals - 1) : 0;
          saw_offset = 1;
          if (value_length > 0 && value_length < sizeof(value)) {
            memcpy(value, equals + 1, value_length);
            value[value_length] = '\0';
            if (http_url_decode(value) != 0 ||
                service_parse_seek_offset_value(value, begin_offset_seconds, end_offset_seconds) != 0) {
              *begin_offset_seconds = 0;
              *end_offset_seconds = 0;
            }
          }
        }

        if (parameter_end < limit) {
          memmove(parameter, parameter_end + 1, strlen(parameter_end + 1) + 1);
        } else if (parameter > query + 1) {
          memmove(parameter - 1, parameter_end, strlen(parameter_end) + 1);
        } else {
          memmove(query, parameter_end, strlen(parameter_end) + 1);
        }
        removed = 1;
        break;
      }
      if (removed)
        break;
      if (parameter_end == limit)
        break;
      parameter = parameter_end + 1;
    }
    if (!removed)
      break;
  }
  return 0;
}

static int direct_add_epoch_offset(long long epoch, int offset, time_t *output) {
  long long adjusted;

  if (!output || (offset > 0 && epoch > INT64_MAX - offset) || (offset < 0 && epoch < INT64_MIN - offset))
    return -1;
  adjusted = epoch + offset;
  *output = (time_t)adjusted;
  return (long long)*output == adjusted ? 0 : -1;
}

static int direct_fill_seek_context(const direct_frame_t *frame, int begin_offset_seconds, int end_offset_seconds,
                                    seek_parse_result_t *context) {
  time_t begin;
  time_t end;
  time_t local_begin;
  time_t local_end;
  struct tm *converted;

  if (direct_add_epoch_offset(frame->start_epoch, begin_offset_seconds, &begin) != 0 ||
      direct_add_epoch_offset(frame->end_epoch, end_offset_seconds, &end) != 0)
    return -1;
  if ((frame->timezone_offset_seconds > 0 && (begin > (time_t)(INT64_MAX - frame->timezone_offset_seconds) ||
                                              end > (time_t)(INT64_MAX - frame->timezone_offset_seconds))))
    return -1;
  local_begin = begin + frame->timezone_offset_seconds;
  local_end = end + frame->timezone_offset_seconds;

  memset(context, 0, sizeof(*context));
  context->has_seek = 1;
  context->has_range_separator = 1;
  context->has_begin = 1;
  context->has_end = 1;
  context->begin_parsed = 1;
  context->end_parsed = 1;
  context->begin_utc = begin;
  context->end_utc = end;
  context->now_utc = time(NULL);
  context->tz_offset_seconds = frame->timezone_offset_seconds;
  snprintf(context->begin_str, sizeof(context->begin_str), "%lld", (long long)begin);
  snprintf(context->end_str, sizeof(context->end_str), "%lld", (long long)end);

  converted = gmtime(&begin);
  if (!converted)
    return -1;
  context->begin_tm_utc = *converted;
  converted = gmtime(&end);
  if (!converted)
    return -1;
  context->end_tm_utc = *converted;
  converted = gmtime(&local_begin);
  if (!converted)
    return -1;
  context->begin_tm_local = *converted;
  converted = gmtime(&local_end);
  if (!converted)
    return -1;
  context->end_tm_local = *converted;
  return 0;
}

static int direct_run_catalog(direct_frame_t *frame) {
  direct_catalog_t catalog = {0};
  direct_json_t json = {0};
  int parse_result = direct_catalog_parse(frame->m3u, frame->base_url, &catalog);
  int result;

  if (parse_result != 0) {
    direct_catalog_free(&catalog);
    return direct_error(parse_result == -2 ? DIRECT_EXIT_INTERNAL : DIRECT_EXIT_DATA,
                        parse_result == -2 ? "allocation_failed" : "invalid_playlist",
                        parse_result == -2 ? "The direct catalog could not be allocated."
                                           : "The M3U playlist is invalid or has no playable entries.");
  }
  if (direct_catalog_json(&catalog, &json) != 0) {
    direct_catalog_free(&catalog);
    free(json.data);
    return direct_error(DIRECT_EXIT_INTERNAL, "output_too_large",
                        "The direct catalog exceeds the supported output limit.");
  }
  result = direct_write_json(json.data) == 0 ? DIRECT_EXIT_OK : DIRECT_EXIT_INTERNAL;
  direct_catalog_free(&catalog);
  free(json.data);
  return result;
}

static int direct_run_resolve(direct_frame_t *frame) {
  direct_catalog_t catalog = {0};
  direct_json_t json = {0};
  direct_source_t *source;
  seek_parse_result_t context;
  char *template;
  char resolved[DIRECT_MAX_URL];
  int begin_offset_seconds;
  int end_offset_seconds;
  int parse_result = direct_catalog_parse(frame->m3u, frame->base_url, &catalog);
  int result;

  if (parse_result != 0) {
    direct_catalog_free(&catalog);
    return direct_error(parse_result == -2 ? DIRECT_EXIT_INTERNAL : DIRECT_EXIT_DATA,
                        parse_result == -2 ? "allocation_failed" : "invalid_playlist",
                        "The catch-up source could not be loaded from the M3U playlist.");
  }
  source = direct_find_source(&catalog, frame->source_id);
  if (!source) {
    direct_catalog_free(&catalog);
    return direct_error(DIRECT_EXIT_DATA, "unknown_source", "The catch-up source identifier is not in the playlist.");
  }
  if (!source->catchup_template) {
    direct_catalog_free(&catalog);
    return direct_error(DIRECT_EXIT_DATA, "catchup_unavailable", "The selected source has no direct catch-up URL.");
  }
  template = strdup(source->catchup_template);
  if (!template) {
    direct_catalog_free(&catalog);
    return direct_error(DIRECT_EXIT_INTERNAL, "allocation_failed", "The catch-up URL could not be allocated.");
  }
  if (direct_strip_seek_controls(template, &begin_offset_seconds, &end_offset_seconds) != 0 ||
      direct_fill_seek_context(frame, begin_offset_seconds, end_offset_seconds, &context) != 0 ||
      url_template_resolve(template, &context, resolved, sizeof(resolved)) != 0 ||
      ((source->catchup_kind == DIRECT_URL_HTTP && direct_validate_absolute_http_url(resolved) != 0) ||
       (source->catchup_kind == DIRECT_URL_NATIVE &&
        (!direct_is_native_url(resolved) || direct_url_has_unsafe_byte(resolved))) ||
       (source->catchup_kind != DIRECT_URL_HTTP && source->catchup_kind != DIRECT_URL_NATIVE)) ||
      strpbrk(resolved, "{}") != NULL) {
    free(template);
    direct_catalog_free(&catalog);
    return direct_error(DIRECT_EXIT_DATA, "resolve_failed", "The catch-up URL template could not be resolved.");
  }
  free(template);

  if (direct_json_append(&json, "{\"schema_version\":2,\"source_id\":\"") != 0 ||
      direct_json_escape(&json, source->id) != 0 || direct_json_append(&json, "\",\"url\":\"") != 0 ||
      direct_json_escape(&json, resolved) != 0 || direct_json_append(&json, "\"}") != 0) {
    direct_catalog_free(&catalog);
    free(json.data);
    return direct_error(DIRECT_EXIT_INTERNAL, "output_too_large",
                        "The resolved catch-up URL exceeds the supported output limit.");
  }
  result = direct_write_json(json.data) == 0 ? DIRECT_EXIT_OK : DIRECT_EXIT_INTERNAL;
  direct_catalog_free(&catalog);
  free(json.data);
  return result;
}

int refplayer_direct_dispatch(int argc, char *argv[], int *exit_code) {
  direct_frame_t frame;
  int resolve;
  int frame_result;

  if (!exit_code || argc < 2 ||
      (strcmp(argv[1], DIRECT_CATALOG_COMMAND) != 0 && strcmp(argv[1], DIRECT_RESOLVE_COMMAND) != 0))
    return 0;
  resolve = strcmp(argv[1], DIRECT_RESOLVE_COMMAND) == 0;

  /* Shared parser/template helpers use logger() for daemon diagnostics. The
   * one-shot protocol reserves stdout exclusively for one JSON document. */
  config.verbosity = (loglevel_t)-1;
  if (argc != 2) {
    *exit_code = direct_error(DIRECT_EXIT_USAGE, "invalid_arguments",
                              "The one-shot command does not accept command-line input values.");
    return 1;
  }
  frame_result = direct_parse_frame(resolve, &frame);
  if (frame_result != 0) {
    direct_frame_free(&frame);
    *exit_code = direct_error(frame_result == -2 ? DIRECT_EXIT_INTERNAL : DIRECT_EXIT_USAGE,
                              frame_result == -2 ? "allocation_failed" : "invalid_frame",
                              frame_result == -2 ? "The one-shot input could not be allocated."
                                                 : "The one-shot stdin frame is invalid or exceeds its limit.");
    return 1;
  }

  *exit_code = resolve ? direct_run_resolve(&frame) : direct_run_catalog(&frame);
  direct_frame_free(&frame);
  return 1;
}
