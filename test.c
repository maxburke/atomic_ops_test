#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

// The list of tests.
#define TESTS \
    T(add) \
    T(add_mfence) \
    T(lockadd) \
    T(xadd) \
    T(swap) \
    T(cmpxchg) \
    T(lockadd_unalign)

// The list of interference modes
#define INTERFERENCE_MODES \
    T(none) \
    T(hyperthread_read_line) \
    T(hyperthread_write_line) \
    T(other_core_read_line) \
    T(other_core_write_line) \
    T(three_cores_read_line) \
    T(three_cores_write_line)

// Declare the tests
#define T(id) extern int64_t test_##id(uint64_t *mem);
TESTS
#undef T

extern void interference_read(uint64_t *mem);
extern void interference_write(uint64_t *mem);

// Declare interference modes
enum
{
#define T(id) IM_##id,
    INTERFERENCE_MODES
    IM_count
#undef T
};

static const char *interference_name(int which)
{
    static const char *names[] = {
    #define T(id) #id,
        INTERFERENCE_MODES
    #undef T
    };
    return names[which];
}

static void lock_to_logical_core(uint32_t which)
{
    cpu_set_t cpu_set;
    int num_cores;

    num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    if (which >= num_cores) {
        printf("Num cores: %d\n", num_cores);
        exit(1);
    }

    CPU_ZERO(&cpu_set);
    CPU_SET(which, &cpu_set);
    if (sched_setaffinity(0, sizeof cpu_set, &cpu_set) < 0) {
        perror("Unable to set affinity");
        exit(1);
    }
}

// Interference thread
enum { NUM_INTERFERENCE_THREADS = 3 };

typedef struct {
    int core_id;
    int interference_mode;
} thread_args;

// Scratch area. This is where our memory updates go to.
// Cache-line aligned (on x86-64).
static uint64_t scratch[16] __attribute__((aligned(64)));

// We use this event to signal to the interference thread that it's time to exit.
static volatile long time_to_exit;

// Number of threads that have started running the interference main loop.
static volatile long num_running;

static void *interference_thread(void *argp)
{
    const thread_args *args = (const thread_args *)argp;
    uint64_t private_mem[8] = { 0 };
    uint64_t *interfere_ptr = private_mem;
    int do_writes = 0;
    int just_started = 1;

    lock_to_logical_core(args->core_id);

    switch (args->interference_mode) {
    case IM_hyperthread_read_line:
        if (args->core_id == 1)
            interfere_ptr = scratch;
        break;

    case IM_hyperthread_write_line:
        if (args->core_id == 1) {
            interfere_ptr = scratch;
            do_writes = 1;
        }
        break;

    case IM_other_core_read_line:
        if (args->core_id == 2)
            interfere_ptr = scratch;
        break;

    case IM_other_core_write_line:
        if (args->core_id == 2) {
            interfere_ptr = scratch;
            do_writes = 1;
        }
        break;

    case IM_three_cores_read_line:
        if (args->core_id == 2 || args->core_id == 4 || args->core_id == 6)
            interfere_ptr = scratch;
        break;

    case IM_three_cores_write_line:
        if (args->core_id == 2 || args->core_id == 4 || args->core_id == 6) {
            interfere_ptr = scratch;
            do_writes = 1;
        }
        break;
    }

    // main loop
    while (!time_to_exit) {
        if (!do_writes)
            interference_read(interfere_ptr);
        else
            interference_write(interfere_ptr);

        // after first loop through (so everything is warmed up),
        // this thread counts as "running".
        if (just_started) {
            just_started = 0;
            __sync_add_and_fetch(&num_running, 1);
        }
    }

    return 0;
}

static int compare_results(const void *a, const void *b)
{
    int64_t ia = *(const int64_t *)a;
    int64_t ib = *(const int64_t *)b;
    if (ia != ib)
        return (ia < ib) ? -1 : 1;
    else
        return 0;
}

static int64_t run_test(int64_t (*test_kernel)(uint64_t *mem))
{
    // run a lot of times and report the median
    enum { NUM_RUNS = 100 };
    int64_t result[NUM_RUNS];
    int run;

    for (run = 0; run < NUM_RUNS; run++)
        result[run] = test_kernel(scratch);

    qsort(result, NUM_RUNS, sizeof(result[0]), compare_results);
    return result[NUM_RUNS / 2];
}

int main()
{
    int interference_mode;

    lock_to_logical_core(0);

    for (interference_mode = 0; interference_mode < IM_count; interference_mode++) {
        thread_args args[NUM_INTERFERENCE_THREADS];
        pthread_t threads[NUM_INTERFERENCE_THREADS];
        pthread_attr_t attr;
        int i;

        printf("interference type: %s\n", interference_name(interference_mode));
        pthread_attr_init(&attr);

        // start the interference threads
        num_running = 0;
        for (i = 0; i < NUM_INTERFERENCE_THREADS; i++) {
            args[i].core_id = i + 1;
            args[i].interference_mode = interference_mode;
            pthread_create(&threads[i], &attr, interference_thread, &args[i]);
        }

        // wait until they're all running (yeah, evil spin loop)
        while (num_running < NUM_INTERFERENCE_THREADS)
            sleep(0);
        
        // run our tests
        #define T(id) printf("%16s: %8.2f cycles/op\n", #id, (double) run_test(test_##id) / 40000.0);
        TESTS
        #undef T

        // wait for thread to shut down
        time_to_exit = 1;

        for (i = 0; i < NUM_INTERFERENCE_THREADS; i++) {
            void *res;
            pthread_join(threads[i], &res);
        }

        pthread_attr_destroy(&attr);

        time_to_exit = 0;
    }

    return 0;
}
