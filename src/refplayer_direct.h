#ifndef __REFPLAYER_DIRECT_H__
#define __REFPLAYER_DIRECT_H__

/**
 * Dispatch RefPlayer one-shot commands before normal daemon configuration.
 *
 * --refplayer-direct-catalog consumes:
 *   line 1: RTP2HTTPD-REFPLAYER-CATALOG/1
 *   line 2: absolute playlist URL
 *   remainder: M3U to EOF
 *
 * --refplayer-direct-resolve consumes:
 *   line 1: RTP2HTTPD-REFPLAYER-RESOLVE/1
 *   line 2: absolute playlist URL
 *   line 3: opaque source ID
 *   lines 4-6: start epoch, end epoch, and local UTC offset seconds
 *   remainder: M3U to EOF
 *
 * Both modes emit exactly one versioned JSON document on stdout and never
 * initialize daemon status, listeners, the supervisor, or workers.
 *
 * @return 1 when a one-shot command was recognized (exit_code is populated),
 *         0 when argv belongs to the normal daemon.
 */
int refplayer_direct_dispatch(int argc, char *argv[], int *exit_code);

#endif /* __REFPLAYER_DIRECT_H__ */
