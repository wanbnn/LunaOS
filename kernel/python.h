#ifndef LUNA_PYTHON_H
#define LUNA_PYTHON_H

#include <stddef.h>

int python_command(int argc, char **argv, char *out, size_t cap);
int python_run_file(const char *path, int argc, char **argv, char *out, size_t cap);

#endif
