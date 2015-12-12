/* Copyright 2015 Google Inc. All Rights Reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sched.h>

#include "cpu_util.h"
#include "expand.h"
#include "timer.h"

typedef unsigned atomic_t;

typedef union {
        struct {
                atomic_t count;
                int cpu;
        } x;
        char pad[AVOID_FALSE_SHARING];
} per_thread_t;

per_thread_t global_counter;

static volatile int relaxed;

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static size_t nr_to_startup;
static uint64_t delay_mask;

static void wait_for_startup(void)
{
        // wait for everyone to spawn
        pthread_mutex_lock(&wait_mutex);
        --nr_to_startup;
        if (nr_to_startup) {
                pthread_cond_wait(&wait_cond, &wait_mutex);
        }
        else {
                pthread_cond_broadcast(&wait_cond);
        }
        pthread_mutex_unlock(&wait_mutex);
}

static void *worker(void *_args)
{
        per_thread_t *args = _args;
        
        // move to our target cpu
        cpu_set_t cpu;
        CPU_ZERO(&cpu);
        CPU_SET(args->x.cpu, &cpu);
        if (sched_setaffinity(0, sizeof(cpu), &cpu)) {
                perror("sched_setaffinity");
                exit(1);
        }

        wait_for_startup();

        if (delay_mask & (1u<<args->x.cpu)) {
                sleep(1);
        }
        while (!relaxed) {
                x50(__sync_fetch_and_add(&global_counter.x.count, 1););
                __sync_fetch_and_add(&args->x.count, 50);
        }

        if (delay_mask & (1u<<args->x.cpu)) {
                sleep(1);
        }
        while (relaxed) {
                x50(__sync_fetch_and_add(&global_counter.x.count, 1); cpu_relax(););
                __sync_fetch_and_add(&args->x.count, 50);
        }
        return NULL;
}

int main(int argc, char **argv)
{
        int c;

        delay_mask = 0;
        while ((c = getopt(argc, argv, "d:")) != -1) {
                switch (c) {
                case 'd':
                        delay_mask = strtoul(optarg, 0, 0);
                        break;
                default:
                        goto usage;
                }
        }

        if (argc - optind != 0) {
usage:
                fprintf(stderr, "usage: %s [-d delay_mask]\n"
                                "by default runs one thread on each cpu, use taskset(1) to\n"
                                "restrict operation to fewer cpus/threads.\n"
                                "the optional delay_mask specifies a mask of cpus on which to delay\n"
                                "the startup.\n", argv[0]);
                exit(1);
        }

        setvbuf(stdout, NULL, _IONBF, BUFSIZ);

        // find the active cpus
        cpu_set_t cpus;
        if (sched_getaffinity(getpid(), sizeof(cpus), &cpus)) {
                perror("sched_getaffinity");
                exit(1);
        }

        // could do this more efficiently, but whatever
        size_t nr_threads = 0;
        int i;
        for (i = 0; i < CPU_SETSIZE; ++i) {
                if (CPU_ISSET(i, &cpus)) {
                        ++nr_threads;
                }
        }

        per_thread_t *thread_args = calloc(nr_threads, sizeof(*thread_args));
        nr_to_startup = nr_threads + 1;
        size_t u;
        i = 0;
        for (u = 0; u < nr_threads; ++u) {
                while (!CPU_ISSET(i, &cpus)) {
                        ++i;
                }
                thread_args[u].x.cpu = i;
                ++i;
                thread_args[u].x.count = 0;
                pthread_t dummy;
                if (pthread_create(&dummy, NULL, worker, &thread_args[u])) {
                        perror("pthread_create");
                        exit(1);
                }
        }

        wait_for_startup();

        atomic_t *samples = calloc(nr_threads, sizeof(*samples));

        printf("results are avg latency per locked increment in ns, one column per thread\n");
        printf("cpu:");
        for (u = 0; u < nr_threads; ++u) {
                printf("%6u  ", thread_args[u].x.cpu);
        }
        printf("\n");
        for (relaxed = 0; relaxed < 2; ++relaxed) {
                printf(relaxed ? "relaxed:\n" : "unrelaxed:\n");

                uint64_t last_stamp = now_nsec();
                size_t sample_nr;
                for (sample_nr = 0; sample_nr < 6; ++sample_nr) {
                        usleep(500000);
                        for (u = 0; u < nr_threads; ++u) {
                                samples[u] = __sync_lock_test_and_set(&thread_args[u].x.count, 0);
                        }
                        uint64_t stamp = now_nsec();
                        int64_t time_delta = stamp - last_stamp;
                        last_stamp = stamp;

                        // throw away the first sample to avoid race issues at startup / mode switch
                        if (sample_nr == 0) continue;

                        printf("  ");
                        double sum = 0.;
                        double sum_squared = 0.;
                        for (u = 0; u < nr_threads; ++u) {
                                double s = time_delta / (double)samples[u];
                                printf("  %6.1f", s);
                                sum += s;
                                sum_squared += s*s;
                        }
                        printf(" : avg %6.1f  sdev %6.1f\n",
                                sum / nr_threads,
                                sqrt((sum_squared - sum*sum/nr_threads)/(nr_threads-1)));
                }
        }
        return 0;
}
