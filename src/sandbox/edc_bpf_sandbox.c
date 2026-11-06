#ifdef __linux__
#define _GNU_SOURCE
#include <sys/syscall.h>
#endif

#include "edc_bpf_sandbox.h"
#include "edc_compat.h"
#include "log.h"
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

/* Architecture detection for BPF audit check */
#if defined(__x86_64__)
#define ARCH_NR AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH_NR AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define ARCH_NR AUDIT_ARCH_ARM
#else
#warning "edc_bpf_sandbox: unsupported architecture, BPF sandbox disabled"
#endif

#if defined(ARCH_NR)

/* ioctl() request that sets inode flags (the chattr path). Landlock does not
 * mediate ioctl, so this single request is denied by argument in
 * build_filter(). Computed with the kernel's _IOW encoding (dir = WRITE << 30,
 * size << 16, type << 8, nr) so it is correct for both LP64 (0x40086602) and
 * ILP32 (0x40046602). */
#define EDC_FS_IOC_SETFLAGS ((1u << 30) | ((unsigned int)sizeof(long) << 16) | ((unsigned int)'f' << 8) | 2u)

/*
 * ---- Syscall fallback definitions ----
 *
 * Ensure every SYS_xxx macro we reference is defined.
 * <sys/syscall.h> provides arch-correct numbers (SYS_read=0 on x86_64,
 * SYS_read=63 on aarch64, etc.) and omits macros for syscalls that don't
 * exist on the target architecture.  We default missing ones to 0 so that
 * filter construction can skip them with a simple "(0) continue" check.
 */

/* Syscalls not present on all architectures (e.g. aarch64 uses *at variants) */
#ifndef SYS_stat
#define SYS_stat 0
#endif
#ifndef SYS_lstat
#define SYS_lstat 0
#endif
#ifndef SYS_fstat
#define SYS_fstat 0
#endif
#ifndef SYS_mmap
#define SYS_mmap 0
#endif
#ifndef SYS_newfstatat
#define SYS_newfstatat 0
#endif
#ifndef SYS_sync_file_range
#define SYS_sync_file_range 0
#endif
#ifndef SYS_fadvise64
#define SYS_fadvise64 0
#endif
#ifndef SYS_poll
#define SYS_poll 0
#endif
#ifndef SYS_access
#define SYS_access 0
#endif
#ifndef SYS_futimesat
#define SYS_futimesat 0
#endif
#ifndef SYS_utimes
#define SYS_utimes 0
#endif
#ifndef SYS_open
#define SYS_open 0
#endif
#ifndef SYS_pipe
#define SYS_pipe 0
#endif
#ifndef SYS_fork
#define SYS_fork 0
#endif
#ifndef SYS_vfork
#define SYS_vfork 0
#endif
#ifndef SYS_select
#define SYS_select 0
#endif
#ifndef SYS_getdents
#define SYS_getdents 0
#endif
#ifndef SYS_getpgrp
#define SYS_getpgrp 0
#endif
#ifndef SYS_setsid
#define SYS_setsid 0
#endif
#ifndef SYS_dup2
#define SYS_dup2 0
#endif
#ifndef SYS_epoll_create
#define SYS_epoll_create 0
#endif
#ifndef SYS_epoll_wait
#define SYS_epoll_wait 0
#endif
#ifndef SYS_eventfd
#define SYS_eventfd 0
#endif
#ifndef SYS_inotify_init
#define SYS_inotify_init 0
#endif
#ifndef SYS_signalfd
#define SYS_signalfd 0
#endif
#ifndef SYS_pause
#define SYS_pause 0
#endif
#ifndef SYS_alarm
#define SYS_alarm 0
#endif
#ifndef SYS_utime
#define SYS_utime 0
#endif
#ifndef SYS_time
#define SYS_time 0
#endif
#ifndef SYS_creat
#define SYS_creat 0
#endif
#ifndef SYS_mkdir
#define SYS_mkdir 0
#endif
#ifndef SYS_rmdir
#define SYS_rmdir 0
#endif
#ifndef SYS_rename
#define SYS_rename 0
#endif
#ifndef SYS_link
#define SYS_link 0
#endif
#ifndef SYS_unlink
#define SYS_unlink 0
#endif
#ifndef SYS_symlink
#define SYS_symlink 0
#endif
#ifndef SYS_readlink
#define SYS_readlink 0
#endif
#ifndef SYS_chmod
#define SYS_chmod 0
#endif
#ifndef SYS_chown
#define SYS_chown 0
#endif
#ifndef SYS_lchown
#define SYS_lchown 0
#endif
#ifndef SYS_mknod
#define SYS_mknod 0
#endif
#ifndef SYS_getrlimit
#define SYS_getrlimit 0
#endif
#ifndef SYS_restart_syscall
#define SYS_restart_syscall 0
#endif
#ifndef SYS_prlimit64
#define SYS_prlimit64 0
#endif
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 0
#endif
#ifndef SYS_rseq
#define SYS_rseq 0
#endif
#ifndef SYS_renameat2
#define SYS_renameat2 0
#endif

/* Syscalls removed from Linux (absent on all modern kernels) */
#ifndef SYS_afs_syscall
#define SYS_afs_syscall 0
#endif
#ifndef SYS_tuxcall
#define SYS_tuxcall 0
#endif
#ifndef SYS_security
#define SYS_security 0
#endif
#ifndef SYS_vserver
#define SYS_vserver 0
#endif
#ifndef SYS_nfsservctl
#define SYS_nfsservctl 0
#endif
#ifndef SYS_getpmsg
#define SYS_getpmsg 0
#endif
#ifndef SYS_putpmsg
#define SYS_putpmsg 0
#endif
#ifndef SYS__sysctl
#define SYS__sysctl 0
#endif
#ifndef SYS_uselib
#define SYS_uselib 0
#endif
#ifndef SYS_create_module
#define SYS_create_module 0
#endif
#ifndef SYS_query_module
#define SYS_query_module 0
#endif
#ifndef SYS_get_kernel_syms
#define SYS_get_kernel_syms 0
#endif
#ifndef SYS_ustat
#define SYS_ustat 0
#endif
#ifndef SYS_sysfs
#define SYS_sysfs 0
#endif
#ifndef SYS_syslog
#define SYS_syslog 0
#endif
#ifndef SYS_vhangup
#define SYS_vhangup 0
#endif
#ifndef SYS_acct
#define SYS_acct 0
#endif
#ifndef SYS_modify_ldt
#define SYS_modify_ldt 0
#endif
#ifndef SYS_set_thread_area
#define SYS_set_thread_area 0
#endif
#ifndef SYS_get_thread_area
#define SYS_get_thread_area 0
#endif
#ifndef SYS_lookup_dcookie
#define SYS_lookup_dcookie 0
#endif
#ifndef SYS_io_setup
#define SYS_io_setup 0
#endif
#ifndef SYS_remap_file_pages
#define SYS_remap_file_pages 0
#endif
#ifndef SYS_epoll_ctl_old
#define SYS_epoll_ctl_old 0
#endif
#ifndef SYS_epoll_wait_old
#define SYS_epoll_wait_old 0
#endif

/* Syscalls added after ~Linux 3.17 (may be absent from older headers) */
#ifndef SYS_membarrier
#define SYS_membarrier 0
#endif
#ifndef SYS_mlock2
#define SYS_mlock2 0
#endif
#ifndef SYS_copy_file_range
#define SYS_copy_file_range 0
#endif
#ifndef SYS_preadv2
#define SYS_preadv2 0
#endif
#ifndef SYS_pwritev2
#define SYS_pwritev2 0
#endif
#ifndef SYS_pkey_mprotect
#define SYS_pkey_mprotect 0
#endif
#ifndef SYS_pkey_alloc
#define SYS_pkey_alloc 0
#endif
#ifndef SYS_pkey_free
#define SYS_pkey_free 0
#endif
#ifndef SYS_statx
#define SYS_statx 0
#endif
#ifndef SYS_io_pgetevents
#define SYS_io_pgetevents 0
#endif
#ifndef SYS_execveat
#define SYS_execveat 0
#endif
#ifndef SYS_memfd_create
#define SYS_memfd_create 0
#endif
#ifndef SYS_getrandom
#define SYS_getrandom 0
#endif
#ifndef SYS_kexec_file_load
#define SYS_kexec_file_load 0
#endif
#ifndef SYS_kexec_load
#define SYS_kexec_load 0
#endif
#ifndef SYS_bpf
#define SYS_bpf 0
#endif
#ifndef SYS_kcmp
#define SYS_kcmp 0
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 0
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 0
#endif
#ifndef SYS_process_mrelease
#define SYS_process_mrelease 0
#endif
#ifndef SYS_userfaultfd
#define SYS_userfaultfd 0
#endif
#ifndef SYS_sched_getattr
#define SYS_sched_getattr 0
#endif
#ifndef SYS_sched_setattr
#define SYS_sched_setattr 0
#endif
#ifndef SYS_seccomp
#define SYS_seccomp 0
#endif
#ifndef SYS_finit_module
#define SYS_finit_module 0
#endif

/* Syscalls added after ~Linux 5.x */
#ifndef SYS_openat2
#define SYS_openat2 0
#endif
/* fchmodat2 (452) and the *at variants of the xattr mutators (Linux 6.13+).
 * Older headers lack them; the filter skips a rule whose number is 0. */
#ifndef SYS_fchmodat2
#define SYS_fchmodat2 0
#endif
#ifndef SYS_setxattrat
#define SYS_setxattrat 0
#endif
#ifndef SYS_removexattrat
#define SYS_removexattrat 0
#endif
#ifndef SYS_clone3
#define SYS_clone3 0
#endif
#ifndef SYS_close_range
#define SYS_close_range 0
#endif
#ifndef SYS_fsopen
#define SYS_fsopen 0
#endif
#ifndef SYS_fsconfig
#define SYS_fsconfig 0
#endif
#ifndef SYS_fsmount
#define SYS_fsmount 0
#endif
#ifndef SYS_fspick
#define SYS_fspick 0
#endif
#ifndef SYS_open_tree
#define SYS_open_tree 0
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 0
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 0
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 0
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 0
#endif
#ifndef SYS_io_uring_enter
#define SYS_io_uring_enter 0
#endif
#ifndef SYS_io_uring_register
#define SYS_io_uring_register 0
#endif
#ifndef SYS_open_by_handle_at
#define SYS_open_by_handle_at 0
#endif
#ifndef SYS_name_to_handle_at
#define SYS_name_to_handle_at 0
#endif
#ifndef SYS_setns
#define SYS_setns 0
#endif
#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 0
#endif
#ifndef SYS_process_vm_writev
#define SYS_process_vm_writev 0
#endif
#ifndef SYS_iopl
#define SYS_iopl 0
#endif
#ifndef SYS_ioperm
#define SYS_ioperm 0
#endif

/* SysV IPC — may be compiled out on some kernel configs */
#ifndef SYS_semget
#define SYS_semget 0
#endif
#ifndef SYS_semop
#define SYS_semop 0
#endif
#ifndef SYS_semctl
#define SYS_semctl 0
#endif
#ifndef SYS_semtimedop
#define SYS_semtimedop 0
#endif
#ifndef SYS_shmget
#define SYS_shmget 0
#endif
#ifndef SYS_shmat
#define SYS_shmat 0
#endif
#ifndef SYS_shmctl
#define SYS_shmctl 0
#endif
#ifndef SYS_shmdt
#define SYS_shmdt 0
#endif
#ifndef SYS_msgget
#define SYS_msgget 0
#endif
#ifndef SYS_msgsnd
#define SYS_msgsnd 0
#endif
#ifndef SYS_msgrcv
#define SYS_msgrcv 0
#endif
#ifndef SYS_msgctl
#define SYS_msgctl 0
#endif

/* POSIX message queues — optional kernel feature */
#ifndef SYS_mq_open
#define SYS_mq_open 0
#endif
#ifndef SYS_mq_unlink
#define SYS_mq_unlink 0
#endif
#ifndef SYS_mq_timedsend
#define SYS_mq_timedsend 0
#endif
/* Clocks and timers, 32-bit arm: musl there carries the time64 spellings
 * (clock_gettime64, timerfd_settime64, ...) and does not define the plain names at
 * all, because every one of those entry points is a 32-bit-time ABI that the libc
 * does not implement. The guards above cover the syscalls a 32-bit target simply
 * lacks; these cover the ones a 32-bit target renamed, which shows up as
 * "SYS_clock_gettime undeclared" rather than as a missing syscall at run time. */
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 0
#endif
#ifndef SYS_clock_settime
#define SYS_clock_settime 0
#endif
#ifndef SYS_clock_getres
#define SYS_clock_getres 0
#endif
#ifndef SYS_clock_nanosleep
#define SYS_clock_nanosleep 0
#endif
#ifndef SYS_gettimeofday
#define SYS_gettimeofday 0
#endif
#ifndef SYS_settimeofday
#define SYS_settimeofday 0
#endif
#ifndef SYS_timer_settime
#define SYS_timer_settime 0
#endif
#ifndef SYS_timer_gettime
#define SYS_timer_gettime 0
#endif
#ifndef SYS_timerfd_settime
#define SYS_timerfd_settime 0
#endif
#ifndef SYS_timerfd_gettime
#define SYS_timerfd_gettime 0
#endif

#ifndef SYS_mq_timedreceive
#define SYS_mq_timedreceive 0
#endif
#ifndef SYS_mq_notify
#define SYS_mq_notify 0
#endif
#ifndef SYS_mq_getsetattr
#define SYS_mq_getsetattr 0
#endif

/* ---- NR_xxx definitions (arch-correct, from <sys/syscall.h>) ---- */
/*
 * All syscall numbers are derived from the system header.
 * If a syscall falls back to 0 (absent on the target arch), the filter
 * construction loop silently skips it — no per-architecture tables needed.
 */

/* File I/O */
#define NR_read SYS_read
#define NR_write SYS_write
#define NR_open SYS_open
#define NR_openat SYS_openat
#define NR_openat2 SYS_openat2
#define NR_close SYS_close
#define NR_close_range SYS_close_range
#define NR_lseek SYS_lseek
#define NR_pread64 SYS_pread64
#define NR_pwrite64 SYS_pwrite64
#define NR_readv SYS_readv
#define NR_writev SYS_writev
#define NR_preadv SYS_preadv
#define NR_pwritev SYS_pwritev
#define NR_preadv2 SYS_preadv2
#define NR_pwritev2 SYS_pwritev2
#define NR_sendfile SYS_sendfile
#define NR_splice SYS_splice
#define NR_tee SYS_tee
#define NR_vmsplice SYS_vmsplice
#define NR_sync_file_range SYS_sync_file_range
#define NR_copy_file_range SYS_copy_file_range
#define NR_readahead SYS_readahead
#define NR_fadvise64 SYS_fadvise64

/* File stat / attributes */
#define NR_stat SYS_stat
#define NR_fstat SYS_fstat
#define NR_lstat SYS_lstat
#define NR_newfstatat SYS_newfstatat
#define NR_statx SYS_statx
#define NR_statfs SYS_statfs
#define NR_fstatfs SYS_fstatfs
#define NR_access SYS_access
#define NR_faccessat SYS_faccessat
#define NR_readlink SYS_readlink
#define NR_readlinkat SYS_readlinkat
#define NR_getdents SYS_getdents
#define NR_getdents64 SYS_getdents64
#define NR_getcwd SYS_getcwd
#define NR_utimensat SYS_utimensat
#define NR_futimesat SYS_futimesat
#define NR_utimes SYS_utimes
#define NR_utime SYS_utime

/* File metadata modification */
#define NR_chmod SYS_chmod
#define NR_fchmod SYS_fchmod
#define NR_fchmodat SYS_fchmodat
#define NR_fchmodat2 SYS_fchmodat2
#define NR_chown SYS_chown
#define NR_fchown SYS_fchown
#define NR_lchown SYS_lchown
#define NR_fchownat SYS_fchownat
#define NR_truncate SYS_truncate
#define NR_ftruncate SYS_ftruncate
#define NR_fallocate SYS_fallocate
#define NR_fsync SYS_fsync
#define NR_fdatasync SYS_fdatasync
#define NR_syncfs SYS_syncfs
#define NR_sync SYS_sync

/* Name / directory operations */
#define NR_creat SYS_creat
#define NR_mkdir SYS_mkdir
#define NR_mkdirat SYS_mkdirat
#define NR_rmdir SYS_rmdir
#define NR_rename SYS_rename
#define NR_renameat SYS_renameat
#define NR_renameat2 SYS_renameat2
#define NR_link SYS_link
#define NR_linkat SYS_linkat
#define NR_unlink SYS_unlink
#define NR_unlinkat SYS_unlinkat
#define NR_symlink SYS_symlink
#define NR_symlinkat SYS_symlinkat
#define NR_mknod SYS_mknod
#define NR_mknodat SYS_mknodat

/* Extended attributes */
#define NR_setxattr SYS_setxattr
#define NR_lsetxattr SYS_lsetxattr
#define NR_fsetxattr SYS_fsetxattr
#define NR_getxattr SYS_getxattr
#define NR_lgetxattr SYS_lgetxattr
#define NR_fgetxattr SYS_fgetxattr
#define NR_listxattr SYS_listxattr
#define NR_llistxattr SYS_llistxattr
#define NR_flistxattr SYS_flistxattr
#define NR_removexattr SYS_removexattr
#define NR_lremovexattr SYS_lremovexattr
#define NR_fremovexattr SYS_fremovexattr
/* Linux 6.13+ *at variants of the mutators (the read-only get/list ones above
 * stay permitted). */
#define NR_setxattrat SYS_setxattrat
#define NR_removexattrat SYS_removexattrat

/* Memory management */
#define NR_mmap SYS_mmap
#define NR_mprotect SYS_mprotect
#define NR_munmap SYS_munmap
#define NR_brk SYS_brk
#define NR_mremap SYS_mremap
#define NR_msync SYS_msync
#define NR_mincore SYS_mincore
#define NR_madvise SYS_madvise
#define NR_mlock SYS_mlock
#define NR_munlock SYS_munlock
#define NR_mlockall SYS_mlockall
#define NR_munlockall SYS_munlockall
#define NR_mlock2 SYS_mlock2
#define NR_membarrier SYS_membarrier
#define NR_migrate_pages SYS_migrate_pages
#define NR_move_pages SYS_move_pages
#define NR_pkey_mprotect SYS_pkey_mprotect
#define NR_pkey_alloc SYS_pkey_alloc
#define NR_pkey_free SYS_pkey_free
#define NR_userfaultfd SYS_userfaultfd
#define NR_remap_file_pages SYS_remap_file_pages

/* Process management */
#define NR_clone SYS_clone
#define NR_clone3 SYS_clone3
#define NR_fork SYS_fork
#define NR_vfork SYS_vfork
#define NR_execve SYS_execve
#define NR_execveat SYS_execveat
#define NR_exit SYS_exit
#define NR_exit_group SYS_exit_group
#define NR_wait4 SYS_wait4
#define NR_waitid SYS_waitid
#define NR_kill SYS_kill
#define NR_tkill SYS_tkill
#define NR_tgkill SYS_tgkill
#define NR_rt_sigaction SYS_rt_sigaction
#define NR_rt_sigprocmask SYS_rt_sigprocmask
#define NR_rt_sigreturn SYS_rt_sigreturn
#define NR_rt_sigpending SYS_rt_sigpending
#define NR_rt_sigtimedwait SYS_rt_sigtimedwait
#define NR_rt_sigqueueinfo SYS_rt_sigqueueinfo
#define NR_rt_sigsuspend SYS_rt_sigsuspend
#define NR_rt_tgsigqueueinfo SYS_rt_tgsigqueueinfo
#define NR_sigaltstack SYS_sigaltstack
#define NR_pause SYS_pause
#define NR_nanosleep SYS_nanosleep
#define NR_restart_syscall SYS_restart_syscall
#define NR_getpid SYS_getpid
#define NR_gettid SYS_gettid
#define NR_getppid SYS_getppid
#define NR_getpgid SYS_getpgid
#define NR_getpgrp SYS_getpgrp
#define NR_getsid SYS_getsid
#define NR_setsid SYS_setsid
#define NR_setpgid SYS_setpgid
#define NR_getuid SYS_getuid
#define NR_getgid SYS_getgid
#define NR_geteuid SYS_geteuid
#define NR_getegid SYS_getegid
#define NR_getgroups SYS_getgroups
#define NR_set_tid_address SYS_set_tid_address
#define NR_set_robust_list SYS_set_robust_list
#define NR_get_robust_list SYS_get_robust_list
#define NR_unshare SYS_unshare
#define NR_prctl SYS_prctl
#define NR_arch_prctl SYS_arch_prctl
#define NR_rseq SYS_rseq
#define NR_modify_ldt SYS_modify_ldt
#define NR_set_thread_area SYS_set_thread_area
#define NR_get_thread_area SYS_get_thread_area
#define NR_umask SYS_umask

/* Scheduler */
#define NR_sched_yield SYS_sched_yield
#define NR_sched_setaffinity SYS_sched_setaffinity
#define NR_sched_getaffinity SYS_sched_getaffinity
#define NR_sched_getparam SYS_sched_getparam
#define NR_sched_setparam SYS_sched_setparam
#define NR_sched_getscheduler SYS_sched_getscheduler
#define NR_sched_setscheduler SYS_sched_setscheduler
#define NR_sched_get_priority_max SYS_sched_get_priority_max
#define NR_sched_get_priority_min SYS_sched_get_priority_min
#define NR_sched_rr_get_interval SYS_sched_rr_get_interval
#define NR_sched_getattr SYS_sched_getattr
#define NR_sched_setattr SYS_sched_setattr
#define NR_getpriority SYS_getpriority
#define NR_setpriority SYS_setpriority

/* Timers & clocks */
#define NR_clock_gettime SYS_clock_gettime
#define NR_clock_settime SYS_clock_settime
#define NR_clock_getres SYS_clock_getres
#define NR_clock_nanosleep SYS_clock_nanosleep
#define NR_clock_adjtime SYS_clock_adjtime
#define NR_gettimeofday SYS_gettimeofday
#define NR_settimeofday SYS_settimeofday
#define NR_time SYS_time
#define NR_times SYS_times
#define NR_getitimer SYS_getitimer
#define NR_setitimer SYS_setitimer
#define NR_alarm SYS_alarm
#define NR_timer_create SYS_timer_create
#define NR_timer_settime SYS_timer_settime
#define NR_timer_gettime SYS_timer_gettime
#define NR_timer_getoverrun SYS_timer_getoverrun
#define NR_timer_delete SYS_timer_delete
#define NR_timerfd_create SYS_timerfd_create
#define NR_timerfd_settime SYS_timerfd_settime
#define NR_timerfd_gettime SYS_timerfd_gettime

/* Resources & limits */
#define NR_getrlimit SYS_getrlimit
#define NR_setrlimit SYS_setrlimit
#define NR_prlimit64 SYS_prlimit64
#define NR_getrusage SYS_getrusage
#define NR_sysinfo SYS_sysinfo
#define NR_uname SYS_uname

/* File descriptor operations */
#define NR_fcntl SYS_fcntl
#define NR_flock SYS_flock
#define NR_dup SYS_dup
#define NR_dup2 SYS_dup2
#define NR_dup3 SYS_dup3
#define NR_pipe SYS_pipe
#define NR_pipe2 SYS_pipe2
#define NR_ioctl SYS_ioctl
#define NR_select SYS_select
#define NR_poll SYS_poll
#define NR_ppoll SYS_ppoll
#define NR_pselect6 SYS_pselect6

/* epoll */
#define NR_epoll_create SYS_epoll_create
#define NR_epoll_create1 SYS_epoll_create1
#define NR_epoll_ctl SYS_epoll_ctl
#define NR_epoll_wait SYS_epoll_wait
#define NR_epoll_pwait SYS_epoll_pwait
#define NR_epoll_ctl_old SYS_epoll_ctl_old
#define NR_epoll_wait_old SYS_epoll_wait_old

/* eventfd / signalfd / inotify / fanotify */
#define NR_eventfd SYS_eventfd
#define NR_eventfd2 SYS_eventfd2
#define NR_signalfd SYS_signalfd
#define NR_signalfd4 SYS_signalfd4
#define NR_inotify_init SYS_inotify_init
#define NR_inotify_init1 SYS_inotify_init1
#define NR_inotify_add_watch SYS_inotify_add_watch
#define NR_inotify_rm_watch SYS_inotify_rm_watch
#define NR_fanotify_init SYS_fanotify_init
#define NR_fanotify_mark SYS_fanotify_mark

/* AIO */
#define NR_io_setup SYS_io_setup
#define NR_io_destroy SYS_io_destroy
#define NR_io_getevents SYS_io_getevents
#define NR_io_submit SYS_io_submit
#define NR_io_cancel SYS_io_cancel
#define NR_io_pgetevents SYS_io_pgetevents

/* Networking */
#define NR_socket SYS_socket
#define NR_connect SYS_connect
#define NR_accept SYS_accept
#define NR_accept4 SYS_accept4
#define NR_sendto SYS_sendto
#define NR_recvfrom SYS_recvfrom
#define NR_sendmsg SYS_sendmsg
#define NR_recvmsg SYS_recvmsg
#define NR_sendmmsg SYS_sendmmsg
#define NR_recvmmsg SYS_recvmmsg
#define NR_shutdown SYS_shutdown
#define NR_bind SYS_bind
#define NR_listen SYS_listen
#define NR_getsockname SYS_getsockname
#define NR_getpeername SYS_getpeername
#define NR_socketpair SYS_socketpair
#define NR_setsockopt SYS_setsockopt
#define NR_getsockopt SYS_getsockopt

/* SysV IPC */
#define NR_semget SYS_semget
#define NR_semop SYS_semop
#define NR_semctl SYS_semctl
#define NR_semtimedop SYS_semtimedop
#define NR_shmget SYS_shmget
#define NR_shmat SYS_shmat
#define NR_shmctl SYS_shmctl
#define NR_shmdt SYS_shmdt
#define NR_msgget SYS_msgget
#define NR_msgsnd SYS_msgsnd
#define NR_msgrcv SYS_msgrcv
#define NR_msgctl SYS_msgctl

/* POSIX message queues */
#define NR_mq_open SYS_mq_open
#define NR_mq_unlink SYS_mq_unlink
#define NR_mq_timedsend SYS_mq_timedsend
#define NR_mq_timedreceive SYS_mq_timedreceive
#define NR_mq_notify SYS_mq_notify
#define NR_mq_getsetattr SYS_mq_getsetattr

/* Miscellaneous kernel features */
#define NR_futex SYS_futex
#define NR_getrandom SYS_getrandom
#define NR_memfd_create SYS_memfd_create
#define NR_perf_event_open SYS_perf_event_open
#define NR_seccomp SYS_seccomp
#define NR_capget SYS_capget
#define NR_kcmp SYS_kcmp
#define NR_pidfd_getfd SYS_pidfd_getfd
#define NR_process_madvise SYS_process_madvise
#define NR_process_mrelease SYS_process_mrelease
#define NR_lookup_dcookie SYS_lookup_dcookie

/* Filesystem mount API (Linux 5.2+) */
#define NR_fsopen SYS_fsopen
#define NR_fsconfig SYS_fsconfig
#define NR_fsmount SYS_fsmount
#define NR_fspick SYS_fspick
#define NR_open_tree SYS_open_tree
#define NR_move_mount SYS_move_mount

/* pidfd (Linux 5.1+) */
#define NR_pidfd_open SYS_pidfd_open
#define NR_pidfd_send_signal SYS_pidfd_send_signal

/* ---- Deny list syscalls ---- */

#define NR_ptrace SYS_ptrace
#define NR_process_vm_readv SYS_process_vm_readv
#define NR_process_vm_writev SYS_process_vm_writev
#define NR_bpf SYS_bpf
#define NR_kexec_load SYS_kexec_load
#define NR_kexec_file_load SYS_kexec_file_load
#define NR_init_module SYS_init_module
#define NR_finit_module SYS_finit_module
#define NR_delete_module SYS_delete_module
#define NR_mount SYS_mount
#define NR_umount2 SYS_umount2
#define NR_pivot_root SYS_pivot_root
#define NR_chroot SYS_chroot
#define NR_setuid SYS_setuid
#define NR_setgid SYS_setgid
#define NR_setreuid SYS_setreuid
#define NR_setregid SYS_setregid
#define NR_setresuid SYS_setresuid
#define NR_setresgid SYS_setresgid
#define NR_setgroups SYS_setgroups
#define NR_setfsuid SYS_setfsuid
#define NR_setfsgid SYS_setfsgid
#define NR_capset SYS_capset
#define NR_personality SYS_personality
#define NR_setns SYS_setns
#define NR_open_by_handle_at SYS_open_by_handle_at
#define NR_name_to_handle_at SYS_name_to_handle_at
#define NR_io_uring_setup SYS_io_uring_setup
#define NR_io_uring_enter SYS_io_uring_enter
#define NR_io_uring_register SYS_io_uring_register
#define NR_add_key SYS_add_key
#define NR_request_key SYS_request_key
#define NR_keyctl SYS_keyctl
#define NR_iopl SYS_iopl
#define NR_ioperm SYS_ioperm
#define NR_reboot SYS_reboot
#define NR_create_module SYS_create_module
#define NR_query_module SYS_query_module
#define NR_get_kernel_syms SYS_get_kernel_syms
#define NR_nfsservctl SYS_nfsservctl
#define NR_getpmsg SYS_getpmsg
#define NR_putpmsg SYS_putpmsg
#define NR__sysctl SYS__sysctl
#define NR_vserver SYS_vserver

/* ---- Historical syscalls (safe list — allow if present) ---- */

#define NR_afs_syscall SYS_afs_syscall
#define NR_tuxcall SYS_tuxcall
#define NR_security SYS_security
#define NR_syslog SYS_syslog
#define NR_ustat SYS_ustat
#define NR_sysfs SYS_sysfs
#define NR_vhangup SYS_vhangup
#define NR_acct SYS_acct
#define NR_swapon SYS_swapon
#define NR_swapoff SYS_swapoff
#define NR_adjtimex SYS_adjtimex
#define NR_sethostname SYS_sethostname
#define NR_setdomainname SYS_setdomainname
#define NR_uselib SYS_uselib

/* ---- BPF filter construction ---- */

static int count_nonzero(const int *arr)
{
    int n = 0;
    for (int i = 0; arr[i] != -1; i++)
    {
        if (arr[i] != 0)
            n++;
    }
    return n;
}

#define SECCOMP_RET_ERRNO_EPERM (SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))

#ifndef BPF_JA
#define BPF_JA 0x00
#endif

#undef BPF_STMT
#undef BPF_JUMP
#define BPF_STMT(code, k) ((struct sock_filter){(unsigned short)(code), 0, 0, (unsigned int)(k)})
#define BPF_JUMP(code, k, jt, jf)                                                                                      \
    ((struct sock_filter){(unsigned short)(code), (unsigned char)(jt), (unsigned char)(jf), (unsigned int)(k)})

#define HDR_LEN 4
#define TAIL_LEN 0

static struct sock_filter *build_filter(size_t *out_len, bool block_metadata)
{
    static const int safe_syscalls[] = {
        NR_read,
        NR_write,
        NR_close,
        NR_stat,
        NR_fstat,
        NR_lstat,
        NR_poll,
        NR_lseek,
        NR_mmap,
        NR_mprotect,
        NR_munmap,
        NR_brk,
        NR_rt_sigaction,
        NR_rt_sigprocmask,
        NR_rt_sigreturn,
        NR_pread64,
        NR_pwrite64,
        NR_readv,
        NR_writev,
        NR_access,
        NR_select,
        NR_sched_yield,
        NR_mremap,
        NR_msync,
        NR_mincore,
        NR_madvise,
        NR_dup,
        NR_dup2,
        NR_dup3,
        NR_nanosleep,
        NR_getitimer,
        NR_setitimer,
        NR_getpid,
        NR_sendfile,
        NR_socket,
        NR_connect,
        NR_accept,
        NR_accept4,
        NR_sendto,
        NR_recvfrom,
        NR_sendmsg,
        NR_recvmsg,
        NR_shutdown,
        NR_bind,
        NR_listen,
        NR_getsockname,
        NR_getpeername,
        NR_socketpair,
        NR_setsockopt,
        NR_getsockopt,
        NR_clone,
        NR_fork,
        NR_vfork,
        NR_execve,
        NR_execveat,
        NR_exit,
        NR_wait4,
        NR_kill,
        NR_uname,
        NR_getuid,
        NR_getgid,
        NR_geteuid,
        NR_getegid,
        NR_getpgid,
        NR_getppid,
        NR_getgroups,
        NR_fcntl,
        NR_flock,
        NR_fsync,
        NR_fdatasync,
        NR_ftruncate,
        NR_getdents,
        NR_getdents64,
        NR_getcwd,
        NR_gettimeofday,
        NR_getrlimit,
        NR_getrusage,
        NR_sysinfo,
        NR_times,
        NR_readlink,
        NR_readlinkat,
        NR_umask,
        NR_futex,
        NR_restart_syscall,
        NR_sched_setaffinity,
        NR_sched_getaffinity,
        NR_exit_group,
        NR_tgkill,
        NR_set_tid_address,
        NR_clock_gettime,
        NR_clock_getres,
        NR_clock_nanosleep,
        NR_set_robust_list,
        NR_get_robust_list,
        NR_epoll_create,
        NR_epoll_create1,
        NR_epoll_ctl,
        NR_epoll_wait,
        NR_epoll_pwait,
        NR_pipe2,
        NR_prctl,
        NR_arch_prctl,
        NR_getrandom,
        NR_memfd_create,
        NR_rseq,
        NR_statx,
        NR_statfs,
        NR_fstatfs,
        NR_prlimit64,
        NR_membarrier,
        NR_mlock,
        NR_munlock,
        NR_mlockall,
        NR_munlockall,
        NR_sigaltstack,
        NR_rt_sigpending,
        NR_rt_sigtimedwait,
        NR_rt_sigsuspend,
        NR_gettid,
        NR_time,
        NR_close_range,
        NR_eventfd,
        NR_eventfd2,
        NR_signalfd,
        NR_signalfd4,
        NR_timerfd_create,
        NR_timerfd_settime,
        NR_timerfd_gettime,
        NR_inotify_init,
        NR_inotify_init1,
        NR_inotify_add_watch,
        NR_inotify_rm_watch,
        NR_faccessat,
        NR_sched_getparam,
        NR_sched_setparam,
        NR_sched_getscheduler,
        NR_sched_setscheduler,
        NR_sched_get_priority_max,
        NR_sched_get_priority_min,
        NR_sched_rr_get_interval,
        NR_unshare,
        NR_setpgid,
        NR_setsid,
        NR_getsid,
        NR_sched_getattr,
        NR_sched_setattr,
        NR_newfstatat,
        NR_pselect6,
        NR_ppoll,
        NR_clock_adjtime,
        NR_syncfs,
        NR_getpriority,
        NR_setpriority,
        NR_getxattr,
        NR_fgetxattr,
        NR_lgetxattr,
        NR_listxattr,
        NR_flistxattr,
        NR_llistxattr,
        NR_mq_open,
        NR_mq_unlink,
        NR_mq_timedsend,
        NR_mq_timedreceive,
        NR_recvmmsg,
        NR_sendmmsg,
        NR_splice,
        NR_tee,
        NR_vmsplice,
        NR_copy_file_range,
        NR_sync_file_range,
        NR_fadvise64,
        NR_fallocate,
        NR_readahead,
        NR_timer_create,
        NR_timer_settime,
        NR_timer_gettime,
        NR_timer_getoverrun,
        NR_timer_delete,
        NR_io_setup,
        NR_io_destroy,
        NR_io_getevents,
        NR_io_submit,
        NR_io_cancel,
        NR_io_pgetevents,
        NR_migrate_pages,
        NR_move_pages,
        NR_seccomp,
        NR_waitid,
        NR_fanotify_init,
        NR_fanotify_mark,
        NR_afs_syscall,
        NR_tuxcall,
        NR_security,
        NR_syslog,
        NR_ustat,
        NR_sysfs,
        NR_vhangup,
        NR_acct,
        NR_swapon,
        NR_swapoff,
        NR_adjtimex,
        NR_sethostname,
        NR_setdomainname,
        NR_getpgrp,
        NR_pause,
        NR_alarm,
        NR_semget,
        NR_semop,
        NR_semctl,
        NR_semtimedop,
        NR_shmget,
        NR_shmat,
        NR_shmctl,
        NR_shmdt,
        NR_msgget,
        NR_msgsnd,
        NR_msgrcv,
        NR_msgctl,
        NR_lookup_dcookie,
        NR_perf_event_open,
        NR_userfaultfd,
        NR_preadv,
        NR_pwritev,
        NR_preadv2,
        NR_pwritev2,
        NR_mlock2,
        NR_pkey_mprotect,
        NR_pkey_alloc,
        NR_pkey_free,
        NR_pidfd_open,
        NR_pidfd_send_signal,
        NR_clone3,
        NR_fsopen,
        NR_fsconfig,
        NR_fsmount,
        NR_fspick,
        NR_modify_ldt,
        NR_set_thread_area,
        NR_get_thread_area,
        NR_rt_tgsigqueueinfo,
        NR_rt_sigqueueinfo,
        NR_tkill,
        NR_mq_notify,
        NR_mq_getsetattr,
        NR_openat,
        NR_openat2,
        NR_mkdirat,
        NR_mknodat,
        NR_symlinkat,
        NR_linkat,
        NR_unlinkat,
        NR_renameat,
        NR_renameat2,
        NR_creat,
        NR_mkdir,
        NR_mknod,
        NR_symlink,
        NR_link,
        NR_unlink,
        NR_rmdir,
        NR_rename,
        NR_truncate,
        -1,
    };

    static const int deny_syscalls[] = {
        NR_ptrace,
        NR_process_vm_readv,
        NR_process_vm_writev,
        NR_process_madvise,
        NR_process_mrelease,
        NR_pidfd_getfd,
        NR_kcmp,
        NR_bpf,
        NR_kexec_load,
        NR_kexec_file_load,
        NR_init_module,
        NR_finit_module,
        NR_delete_module,
        NR_mount,
        NR_umount2,
        NR_pivot_root,
        NR_chroot,
        NR_setuid,
        NR_setgid,
        NR_setreuid,
        NR_setregid,
        NR_setresuid,
        NR_setresgid,
        NR_setgroups,
        NR_setfsuid,
        NR_setfsgid,
        NR_capset,
        NR_personality,
        NR_setns,
        NR_open_by_handle_at,
        NR_name_to_handle_at,
        NR_io_uring_setup,
        NR_io_uring_enter,
        NR_io_uring_register,
        NR_add_key,
        NR_request_key,
        NR_keyctl,
        NR_iopl,
        NR_ioperm,
        NR_reboot,
        NR_create_module,
        NR_query_module,
        NR_get_kernel_syms,
        NR_nfsservctl,
        NR_getpmsg,
        NR_putpmsg,
        NR__sysctl,
        NR_vserver,
        NR_open_tree,
        NR_move_mount,
        -1,
    };

    /* Metadata mutators that Landlock does not mediate, so they need a seccomp
     * backstop -- but only when the mount-namespace sandbox is NOT active: it makes
     * the read-only paths immutable (EROFS) while leaving writable paths alone,
     * whereas these rules are path-blind and would also break chmod/xattr on
     * writable paths. */
    static const int deny_metadata_syscalls[] = {
        NR_chmod,     NR_fchmod,      NR_fchmodat,     NR_fchmodat2,    NR_chown,      NR_fchown,        NR_fchownat,
        NR_lchown,    NR_utimensat,   NR_utimes,       NR_futimesat,    NR_utime,      NR_setxattr,      NR_fsetxattr,
        NR_lsetxattr, NR_removexattr, NR_fremovexattr, NR_lremovexattr, NR_setxattrat, NR_removexattrat, -1,
    };

    int safe_n = count_nonzero(safe_syscalls);
    int deny_n = count_nonzero(deny_syscalls) + (block_metadata ? count_nonzero(deny_metadata_syscalls) : 0);

    /*
     * Deny rules that must also match a syscall argument. seccomp can read the
     * argument registers, so a single ioctl() request can be forbidden without
     * banning ioctl() itself (terminals, sockets, ... all use it). Each entry
     * is {syscall, arg index, value}; the ioctl request is argument 1.
     * FS_IOC_SETFLAGS is the chattr-style inode-flag setter, which Landlock
     * does not mediate. Terminated by nr == -1. */
    static const struct
    {
        int nr;
        int arg;
        unsigned int val;
    } deny_arg_syscalls[] = {
        {NR_ioctl, 1, EDC_FS_IOC_SETFLAGS},
        {-1, 0, 0},
    };

    int arg_n = 0;
    if (block_metadata)
    {
        for (int a = 0; deny_arg_syscalls[a].nr != -1; a++)
            if (deny_arg_syscalls[a].nr != 0)
                arg_n++;
    }

    /*
     * Layout:
     *   [arch check][load nr]
     *   safe block: chunks of <= SAFE_CHUNK JEQ entries. Each chunk's matches
     *       jump forward to an inline ALLOW stub at the end of the chunk, while
     *       the fall-through runs an unconditional JA placed *before* that stub
     *       to skip over it and continue with the next chunk (or the deny
     *       block). The JA is essential: without it the fall-through lands on
     *       the stub and returns ALLOW for every syscall not in the safe list,
     *       which made the deny block unreachable.
     *   deny block: one JEQ per denied syscall -> jump to DENY_TARGET.
     *   default: ALLOW (fail-open for unknown syscalls).
     *   DENY_TARGET: return EPERM.
     * JEQ jt/jf are u8, so every jump must stay within 255 instructions of its
     * source; the chunk size keeps safe jumps <= SAFE_CHUNK and deny_n stays
     * small (< 255).
     */
#define SAFE_CHUNK 200
    int nchains = safe_n > 0 ? (safe_n + SAFE_CHUNK - 1) / SAFE_CHUNK : 0;

    /* header (4) + safe entries + 2 (JA + ALLOW stub) per chunk + deny entries
     * + arg-nr checks + 4 per arg rule block + default ALLOW + DENY_TARGET. */
    size_t cap =
        HDR_LEN + (size_t)safe_n + 2 * (size_t)nchains + (size_t)deny_n + (size_t)arg_n + 4 * (size_t)arg_n + 2 + 8;
    struct sock_filter *f = calloc(cap, sizeof(struct sock_filter));
    if (!f)
        return NULL;
    size_t pos = 0;

    /* arch check */
    f[pos++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4);
    f[pos++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH_NR, 1, 0);
    f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL);

    /* load syscall number */
    f[pos++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0);

    /* Emit the safe block, chunk by chunk. */
    int emitted = 0;
    int i = 0;
    while (emitted < safe_n)
    {
        int count = (safe_n - emitted) > SAFE_CHUNK ? SAFE_CHUNK : (safe_n - emitted);
        int stub_pos = (int)pos + count + 1; /* after the count JEQs + the JA */
        int j = 0;
        for (; safe_syscalls[i] != -1 && j < count; i++)
        {
            if (safe_syscalls[i] == 0)
                continue;
            int jt = stub_pos - (int)pos - 1;
            f[pos++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)safe_syscalls[i], (unsigned char)jt, 0);
            j++;
            emitted++;
        }
        /* Fall-through: skip the ALLOW stub and continue with the next block.
         * Compute the offset first: using `pos` in the RHS of `f[pos++] = ...`
         * is unsequenced (UB) and the compiler may read the post-increment value. */
        int next_pos = stub_pos + 1;
        int ja = next_pos - (int)pos - 1;
        f[pos++] = BPF_JUMP(BPF_JMP | BPF_JA, (unsigned int)ja, 0, 0);
        f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW); /* stub */
    }

    /*
     * Tail layout (every jump is forward -- classic BPF jumps cannot go back):
     *   [deny_n plain checks]     -> DENY_TARGET
     *   [arg_n nr checks]         -> that rule's block below
     *   [default ALLOW]
     *   [4 * arg_n rule blocks]   self-contained: LD args[i]; JEQ value (skip
     *                             next); RET ALLOW; RET ERRNO
     *   [DENY_TARGET]
     */
    int deny_target_pos = (int)pos + deny_n + arg_n + 4 * arg_n + 1;
    int default_allow_pos = (int)pos + deny_n + arg_n;
    int blocks_base = default_allow_pos + 1;

    /* Plain deny checks -> DENY_TARGET. */
    for (int d = 0; deny_syscalls[d] != -1; d++)
    {
        if (deny_syscalls[d] == 0)
            continue;
        int jt = deny_target_pos - (int)pos - 1;
        f[pos++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)deny_syscalls[d], (unsigned char)jt, 0);
    }

    /* Metadata deny checks (fallback only) -> DENY_TARGET. */
    if (block_metadata)
    {
        for (int d = 0; deny_metadata_syscalls[d] != -1; d++)
        {
            if (deny_metadata_syscalls[d] == 0)
                continue;
            int jt = deny_target_pos - (int)pos - 1;
            f[pos++] =
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)deny_metadata_syscalls[d], (unsigned char)jt, 0);
        }
    }

    /* Arg-aware deny checks -> the matching rule block. */
    {
        int r = 0;
        for (int a = 0; r < arg_n && deny_arg_syscalls[a].nr != -1; a++)
        {
            if (deny_arg_syscalls[a].nr == 0)
                continue;
            int block = blocks_base + 4 * r;
            int jt = block - (int)pos - 1;
            f[pos++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)deny_arg_syscalls[a].nr, (unsigned char)jt, 0);
            r++;
        }
    }

    /* default: unknown syscall -> ALLOW (fail-open) */
    f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* One self-contained block per arg rule. args[] start at offset 16 in
     * struct seccomp_data (8 bytes each); the low 32 bits hold the request. */
    {
        int r = 0;
        for (int a = 0; r < arg_n && deny_arg_syscalls[a].nr != -1; a++)
        {
            if (deny_arg_syscalls[a].nr == 0)
                continue;
            f[pos++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 16 + 8 * (unsigned int)deny_arg_syscalls[a].arg);
            f[pos++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, deny_arg_syscalls[a].val, 1, 0);
            f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
            f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO_EPERM);
            r++;
        }
    }

    /* DENY_TARGET */
    f[pos++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO_EPERM);

    *out_len = pos;
    return f;
}

bool edc_bpf_sandbox_apply_ex(const char *sandbox_root, bool block_metadata)
{
    size_t filter_len = 0;
    struct sock_filter *filter = build_filter(&filter_len, block_metadata);
    if (!filter)
    {
        return false;
    }

    struct sock_fprog prog = {
        .len = (unsigned short)filter_len,
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
    {
        log_error("edc_bpf_sandbox: seccomp filter install failed: %s", strerror(errno));
        free(filter);
        return false;
    }

    free(filter);
    log_info("edc_bpf_sandbox: seccomp-bpf sandbox enforced, root='%s', metadata=%s", sandbox_root,
             block_metadata ? "blocked" : "advisory(ns)");
    return true;
}

bool edc_bpf_sandbox_apply(const char *sandbox_root)
{
    return edc_bpf_sandbox_apply_ex(sandbox_root, true);
}

#else /* !defined(ARCH_NR) */

bool edc_bpf_sandbox_apply_ex(const char *sandbox_root, bool block_metadata)
{
    (void)sandbox_root;
    (void)block_metadata;
    log_debug("edc_bpf_sandbox: unsupported architecture, BPF sandbox disabled");
    return false;
}

bool edc_bpf_sandbox_apply(const char *sandbox_root)
{
    return edc_bpf_sandbox_apply_ex(sandbox_root, true);
}

#endif /* ARCH_NR */

#else /* !__linux__ */

bool edc_bpf_sandbox_apply_ex(const char *sandbox_root, bool block_metadata)
{
    (void)sandbox_root;
    (void)block_metadata;
    return false;
}

bool edc_bpf_sandbox_apply(const char *sandbox_root)
{
    return edc_bpf_sandbox_apply_ex(sandbox_root, true);
}

#endif /* __linux__ */
