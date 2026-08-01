#ifndef ST_PROCESS_H
#define ST_PROCESS_H

#include "./st_helper.h"
#include "./st_string.h"

#if defined(__linux__)
   #include <sys/wait.h>
    #include <unistd.h>
#elif defined(_WIN32)
   #include <windows.h>
#endif

// TODO: Not limit the max arg count rn this is the only wait I figured out to
// solve it.
#define MAX_ARGS 256

typedef struct {
    const char *args[MAX_ARGS + 1];
    FILE *out;
    b8 async;
} ST_proc_opt_t;

typedef struct {
    ST_proc_opt_t opt;
    i32 id;

#ifdef _WIN32
struct {
    HANDLE process;
    HANDLE thread;
    u32 process_id;
    u32 thread_id;
} platform;
#endif

} ST_proc_t;

typedef struct {
    ST_proc_t *items;
    u32 count, capacity;
} ST_procs_t;

void ST_append_process_opt(ST_procs_t *procs, ST_proc_opt_t proc);
b8 ST_run_process(ST_proc_t *procs);
b8 ST_wait_process(ST_proc_t *procs);
b8 ST_run_processes(ST_procs_t *procs);
void ST_free_process(ST_procs_t *procs);

#define ST_append_process(procs, ...) ST_append_process_opt((procs), (ST_proc_opt_t){__VA_ARGS__})

#endif
