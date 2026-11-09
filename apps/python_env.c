#include "install.h"
#include "luna.h"
#include "python.h"

#define LIB_DIR "/lib"
#define ENV_CONFIG "/pyvenv.cfg"

static char active_env[PATH_CAP];

static bool under_home(const char *path) {
    const char *home = account_home();
    size_t len = strlen(home);
    return !strncmp(path, home, len) && path[len] == '/';
}

static bool directory(const char *path) {
    const File *file = fs_find(path);
    return file && file->dir;
}

static bool env_paths(const char *path, char root[PATH_CAP], char lib[PATH_CAP],
                      char config[PATH_CAP]) {
    if (!fs_path(path, root) || !under_home(root) ||
        strlen(root) + sizeof(ENV_CONFIG) > PATH_CAP || strlen(root) + strlen(ENV_CONFIG) > 120)
        return false;
    strcpy(lib, root);
    strcat(lib, LIB_DIR);
    strcpy(config, root);
    strcat(config, ENV_CONFIG);
    return true;
}

static bool valid_env(const char *lib, const char *config) {
    const File *file = fs_find(config);
    return file && !file->dir && directory(lib);
}

bool python_library_path(char *out) {
    if (!active_env[0] || !under_home(active_env)) {
        strcpy(out, account_home());
        strcat(out, LIB_DIR);
        return true;
    }
    char root[PATH_CAP], config[PATH_CAP];
    return env_paths(active_env, root, out, config) && valid_env(out, config);
}

int python_venv_create(const char *path, char *out, size_t cap) {
    char root[PATH_CAP], lib[PATH_CAP], config[PATH_CAP];
    if (!env_paths(path, root, lib, config)) {
        append(out, "venv: choose a path within your home\n", cap);
        return 2;
    }
    if (valid_env(lib, config)) {
        append(out, "venv: environment already exists\n", cap);
        return 0;
    }
    if (fs_create(root, true)) {
        append(out, "venv: cannot create environment directory\n", cap);
        return 1;
    }
    if (fs_create(lib, true) || fs_write(config, "implementation = MicroPython\n", false)) {
        fs_remove(lib);
        fs_remove(root);
        append(out, "venv: cannot initialize environment\n", cap);
        return 1;
    }
    append(out, "Created Python environment: ", cap);
    append(out, root, cap);
    append(out, "\n", cap);
    return 0;
}

int python_venv_command(int argc, char **argv, char *out, size_t cap) {
    if (argc == 3 && !strcmp(argv[1], "create"))
        return python_venv_create(argv[2], out, cap);
    if (argc == 3 && !strcmp(argv[1], "activate")) {
        char root[PATH_CAP], lib[PATH_CAP], config[PATH_CAP];
        if (!env_paths(argv[2], root, lib, config) || !valid_env(lib, config)) {
            append(out, "venv: environment not found\n", cap);
            return 1;
        }
        strcpy(active_env, root);
        append(out, "Activated ", cap);
        append(out, root, cap);
        append(out, "\n", cap);
        return 0;
    }
    if ((argc == 2 && !strcmp(argv[1], "deactivate")) ||
        (argc == 1 && !strcmp(argv[0], "deactivate"))) {
        active_env[0] = 0;
        append(out, "Python environment deactivated\n", cap);
        return 0;
    }
    append(out, "usage: venv create PATH | activate PATH | deactivate\n", cap);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}
