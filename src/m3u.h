#ifndef __M3U_H__
#define __M3U_H__

#include <stdint.h>
#include <stdio.h>

#define M3U_CATALOG_ID_SIZE 33

/* RefPlayer-facing channel catalog.  This deliberately stores only normalized
 * helper routes and presentation metadata; upstream protocol URLs remain
 * private to service_t. */
typedef struct m3u_catalog_channel_s {
  char id[M3U_CATALOG_ID_SIZE];
  char *title;
  char *group_title;
  char *service_name;
  char *catchup_service_name;
  double catchup_retention_seconds;
  int source_kind;
  struct m3u_catalog_channel_s *next;
} m3u_catalog_channel_t;

/* M3U cache structure for external M3U state tracking */
typedef struct {
  int retry_count;         /* Current retry count (0-8) */
  int64_t next_retry_time; /* Next retry time in milliseconds (0 if not retrying) */

  /* Transformed M3U playlist buffer */
  char *transformed_m3u;             /* Dynamic buffer for transformed playlist */
  size_t transformed_m3u_size;       /* Total allocated size */
  size_t transformed_m3u_used;       /* Used size (content length) */
  size_t transformed_m3u_inline_end; /* Marks end of inline content */
  int transformed_m3u_has_header;    /* 1 if #EXTM3U header was added, 0 otherwise
                                      */

  /* ETag for transformed M3U playlist */
  char transformed_m3u_etag[33];  /* MD5 hash as hex string */
  int transformed_m3u_etag_valid; /* 1 if etag is valid, 0 otherwise */

  /* Structured native-player catalog, built by the same parser that creates
   * streaming services. */
  m3u_catalog_channel_t *catalog_channels;
  m3u_catalog_channel_t *catalog_channels_tail;
  size_t catalog_channel_count; /* Number of top-level RefPlayer entries */
  int external_catalog_ready;
} m3u_cache_t;

/* Parse M3U content and create services
 * content: M3U content as string
 * source_url: source URL of the M3U (for identification, can be NULL for
 * inline) Returns: 0 on success, -1 on error
 */
int m3u_parse_and_create_services(const char *content, const char *source_url);

/* Check if a line is an M3U header
 * line: line to check
 * Returns: 1 if line is #EXTM3U, 0 otherwise
 */
int m3u_is_header(const char *line);

/* Shared M3U lexical primitives used by both the server parser and the
 * RefPlayer one-shot direct parser. */
char *m3u_extract_tvg_url(const char *line);
int m3u_extract_attribute_at(const char *line, const char *attr_name, size_t occurrence, char *value,
                             size_t value_size);
int m3u_extract_attribute(const char *line, const char *attr_name, char *value, size_t value_size);
int m3u_extract_service_name(const char *line, char *name, size_t name_size);

/* Get the transformed M3U playlist
 * Returns: transformed M3U content (static buffer, valid until next parse)
 */
const char *m3u_get_transformed_playlist(void);

/* Generate complete M3U playlist dynamically based on request headers
 * host_header: HTTP Host header (can be NULL)
 * x_forwarded_host: X-Forwarded-Host header (can be NULL, only used when
 * xff=yes) x_forwarded_proto: X-Forwarded-Proto header (can be NULL, only used
 * when xff=yes) Returns: malloc'd complete M3U content with header and replaced
 * placeholders, caller must free
 */
char *m3u_generate_playlist(const char *host_header, const char *x_forwarded_host, const char *x_forwarded_proto);

/* Get the ETag for the current transformed M3U playlist
 * Returns: ETag string (static buffer), or NULL if no playlist
 */
const char *m3u_get_etag(void);

/* Reset the transformed M3U playlist buffer
 * Called when configuration is reloaded
 */
void m3u_reset_transformed_playlist(void);

/* Reset only the external M3U playlist buffer
 * Called when external M3U is reloaded (keeps inline content)
 */
void m3u_reset_external_playlist(void);

/* Create a restorable snapshot of the M3U cache.
 * Returns 0 on success, -1 on allocation failure.
 */
int m3u_cache_snapshot(m3u_cache_t *snapshot);

/* Free resources owned by an M3U cache snapshot. */
void m3u_cache_snapshot_free(m3u_cache_t *snapshot);

/* Restore the global M3U cache from a snapshot.
 * The snapshot ownership is moved into the global cache.
 */
void m3u_cache_restore_snapshot(m3u_cache_t *snapshot);

/* Get server address as complete URL
 * Priority: hostname config > non-upstream interface private IP > non-upstream
 * interface public IP > upstream interface IP > localhost Returns: malloc'd
 * string containing complete URL (protocol://host:port/ or
 * protocol://host:port/path/) Always ends with trailing slash '/' Port is
 * omitted if it's 80 for http or 443 for https Caller must free the returned
 * string
 */
char *get_server_address(void);

/* Async external M3U reloading (non-blocking, for worker processes)
 * epfd: epoll file descriptor for async I/O
 * Returns: 0 if async fetch started, -1 on error or if not configured
 */
int m3u_reload_external_async(int epfd);

/* Get M3U cache for retry state tracking
 * Returns: pointer to m3u_cache_t structure
 */
m3u_cache_t *m3u_get_cache(void);

/* Generate the versioned RefPlayer catalog as JSON.  URLs are generated from
 * request headers exactly like /playlist.m3u.  Returns malloc'd JSON. */
char *m3u_generate_refplayer_catalog(const char *host_header, const char *x_forwarded_host,
                                     const char *x_forwarded_proto);

/* Resolve opaque source IDs used by the RefPlayer catalog. */
const m3u_catalog_channel_t *m3u_catalog_find_source(const char *source_id);

/* True once the configured external M3U has completed its first successful
 * parse, or immediately when no external M3U is configured. */
int m3u_refplayer_is_ready(void);

#endif /* __M3U_H__ */
