#ifndef LUNA_GIT_H
#define LUNA_GIT_H
#include <stddef.h>
/* Native Git-compatible repository metadata and SHA-1 blob identifiers.
 * git add/commit/clone/push/pull are NOT implemented in this first phase. */
int luna_git_command(int argc, char **argv, char *out, size_t cap);
#endif
