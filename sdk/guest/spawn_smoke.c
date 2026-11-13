#include "posix.h"

/* End-to-end process test: parent spawns a new ring-3 ELF image and waits
 * for its real exit status. Processes do not share private memory pages. */
int main(int argc, char **argv, char **envp) {
    if (argc == 2 && argv[1] && argv[1][0] == 'C') {
        static const char child[] = "LUNA CHILD EXECUTE OK\n";
        if (write(1, child, sizeof(child) - 1u) != (long)(sizeof(child) - 1u))
            return 8;
        return 9;
    }
    const char *args[] = {"/tmp/spawn.elf", "Child", 0};
    int pid = luna_spawn("/tmp/spawn.elf", args, (const char *const *)envp);
    if (pid < 1)
        return 71;
    int status = -1;
    if (luna_waitpid(pid, &status, 0) != pid || status != (9 << 8))
        return 72;
    static const char parent[] = "LUNA SPAWN WAIT OK\n";
    if (write(1, parent, sizeof(parent) - 1u) != (long)(sizeof(parent) - 1u))
        return 73;
    return 0;
}
