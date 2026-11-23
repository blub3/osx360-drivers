int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("XenosMultiClient 1");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--version]\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    int ends[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
        perror("socketpair");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    const pid_t child = fork();
    if (child == 0) {
        close(ends[0]);
        alarm(20);
        const int passed = runWorker(ends[1]);
        close(ends[1]);
        _exit(passed ? 0 : 1);
    }

    close(ends[1]);
    if (child < 0) {
        perror("fork");
        close(ends[0]);
        return 1;
    }

    alarm(20);
    const int ownerPassed = runOwner(ends[0], child);
    close(ends[0]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    alarm(0);
    if (!ownerPassed || waited != child || !WIFEXITED(status) || WEXITSTATUS(status))
        return 1;
    puts("PASS multi_client processes=2 verified_clears=4 pixels=4096 fifo=1 exit_cleanup=1");
    return 0;
}
