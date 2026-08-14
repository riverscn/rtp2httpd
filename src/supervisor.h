#ifndef __SUPERVISOR_H__
#define __SUPERVISOR_H__

#define SUPERVISOR_WORKER_ID (-1)

/**
 * Prepare the opt-in RefPlayer host lifetime contract.
 *
 * When RTP2HTTPD_REFPLAYER_LIFETIME_STDIN is exactly "1", the calling
 * process becomes the leader of a new process group and supervisor_run()
 * treats EOF/HUP on standard input as a graceful shutdown request.  When the
 * environment variable is absent or has any other value, this is a no-op.
 *
 * This must be called before the supervisor initializes listeners or forks
 * workers so the helper server is isolated from its host's process group.
 *
 * @return 0 on success or when disabled, -1 when process-group setup fails
 */
int supervisor_prepare_refplayer_lifetime(void);

/**
 * Supervisor module for multi-process worker management
 *
 * The supervisor is responsible for:
 * - Spawning and managing worker processes
 * - Monitoring worker health and restarting crashed workers
 * - Rate-limiting restarts to prevent restart storms
 *
 * Future extensions (not yet implemented):
 * - SIGHUP handling for configuration reload
 */

/**
 * Run the supervisor process
 *
 * This function forks worker processes and monitors them.
 * When a worker exits unexpectedly, it will be restarted.
 * The supervisor exits when it receives SIGTERM/SIGINT.
 *
 * RefPlayer-owned child processes monitor their supervisor's lifetime (using
 * PR_SET_PDEATHSIG on Linux and EVFILT_PROC plus a parent-PID guard on BSD
 * platforms) so an unexpectedly terminated supervisor cannot orphan workers.
 *
 * @return 0 on success, non-zero on error
 */
int supervisor_run(void);

/**
 * Run the worker process business logic
 *
 * This function contains the main worker logic extracted from main():
 * - Creates and binds listening sockets
 * - Runs the event loop
 * - Cleans up resources on exit
 *
 * @return 0 on success, non-zero on error
 */
int run_worker(void);

#endif /* __SUPERVISOR_H__ */
