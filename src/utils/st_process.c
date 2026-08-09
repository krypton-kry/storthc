#define _POSIX_C_SOURCE 200809L

#include "st_process.h"
#include <stdio.h>

// stolen from nob.h
void ST_win32_cmd_quote(const char **cmd, ST_sb_t *sb) {
    
    const char **original_args = cmd;
    int arg_count = 0;
    
    while (original_args[arg_count] != NULL)
        arg_count++;
    
    for (int i = 0; i < arg_count; ++i) {
        const char *arg = cmd[i];
        if (arg == NULL) break;
        size_t len = strlen(arg);
        if (i > 0) ST_da_append(sb, ' ');
        if (len != 0 && NULL == strpbrk(arg, " \t\n\v\"")) {
            // no need to quote
            ST_append_to_builder(sb, arg);
        } else {
            // we need to escape:
            // 1. double quotes in the original arg
            // 2. consequent backslashes before a double quote
            size_t backslashes = 0;
            ST_da_append(sb, '\"');
            for (size_t j = 0; j < len; ++j) {
                char x = arg[j];
                if (x == '\\') {
                    backslashes += 1;
                } else {
                    if (x == '\"') {
                        // escape backslashes (if any) and the double quote
                        for (size_t k = 0; k < 1+backslashes; ++k) {
                            ST_da_append(sb, '\\');
                        }
                    }
                    backslashes = 0;
                }
                ST_da_append(sb, x);
            }
            // escape backslashes (if any)
            for (size_t k = 0; k < backslashes; ++k) {
                ST_da_append(sb, '\\');
            }
            ST_da_append(sb, '\"');
        }
    }
}
 
// This will append all the process options I have set into procs structure
// which will be used in the actual run process.
void ST_append_process_opt(ST_procs_t *procs, ST_proc_opt_t opt) {
    ST_proc_t p = {0};
    p.opt = opt;
    p.id = -1;
    ST_da_append(procs, p);
}

// This functions run the process.
b8 ST_run_process(ST_proc_t *proc) {
#if defined(__linux__)
    fflush(NULL);
    pid_t id = fork();
    if (id < 0)
        return 0;
    if (id == 0) {
        if (proc->opt.out)
            dup2(fileno(proc->opt.out), STDOUT_FILENO);

        // This is a trick to make stdout flush I did not disable buffering.
        // This is for when passing -r to run the command which obv is not
        // buffered so it does not auto show me stuff printed to stdout.
        char *const *original_args = (char *const *)proc->opt.args;
        int arg_count = 0;
        while (original_args[arg_count] != NULL)
            arg_count++;
        char **new_args = malloc((arg_count + 3) * sizeof(char *));
        if (new_args == NULL) {
            _exit(127);
        }
        new_args[0] = "stdbuf";
        new_args[1] = "-o0";

        for (int i = 0; i < arg_count; i++) {
            new_args[i + 2] = original_args[i];
        }

        new_args[arg_count + 2] = NULL;
        execvp(new_args[0], new_args);
        _exit(127);
    }
    proc->id = id;
    if (proc->opt.async)
        return 1;
    return ST_wait_process(proc);

#elif defined(_WIN32)
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION	info = {0};
    startup.cb = sizeof(startup);
    
    ST_sb_t sb = {0};
    ST_win32_cmd_quote(proc->opt.args, &sb);
    char *args = strdup(ST_sb_cstr(&sb));
    free(sb.items);

    if(!CreateProcessA(NULL, args, NULL, NULL, 0, 0, NULL, NULL, &startup, &info)) {
        u32 err = GetLastError();
        fprintf(stderr, "CreateProcess for `%s` failed with error : %lu\n", args, err);
        free(args);
        return 0;
    }
    free(args);
    
    proc->platform.process = info.hProcess;
    proc->platform.thread = info.hThread;
    proc->platform.process_id = info.dwProcessId;
    proc->platform.thread_id = info.dwThreadId;
    
    if (proc->opt.async)
        return 1;
    return ST_wait_process(proc);
#endif
}

b8 ST_wait_process(ST_proc_t *proc) {
#if defined(__linux__)
    if (proc->id <= 0)
        return 1;
    int status = 0;
    pid_t r = waitpid(proc->id, &status, 0);
    proc->id = -1;
    if (r < 0)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;

#elif defined(_WIN32)
    if(WaitForSingleObject(proc->platform.process, INFINITE) != WAIT_OBJECT_0) {
        fprintf(stderr, "WaitForSingleObject failed: %lu\n", GetLastError());
        return 0;
    }
    
    u32 exit_code = 0;
    int result = GetExitCodeProcess(proc->platform.process, &exit_code);
    
    CloseHandle(proc->platform.process);
    CloseHandle(proc->platform.thread);
    if(!result) return 0;
    
    return exit_code == 0;
#endif
}

// TODO make customizable with reset or no reset.
b8 ST_run_processes(ST_procs_t *procs) {
    b8 ok = 1;
    for (u32 i = 0; i < procs->count; i++) {
        if (!ST_run_process(&procs->items[i]))
            ok = 0;
    }

    for (u32 i = 0; i < procs->count; i++) {
        if(procs->items[i].opt.async)
            if (!ST_wait_process(&procs->items[i]))
                ok = 0;
    }

    procs->count = 0;
    return ok;
}

void ST_free_process(ST_procs_t *procs) {
    free(procs->items);
    procs->count = 0;
    procs->capacity = 0;
}
