#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>
#include <ram-err.h>
#include <ram-def.h>

typedef struct {
    u32 idx;
} pathid_s;

typedef struct {
    int wd;
    pathid_s root;
    char *path;
} watch_info_s;

// NOTE: this can be a chunked list if we want to allow this to grow
typedef struct {
    u32 n;
    u32 cap;
    watch_info_s *infos;
} watch_infos_s;

typedef struct {
    u32 n_queue;
    pathid_s *queue;
    u32 mutex;
    unsigned int sleep;
    pid_t pid;
} queue_state_s;

typedef struct {
    int inotify_fd;
    u32 n_paths;
    char **paths;
    watch_infos_s watch;
    queue_state_s queue_state;
} watch_state_s;

void join_path(char *head, char *tail, size_t buf_size, char *buf) {
    die_if(snprintf(buf, buf_size, "%s/%s", head, tail) >= buf_size, "snprintf");
}

void path_changed(queue_state_s *qs, char *path) {
    char tmpfile[32];
    die_if(snprintf(tmpfile, sizeof(tmpfile), "/tmp/watch-tags-%d", qs->pid) >= sizeof(tmpfile), "snprintf");
    char fullcmd[128];
    die_if(snprintf(fullcmd, sizeof(fullcmd), "ctags -R -f %s", tmpfile) >= sizeof(fullcmd), "snprintf");

    perr_die_if(chdir(path) != 0, "chdir");

    FILE *pipe = popen(fullcmd, "r");
    // TODO check exit code
    pclose(pipe);

    char tagspath[PATH_MAX];
    join_path(path, "tags", sizeof(tagspath), tagspath);
    rename(tmpfile, tagspath);

    puts(path);
}

char *lookup_path(watch_state_s *state, pathid_s path_id) {
    assert(path_id.idx < state->n_paths);
    char *result = state->paths[path_id.idx];
    return result;
}

void *process_queue(void *pthread_data) {
    watch_state_s *state = pthread_data;
    queue_state_s *qs = &state->queue_state;
    for (;;) {
        while (!__sync_bool_compare_and_swap(&qs->mutex, 0, 1)) {
            perr_die_if(pthread_yield() != 0, "pthread_yield");
        }

        for (u32 i = 0; i < qs->n_queue; i++) {
            pathid_s path_id = qs->queue[i];
            char *path = lookup_path(state, path_id);
            path_changed(qs, path);
        }
        // clear queue
        qs->n_queue = 0;
        qs->mutex = 0;

        sleep(qs->sleep);
    }
    return NULL;
}

unsigned int parse_sleep(char *s, unsigned int default_sleep) {
    unsigned int result = default_sleep;
    if (s) {
        char *endptr = s;
        unsigned int parsed_val = strtol(s, &endptr, 10);
        if (endptr != s) {
            result = parsed_val;
        }
    }
    return result;
}

void watch_path(watch_state_s *state, char *fullpath, pathid_s toplevel_pathid) {
    int wd = inotify_add_watch(state->inotify_fd, fullpath,
            IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE
    );
    if (wd < 0) {
        perror("inotify_add_watch");
        fprintf(stderr, "%s\n", fullpath);
        return;
    }
    watch_infos_s *wis = &state->watch;
    assert(wis->n < wis->cap);
    watch_info_s *wi = wis->infos + wis->n++;
    wi->wd = wd;
    wi->root = toplevel_pathid;
    wi->path = strdup(fullpath);
}

void recursively_watch_dirs(watch_state_s *state, char *fullpath, pathid_s toplevel_pathid) {
    struct stat sb;
    // TODO need to check if this should be lstat instead
    // depends on if you can give symlinks to inotify
    int rc = stat(fullpath, &sb);
    if (rc) {
        perror("stat");
        fprintf(stderr, "%s\n", fullpath);
    } else {
        if (S_ISDIR(sb.st_mode)) {

            watch_path(state, fullpath, toplevel_pathid);

            DIR *dp = opendir(fullpath);
            struct dirent *ep;
            if (dp) {
                while ((ep = readdir(dp))) {
                    if (ep->d_name[0] != '.') {
                        char fulldirpath[PATH_MAX];
                        join_path(fullpath, ep->d_name, sizeof(fulldirpath), fulldirpath);
                        recursively_watch_dirs(state, fulldirpath, toplevel_pathid);
                    }
                }
                closedir(dp);
            }
        }
    }
}

bool pathid_matches(pathid_s a, pathid_s b) {
    return a.idx == b.idx;
}

void add_pathid(u32 *n, pathid_s *pathids, pathid_s pathid) {
    u32 i;
    for (i = 0; i < *n; i++) {
        if (pathid_matches(pathids[i], pathid)) {
            break;
        }
    }
    if (i == *n) {
        pathids[*n] = pathid;
        *n += 1;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        die_usage(argv[0], "<watch-path>");
    }

    watch_state_s state = {0};

    state.inotify_fd = inotify_init();
    perr_die_if(state.inotify_fd < 0, "inotify_init");

    state.n_paths = argc - 1;
    size_t path_buf_size = sizeof(char *) * state.n_paths + PATH_MAX * state.n_paths;
    state.paths = malloc(path_buf_size);
    char *path_buf = (char *)(state.paths + state.n_paths);
    for (u32 i = 0; i < state.n_paths; i++) {
        char *path = argv[i + 1];
        char *abs_path = realpath(path, path_buf);
        perr_die_if(!abs_path, "realpath");
        state.paths[i] = path_buf;
        path_buf += strlen(path_buf) + 1;
    }

    state.watch.cap = 4096;
    state.watch.infos = malloc(sizeof(watch_info_s) * state.watch.cap);

    for (u32 i = 0; i < state.n_paths; i++) {
        pathid_s pathid = {.idx=i};
        char *path_to_watch = state.paths[i];
        recursively_watch_dirs(&state, path_to_watch, pathid);
    }

    queue_state_s *queue_state = &state.queue_state;
    char *sleep_str = getenv("WATCHTAGS_SLEEP");

    queue_state->sleep = parse_sleep(sleep_str, 30);
    queue_state->pid = getpid();
    queue_state->queue = malloc(sizeof(pathid_s) * state.n_paths);

    pthread_t thread;
    perr_die_if(pthread_create(&thread, NULL, process_queue, &state) != 0, "pthread_create");

    char inotify_buf[4096];

    pathid_s *dirty_path_ids = malloc(sizeof(pathid_s) * state.n_paths);
    u32 n_dirty_path_ids = 0;

    for (;;) {
        ssize_t n = read(state.inotify_fd, inotify_buf, sizeof(inotify_buf));
        perr_die_if(n < 0, "read");
        for (char *p = inotify_buf; p < inotify_buf + n; p += sizeof(struct inotify_event)) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (!e->mask) {
                continue;
            }
            if (strcmp(e->name, "tags") == 0) {
                // it's important to ignore tags modifications
                // otherwise we'll get stuck in a loop!
                continue;
            }
            for (u32 i = 0; i < state.watch.n; i++) {
                watch_info_s *wi = state.watch.infos + i;
                if (wi->wd == e->wd) {
                    pathid_s pathid = wi->root;
                    add_pathid(&n_dirty_path_ids, dirty_path_ids, pathid);

                    if (e->mask & (IN_CREATE | IN_ISDIR)) {
                        char fullpath[PATH_MAX];
                        join_path(wi->path, e->name, sizeof(fullpath), fullpath);
                        watch_path(&state, fullpath, pathid);
                    }
                    break;
                }
            }
        }
        while (!__sync_bool_compare_and_swap(&queue_state->mutex, 0, 1)) {
            perr_die_if(pthread_yield() != 0, "pthread_yield");
        }
        for (u32 i = 0; i < n_dirty_path_ids; i++) {
            pathid_s pathid = dirty_path_ids[i];
            add_pathid(&queue_state->n_queue, queue_state->queue, pathid);
        }
        n_dirty_path_ids = 0;
        queue_state->mutex = 0;
    }

    return 0;
}
