/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation:
 *   - UNIX domain socket control-plane IPC (supervisor <-> CLI)
 *   - clone() with PID, UTS, and mount namespace isolation
 *   - chroot into per-container rootfs with /proc mounted
 *   - pipe-based stdout/stderr capture into supervisor
 *   - bounded-buffer producer/consumer logging pipeline (mutex + condvar)
 *   - SIGCHLD reaping (no zombies), SIGINT/SIGTERM orderly shutdown
 *   - kernel module registration via ioctl
 *   - CMD_START (background), CMD_RUN (blocking), CMD_PS, CMD_LOGS, CMD_STOP
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT   (64UL << 20)   /* 64 MiB */
#define MAX_CONTAINERS       64

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;   /* set by CMD_STOP before SIGTERM */
    char              log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t     items[LOG_BUFFER_CAPACITY];
    size_t         head;
    size_t         tail;
    size_t         count;
    int            shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;  /* write-end of the pipe the supervisor reads from */
} child_config_t;

/* Producer thread arg: one per container pipe */
typedef struct {
    int             read_fd;
    char            container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

typedef struct {
    int                server_fd;
    int                monitor_fd;
    volatile int       should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/* Global supervisor context (accessed by signal handlers)            */
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                    nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                      */
/* ------------------------------------------------------------------ */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Push a log_item into the buffer.
 * Blocks if full; returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Pop a log_item from the buffer.
 * Returns 1 if item was retrieved, 0 if shutting down and buffer empty.
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {
        /* Shutting down and nothing left */
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Logging Consumer Thread                                             */
/* ------------------------------------------------------------------ */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (1) {
        int got = bounded_buffer_pop(buf, &item);
        if (!got)
            break;  /* shutdown + empty */

        /* Open log file for this container (append) */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open");
            continue;
        }
        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) { perror("logging_thread: write"); break; }
            written += (size_t)n;
        }
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer Thread: reads from container pipe, pushes to buffer       */
/* ------------------------------------------------------------------ */
static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(pa->read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        bounded_buffer_push(pa->buffer, &item);
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container Child Entrypoint (runs after clone())                    */
/* ------------------------------------------------------------------ */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the write-end of the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) { perror("dup2 stdout"); return 1; }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) { perror("dup2 stderr"); return 1; }
    close(cfg->log_write_fd);

    /* Set hostname to container id (UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("sethostname");  /* non-fatal */

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* chroot into container rootfs */
    if (chdir(cfg->rootfs) != 0) { perror("chdir"); return 1; }
    if (chroot(".") != 0)        { perror("chroot"); return 1; }
    if (chdir("/") != 0)         { perror("chdir /"); return 1; }

    /* Mount /proc so ps, top, etc. work inside */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc");  /* non-fatal if already mounted */

    /* Execute the requested command */
    char *argv_exec[] = { cfg->command, NULL };
    char *envp[]      = { "PATH=/bin:/usr/bin:/sbin:/usr/sbin", "HOME=/root", NULL };
    execve(cfg->command, argv_exec, envp);

    /* execve only returns on error */
    perror("execve");
    return 127;
}

/* ------------------------------------------------------------------ */
/* Kernel Monitor Registration                                         */
/* ------------------------------------------------------------------ */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container Record Helpers (call with metadata_lock held)            */
/* ------------------------------------------------------------------ */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strcmp(r->id, id) == 0) return r;
        r = r->next;
    }
    return NULL;
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}

/* ------------------------------------------------------------------ */
/* Spawn a new container                                               */
/* ------------------------------------------------------------------ */
static container_record_t *spawn_container(supervisor_ctx_t *ctx,
                                            const control_request_t *req)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return NULL; }

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { close(pipefd[0]); close(pipefd[1]); return NULL; }

    /* Build child config on heap (freed by child after exec is irrelevant;
       but we pass via pointer so keep alive until clone returns) */
    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) { free(stack); close(pipefd[0]); close(pipefd[1]); return NULL; }

    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value  = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Clone with new PID, UTS, and mount namespaces */
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);

    free(stack);

    if (pid < 0) {
        perror("clone");
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    /* Parent closes write-end; child has its own copy */
    close(pipefd[1]);
    free(cfg);  /* child_fn has already been called by the kernel */

    /* Create container record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    if (!rec) { close(pipefd[0]); return NULL; }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes);

    /* Spin up producer thread to drain the pipe into the log buffer */
    producer_arg_t *pa = malloc(sizeof(producer_arg_t));
    if (pa) {
        pa->read_fd = pipefd[0];
        strncpy(pa->container_id, rec->id, CONTAINER_ID_LEN - 1);
        pa->buffer = &ctx->log_buffer;
        pthread_t pt;
        if (pthread_create(&pt, NULL, producer_thread, pa) != 0) {
            free(pa);
            close(pipefd[0]);
        } else {
            pthread_detach(pt);
        }
    } else {
        close(pipefd[0]);
    }

    return rec;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler: reap zombies, update metadata                     */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *r = g_ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status)) {
                    r->exit_code = WEXITSTATUS(status);
                    r->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    r->exit_signal = WTERMSIG(status);
                    /* Distinguish manual stop vs hard-limit kill */
                    if (r->stop_requested)
                        r->state = CONTAINER_STOPPED;
                    else
                        r->state = CONTAINER_KILLED;
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, r->id, pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* SIGINT / SIGTERM handler: signal supervisor event loop to exit     */
/* ------------------------------------------------------------------ */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Handle one control request from a CLI client                       */
/* ------------------------------------------------------------------ */
static void handle_request(supervisor_ctx_t *ctx,
                            int client_fd,
                            const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {

    case CMD_START: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *existing = find_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (existing && existing->state == CONTAINER_RUNNING) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' is already running (pid %d)",
                     req->container_id, existing->host_pid);
            break;
        }

        container_record_t *rec = spawn_container(ctx, req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container '%s'", req->container_id);
            break;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        add_container(ctx, rec);
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Started container '%s' pid=%d", rec->id, rec->host_pid);
        break;
    }

    case CMD_RUN: {
        /* Same as START but we block until the container exits */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *existing = find_container(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (existing && existing->state == CONTAINER_RUNNING) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' is already running", req->container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        container_record_t *rec = spawn_container(ctx, req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container '%s'", req->container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        add_container(ctx, rec);
        pid_t run_pid = rec->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Wait for this specific container to finish */
        int wstatus = 0;
        while (waitpid(run_pid, &wstatus, 0) < 0 && errno == EINTR)
            ;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req->container_id);
        if (r) {
            if (WIFEXITED(wstatus)) {
                r->exit_code = WEXITSTATUS(wstatus);
                r->state     = CONTAINER_EXITED;
                resp.status  = r->exit_code;
            } else if (WIFSIGNALED(wstatus)) {
                r->exit_signal = WTERMSIG(wstatus);
                r->state = r->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
                resp.status = 128 + r->exit_signal;
            }
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd, r->id, run_pid);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' finished exit_status=%d",
                 req->container_id, resp.status);
        break;
    }

    case CMD_PS: {
        resp.status = 0;
        char *p   = resp.message;
        int  left = (int)sizeof(resp.message) - 1;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        int wrote = snprintf(p, (size_t)left,
                             "%-12s %-8s %-10s %-26s\n",
                             "ID", "PID", "STATE", "STARTED");
        if (wrote > 0 && wrote < left) { p += wrote; left -= wrote; }

        while (r && left > 0) {
            char tbuf[32];
            struct tm *tm = localtime(&r->started_at);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
            wrote = snprintf(p, (size_t)left,
                             "%-12s %-8d %-10s %-26s\n",
                             r->id, r->host_pid,
                             state_to_string(r->state), tbuf);
            if (wrote > 0 && wrote < left) { p += wrote; left -= wrote; }
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }

    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req->container_id);
        char log_path[PATH_MAX] = {0};
        if (r) strncpy(log_path, r->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!r) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No container '%s'", req->container_id);
            break;
        }

        int lfd = open(log_path, O_RDONLY);
        if (lfd < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Cannot open log: %s", log_path);
            break;
        }
        /* Stream log file in chunks directly to client */
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Log for '%s':\n", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);

        char chunk[4096];
        ssize_t n;
        while ((n = read(lfd, chunk, sizeof(chunk))) > 0)
            send(client_fd, chunk, (size_t)n, 0);
        close(lfd);
        return;   /* already sent response */
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req->container_id);
        pid_t target_pid = r ? r->host_pid : -1;
        if (r) r->stop_requested = 1;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (target_pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No container '%s'", req->container_id);
            break;
        }

        /* Graceful: SIGTERM first, then SIGKILL if still alive */
        kill(target_pid, SIGTERM);
        struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        if (kill(target_pid, 0) == 0)   /* still alive */
            kill(target_pid, SIGKILL);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Stop sent to '%s' (pid %d)", req->container_id, target_pid);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
}

/* ------------------------------------------------------------------ */
/* Supervisor Main Loop                                                */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    (void)rootfs;  /* rootfs-base path noted but containers use own copies */

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Metadata lock */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    /* Bounded buffer */
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Log directory */
    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor device (optional — continue if absent) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] /dev/container_monitor not available; memory monitoring disabled\n");

    /* Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) != 0) {
        perror("listen");
        goto cleanup;
    }
    chmod(CONTROL_PATH, 0666);

    /* Signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* Start logging consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stdout, "[supervisor] Ready. Listening on %s\n", CONTROL_PATH);
    fflush(stdout);

    /* Event loop */
    while (!ctx.should_stop) {
        /* Non-blocking accept with short timeout via select */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;  /* timeout — check should_stop */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        control_request_t req;
        ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        if (n == (ssize_t)sizeof(req))
            handle_request(&ctx, client_fd, &req);
        close(client_fd);
    }

    fprintf(stdout, "[supervisor] Shutting down...\n");
    fflush(stdout);

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Short wait for containers to exit */
    struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
    nanosleep(&ts, NULL);

    /* Force-kill stragglers */
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING)
            kill(r->host_pid, SIGKILL);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

cleanup:
    /* Signal log buffer shutdown and join logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur);
        cur = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    if (ctx.server_fd  >= 0) close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stdout, "[supervisor] Clean exit.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI Client: connect to supervisor and send/receive                 */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect — is the supervisor running?");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    /* For CMD_LOGS the supervisor streams raw data after the initial response */
    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n <= 0) { perror("recv"); close(fd); return 1; }

    printf("%s\n", resp.message);

    /* If streaming log data follows (for CMD_LOGS) */
    if (req->kind == CMD_LOGS && resp.status == 0) {
        char buf[4096];
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)n, stdout);
    }

    close(fd);
    return (resp.status == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI Command Handlers                                                */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
