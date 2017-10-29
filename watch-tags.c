#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ram-err.h>
#include <sys/inotify.h>
#include <pthread.h>

typedef struct {
    char *path;
    int fd;
} watch_path_s;

typedef struct {
    watch_path_s *watch_paths;
    unsigned int n_used;
} watch_paths_s;

typedef enum {
    ProcState_None,
    ProcState_Processing,
    ProcState_Enqueueing,
} proc_state_s;

typedef struct {
    watch_paths_s queue;
    proc_state_s proc_state;
    unsigned int wait_time;
} queue_state_s;

void add_watch_path(watch_paths_s *dps, watch_path_s *wp) {
    for (unsigned int dp_idx = 0; dp_idx < dps->n_used; dp_idx++) {
        watch_path_s *dp = dps->watch_paths + dp_idx;
        if (dp->fd == wp->fd) {
            return;
        }
    }
    watch_path_s *dp = dps->watch_paths + dps->n_used++;
    *dp = *wp;
}

bool enqueue_watch_paths(queue_state_s *qs, watch_paths_s *dps) {
    if (__sync_bool_compare_and_swap(&qs->proc_state, ProcState_None, ProcState_Enqueueing)) {
        for (unsigned int i = 0; i < dps->n_used; i++) {
            watch_path_s *wp = dps->watch_paths + i;
            add_watch_path(&qs->queue, wp);
        }
        qs->proc_state = ProcState_None;
        return true;
    } else {
        return false;
    }
}

void path_changed(char *path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd)-1, "ctagspath %s", path);
    FILE *pipe = popen(cmd, "r");
    pclose(pipe);
}

void *process_queue(void *pthread_data) {
    queue_state_s *qs = pthread_data;
    watch_paths_s *queue = &qs->queue;
    for (;;) {
        while (!__sync_bool_compare_and_swap(&qs->proc_state, ProcState_None, ProcState_Processing)) {
            perr_die_if(pthread_yield() != 0,  "pthread_yield");
        }

        for (unsigned int i = 0; i < queue->n_used; i++) {
            watch_path_s *wp = queue->watch_paths + i;

            path_changed(wp->path);
        }
        queue->n_used = 0;
        qs->proc_state = ProcState_None;

        sleep(qs->wait_time);
    }
    return NULL;
}

unsigned int parse_wait_time(char *s, unsigned int default_wait_time) {
    unsigned int result = default_wait_time;
    if (s) {
        char *endptr = s;
        unsigned int parsed_val = strtol(s, &endptr, 10);
        if (endptr != s) {
            result = parsed_val;
        }
    }
    return result;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        die_usage(argv[0], "<watch-path>");
    }

    unsigned int n_watch_paths = argc - 1;
    unsigned int mem_size = n_watch_paths * sizeof(watch_path_s) * 3;
    void *memory = malloc(mem_size);
    memset(memory, 0, mem_size);

    watch_path_s *watch_paths = memory;
    watch_path_s *dirty_watch_paths = watch_paths + n_watch_paths;
    watch_path_s *queue_watch_paths = dirty_watch_paths + n_watch_paths;

    int inotify_fd = inotify_init();
    perr_die_if(inotify_fd < 0, "inotify_init");
    unsigned int watch_path_idx = 0;
    for (unsigned int arg_idx = 1; arg_idx < argc; arg_idx++) {
        char *watch_path = argv[arg_idx];
        int watch_fd = inotify_add_watch(
            inotify_fd,
            watch_path,
            IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE
        );
        perr_die_if(watch_fd < 0, "inotify_add_watch");
        watch_path_s *watched_path = watch_paths + watch_path_idx++;
        watched_path->fd = watch_fd;
        watched_path->path = watch_path;
    }

    watch_paths_s dirty_paths = {0};
    dirty_paths.watch_paths = dirty_watch_paths;

    queue_state_s queue_state = {0};
    queue_state.queue.watch_paths = queue_watch_paths;
    char *wait_time_str = getenv("WATCHTAGS_WAIT_TIME");
    queue_state.wait_time = parse_wait_time(wait_time_str, 60);

    pthread_t thread;
    perr_die_if(pthread_create(&thread, NULL, process_queue, &queue_state) != 0, "pthread_create");

    char inotify_buf[4096];

    for (;;) {
        ssize_t n = read(inotify_fd, inotify_buf, sizeof(inotify_buf));
        perr_die_if(n < 0, "read");
        for (char *p = inotify_buf; p < inotify_buf + n; p += sizeof(struct inotify_event)) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (strcmp(e->name, "tags") == 0) {
                // it's important to ignore tags modifications
                // otherwise we'll get stuck in a loop!
                continue;
            }
            for (unsigned int wp_idx = 0; wp_idx < n_watch_paths; wp_idx++) {
                watch_path_s *watched_path = watch_paths + wp_idx;
                if (e->wd == watched_path->fd) {
                    add_watch_path(&dirty_paths, watched_path);
                    break;
                }
            }
        }
        while (!enqueue_watch_paths(&queue_state, &dirty_paths)) {
            perr_die_if(pthread_yield() != 0,  "pthread_yield");
        }
        dirty_paths.n_used = 0;
    }

    return 0;
}
