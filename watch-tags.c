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
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>

typedef struct watch_path_s {
    char *path;
    int fd;
    struct watch_path_s *base;
} watch_path_s;

typedef struct watch_link_s {
    watch_path_s *wp;
    struct watch_link_s *next;
} watch_link_s;

typedef enum {
    ProcState_None,
    ProcState_Processing,
    ProcState_Enqueueing,
} proc_state_s;

typedef struct {
    watch_link_s *first_queue;
    proc_state_s proc_state;
    unsigned int wait_time;
    char *cmd;
} queue_state_s;

typedef struct {
    watch_link_s *first_watched;
    watch_link_s *first_dirty;
    watch_link_s *first_queue;
    watch_link_s *first_free;

    queue_state_s queue_state;

} watch_state_s;

watch_path_s *alloc_watch_path(watch_state_s *state) {
    watch_path_s *result = malloc(sizeof(watch_state_s));
    result->fd = 0;
    result->path = 0;
    result->base = 0;
    return result;
}

watch_link_s *alloc_link(watch_state_s *state) {
    watch_link_s *result;
    if (state->first_free) {
        result = state->first_free;
        state->first_free = state->first_free->next;
    } else {
        result = malloc(sizeof(watch_link_s));
    }
    result->next = 0;
    result->wp = 0;
    return result;
}

void free_link(watch_state_s *state, watch_link_s *link) {
    link->next = state->first_free;
    state->first_free = link;
}

watch_link_s *add_watch_path_if_needed(watch_state_s *state, watch_link_s *dest_list, watch_path_s *wp) {
    // first check if in list already
    for (watch_link_s *l = dest_list; l; l = l->next) {
        if (l->wp->fd == wp->fd) {
            return dest_list;
        }
    }
    watch_link_s *new_link = alloc_link(state);
    new_link->wp = wp;
    new_link->next = dest_list;
    return new_link;
}

bool enqueue_watch_paths(watch_state_s *state, queue_state_s *qs, watch_link_s *first_dirty) {
    if (__sync_bool_compare_and_swap(&qs->proc_state, ProcState_None, ProcState_Enqueueing)) {
        for (watch_link_s *l = first_dirty; l; l = l->next) {
            qs->first_queue = add_watch_path_if_needed(state, qs->first_queue, l->wp);
        }
        qs->proc_state = ProcState_None;
        return true;
    } else {
        return false;
    }
}

void path_changed(queue_state_s *qs, char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd)-1, "%s %s", qs->cmd, path);
    puts(cmd);
    FILE *pipe = popen(cmd, "r");
    pclose(pipe);
}

void free_links(watch_state_s *state, watch_link_s *first_link) {
    watch_link_s *l = first_link;
    while (l) {
        watch_link_s *next = l->next;
        free_link(state, l);
        l = next;
    }
}

void *process_queue(void *pthread_data) {
    watch_state_s *state = pthread_data;
    queue_state_s *qs = &state->queue_state;
    for (;;) {
        while (!__sync_bool_compare_and_swap(&qs->proc_state, ProcState_None, ProcState_Processing)) {
            perr_die_if(pthread_yield() != 0,  "pthread_yield");
        }

        for (watch_link_s *l = qs->first_queue; l; l = l->next) {
            path_changed(qs, l->wp->path);
        }
        free_links(state, qs->first_queue);

        qs->first_queue = NULL;
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

watch_path_s *add_inotify_path(watch_state_s *state, int inotify_fd, char *path) {
    int watch_fd = inotify_add_watch(inotify_fd, path,
            IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE
    );
    // TODO probably don't to die in the general case, just on startup?
    perr_die_if(watch_fd < 0, "inotify_add_watch");
    watch_path_s *wp = alloc_watch_path(state);
    // NOTE: watched paths are never freed
    // they could be remove on delete calls in the future, but that would be more involved
    // we'd possibly even want to recreate the entire tree for the base path to resync
    wp->path = strdup(path);
    wp->base = 0;
    wp->fd = watch_fd;
    return wp;
}

void recursively_watch_dirs(watch_state_s *state, int inotify_fd, char *path, watch_path_s *base) {
    struct stat sb;
    // TODO need to check if this should be lstat instead
    // depends on if you can give symlinks to inotify
    int rc = stat(path, &sb);
    if (rc) {
        perror("stat");
        fprintf(stderr, "%s\n", path);
    } else {
        if (S_ISDIR(sb.st_mode)) {
            watch_path_s *wp = add_inotify_path(state, inotify_fd, path);
            wp->base = base;

            watch_link_s *link = alloc_link(state);
            link->wp = wp;
            link->next = state->first_watched;
            state->first_watched = link;

            if (!base) {
                // if we have no base, use the currently added on
                base = wp;
            }
            DIR *dp = opendir(path);
            struct dirent *ep;
            if (dp) {
                while ((ep = readdir(dp))) {
                    if (ep->d_name[0] != '.') {
                        char *fullpath;
                        perr_die_if(asprintf(&fullpath, "%s/%s", path, ep->d_name) < 0, "asprintf");
                        recursively_watch_dirs(state, inotify_fd, fullpath, base);
                        free(fullpath);
                    }
                }
                closedir(dp);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        die_usage(argv[0], "<watch-path>");
    }

    watch_state_s state = {0};

    int inotify_fd = inotify_init();
    perr_die_if(inotify_fd < 0, "inotify_init");
    for (unsigned int arg_idx = 1; arg_idx < argc; arg_idx++) {
        char *path_to_watch = argv[arg_idx];

        recursively_watch_dirs(&state, inotify_fd, path_to_watch, NULL);
    }

    queue_state_s *queue_state = &state.queue_state;
    char *wait_time_str = getenv("WATCHTAGS_WAIT_TIME");
    queue_state->wait_time = parse_wait_time(wait_time_str, 60);
    char *cmd = getenv("WATCHTAGS_CMD");
    if (cmd) {
        queue_state->cmd = cmd;
    } else {
        queue_state->cmd = "ctagspath";
    }

    pthread_t thread;
    perr_die_if(pthread_create(&thread, NULL, process_queue, &state) != 0, "pthread_create");

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
            for (watch_link_s *l = state.first_watched; l; l = l->next) {
                if (e->wd == l->wp->fd) {
                    watch_path_s *wp = l->wp;
                    if (wp->base) {
                        wp = wp->base;
                    }
                    state.first_dirty = add_watch_path_if_needed(&state, state.first_dirty, wp);
                    break;
                }
            }
        }
        while (!enqueue_watch_paths(&state, queue_state, state.first_dirty)) {
            perr_die_if(pthread_yield() != 0,  "pthread_yield");
        }
        free_links(&state, state.first_dirty);
        state.first_dirty = NULL;
    }

    return 0;
}
