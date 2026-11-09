#ifndef LUNA_PYTHON_H
#define LUNA_PYTHON_H

#include <stdbool.h>
#include <stddef.h>

int micropython_command(int argc, char **argv, char *out, size_t cap);
int python_run_file(const char *path, int argc, char **argv, char *out, size_t cap);

/* Resolve the active environment's library directory, or the user's default. */
bool python_library_path(char *out);
int python_venv_command(int argc, char **argv, char *out, size_t cap);
int python_pip_command(int argc, char **argv, char *out, size_t cap);
int python_venv_create(const char *path, char *out, size_t cap);

int cpython_command(int argc, char **argv, char *out, size_t cap);

#endif
