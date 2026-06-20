#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/sched.h> /* Definition of struct clone_args */
#include <sched.h>       /* Definition of CLONE_* constants */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

void set_map(char* file, int inside_id, int outside_id, int len) {
    FILE* mapfd = fopen(file, "w");
    if (NULL == mapfd) {
        fprintf(stderr, "ERROR opening %s: %s\n", file, strerror(errno));
        return;
    }
    int ret = fprintf(mapfd, "%d %d %d\n", inside_id, outside_id, len);
    if (ret < 0) {
        fprintf(stderr, "ERROR writing %s: %s\n", file, strerror(errno));
    }
    if (fclose(mapfd) != 0) {
        fprintf(stderr, "ERROR closing %s: %s\n", file, strerror(errno));
    }
}

void set_map_s(char* file, char* s) {
    FILE* mapfd = fopen(file, "w");
    if (NULL == mapfd) {
        fprintf(stderr, "ERROR opening %s: %s\n", file, strerror(errno));
        return;
    }
    int ret = fprintf(mapfd, "%s\n", s);
    if (ret < 0) {
        fprintf(stderr, "ERROR writing %s: %s\n", file, strerror(errno));
    }
    if (fclose(mapfd) != 0) {
        fprintf(stderr, "ERROR closing %s: %s\n", file, strerror(errno));
    }
}

#define STACK_SIZE (1024 * 1024)
static char container_stack[STACK_SIZE];
int container_main(void* arg) {
    int* fds = (int*)arg;
    int pipe_read_fd = fds[0];

    close(fds[1]);

    fprintf(stderr, "Child pid = %d -- waiting for uid/gid mapping...\n",
            getpid());

    char ch;
    if (read(pipe_read_fd, &ch, 1) != 0) {
        fprintf(stderr, "Child pid = %d -- received signal, mapping ready.\n",
                getpid());
    }
    close(pipe_read_fd);

    fprintf(stderr, "Child uid=%d gid=%d euid=%d\n", getuid(), getgid(),
            geteuid());

    fprintf(stderr, "Child pid = %d -- step into container.\n", getpid());
    const char hostname[] = "container_lite";
    sethostname(hostname, sizeof(hostname) - 1);

    if (mount("rootfs", "rootfs", NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("bind rootfs");
    }
    if (mount(NULL, "rootfs", NULL, MS_PRIVATE, NULL) != 0) {
        perror("private rootfs");
    }

    umount2("rootfs/proc", MNT_DETACH);
    if (mount("proc", "rootfs/proc", "proc", 0, NULL) != 0) {
        perror("proc");
    }
    umount2("rootfs/sys", MNT_DETACH);
    if (mount("sysfs", "rootfs/sys", "sysfs", 0, NULL) != 0) {
        perror("sys");
    }
    umount2("rootfs/tmp", MNT_DETACH);
    if (mount("none", "rootfs/tmp", "tmpfs", 0, NULL) != 0) {
        perror("tmp");
    }
    umount2("rootfs/dev/pts", MNT_DETACH);
    umount2("rootfs/dev/shm", MNT_DETACH);
    if (mount("devpts", "rootfs/dev/pts", "devpts", 0, NULL) != 0) {
        perror("dev/pts");
    }
    if (mount("shm", "rootfs/dev/shm", "tmpfs", 0, NULL) != 0) {
        perror("dev/shm");
    }
    umount2("rootfs/run", MNT_DETACH);
    if (mount("tmpfs", "rootfs/run", "tmpfs", 0, NULL) != 0) {
        perror("run");
    }

    mkdir("rootfs/.put_old", 0755);
    if (syscall(SYS_pivot_root, "rootfs", "rootfs/.put_old") != 0) {
        perror("pivot_root");
    }
    if (chdir("/") != 0) {
        perror("chdir");
    }
    umount2("/.put_old", MNT_DETACH);
    rmdir("/.put_old");

    char* const container_args[] = {"/bin/sh", NULL};
    if (execv(container_args[0], container_args)) {
        perror("execv");
    }
    return 0;
}

int main(void) {
    int pipefd[2];
    int fds[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }
    fds[0] = pipefd[0];
    fds[1] = pipefd[1];

    int container_pid =
        clone(container_main, container_stack + sizeof(container_stack),
              CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWIPC |
                  CLONE_NEWNS | SIGCHLD,
              fds);
    if (container_pid < 0) {
        fprintf(stderr, "start container error: %s\n", strerror(errno));
        return container_pid;
    }

    fprintf(stderr, "Parent pid = %d -- start container pid = %d.\n", getpid(),
            container_pid);

    close(pipefd[0]);

    const int gid = getgid(), uid = getuid();
    char file[256];

    sprintf(file, "/proc/%d/setgroups", container_pid);
    set_map_s(file, "deny");

    sprintf(file, "/proc/%d/uid_map", container_pid);
    set_map(file, 0, uid, 1);
    sprintf(file, "/proc/%d/gid_map", container_pid);
    set_map(file, 0, gid, 1);

    fprintf(stderr, "User namespace mapping configured, signaling child.\n");
    close(pipefd[1]);

    waitpid(container_pid, NULL, 0);
    return 0;
}
