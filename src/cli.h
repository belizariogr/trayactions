#ifndef CLI_H
#define CLI_H

/**
 * Handle --run / --run-or-focus if present.
 * Returns -1 when argv is not a CLI run mode (caller should start the tray).
 * Otherwise returns a process exit code (0 on success).
 */
int cli_try_handle(int argc, char **argv);

#endif
