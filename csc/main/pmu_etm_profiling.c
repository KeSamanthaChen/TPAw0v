/*
    Brief: This is a simple demo to show how to use ETM to trace a target application.

    This demo should run on ZCU102/Kria board as long as the APU has linux running.
    Contrary to the original paper, this demo does not need RPU. 

    The purpose of this demo is to provide a template for researchers who want to use the CoreSight debug infrastructure.

    This demo illustrates how to use ETR to route trace data to any memory mapped address.

    Author: Ke Chen
    Date: 2024-08-26
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/wait.h>
#include "common.h"
#include "pmu_event.h"
#include "cs_etm.h"
#include "cs_config.h"
#include "buffer.h"
#include "pmu_counter.h"
#include <stdlib.h>
#include <string.h>
#include "ini.h"

#define MAX_EVENTS 100
#define MAX_EVENT_NAME 50

typedef struct {
    char name[MAX_EVENT_NAME];
    int number;
} PmuEvent;

typedef struct {
    PmuEvent pmu_events[MAX_EVENTS];
    int pmu_events_count;
    char function_name[100];
    int etm_coefficient;
} configuration;

static int handler(void* user, const char* section, const char* name, const char* value) {
    configuration* pconfig = (configuration*)user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("Events", "pmu_events")) {
        char* line = strdup(value);
        char* token = strtok(line, "\n");
        while (token != NULL && pconfig->pmu_events_count < MAX_EVENTS) {
            while (*token == ' ' || *token == '\t' || *token == ';') token++; // Skip leading whitespace and semicolon
            
            if (*token != '\0') { // If there's anything left in the token
                char event_name[MAX_EVENT_NAME];
                int event_number;
                if (sscanf(token, "%[^:]:%d", event_name, &event_number) == 2) {
                    strncpy(pconfig->pmu_events[pconfig->pmu_events_count].name, event_name, MAX_EVENT_NAME - 1);
                    pconfig->pmu_events[pconfig->pmu_events_count].name[MAX_EVENT_NAME - 1] = '\0';
                    pconfig->pmu_events[pconfig->pmu_events_count].number = event_number;
                    pconfig->pmu_events_count++;
                }
            }
            token = strtok(NULL, "\n");
        }
        free(line);
    } else if (MATCH("Configuration", "function")) {
        if (strstr(value, "cs_config_etr_mp") != NULL) {
            strncpy(pconfig->function_name, "cs_config_etr_mp", sizeof(pconfig->function_name) - 1);
            pconfig->function_name[sizeof(pconfig->function_name) - 1] = '\0';
        }
    } else if (MATCH("Variables", "etm_coefficient")) {
        pconfig->etm_coefficient = atoi(value);
    }
    return 1;
}

int main(int argc, char* argv[]) {
    configuration config = {0}; // Initialize all fields to 0/NULL

    if (ini_parse("profiling_config.ini", handler, &config) < 0) {
        printf("Can't load 'config.ini'\n");
        return 1;
    }

    printf("PMU Events count: %d\n", config.pmu_events_count);
    for (int i = 0; i < config.pmu_events_count; i++) {
        printf("Event %d: %s:%d\n", i+1, config.pmu_events[i].name, config.pmu_events[i].number);
    }
    printf("Function name: %s\n", config.function_name);
    printf("ETM Coefficient: %d\n", config.etm_coefficient);

    return 0;
}

// extern ETM_interface *etms[4];

// int main(int argc, char *argv[])
// {


    // printf("Perf open.\n");
    // perf_open();

    // printf("Vanilla ZCU102 self-host trace demo.\n");
    // printf("Build: on %s at %s\n\n", __DATE__, __TIME__);

    // pid_t target_pid;

    // // Disabling all cpuidle. Access the ETM of an idled core will cause a hang.
    // linux_disable_cpuidle();
    
    // // Pin to the 4-th core, because we will use 1st core to run the target application.  
    // pin_to_core(3);

    // // configure CoreSight to use ETR; The addr and size is the On-Chip memory (OCM) on chip.
    // // You can change the addr and size to use any other
    // // uint64_t buf_addr = 0x00FFE00000; // RPU 0 ATCM
    // // uint32_t buf_size = 1024 * 64;
    // uint64_t buf_addr = 0x00FFFC0000;  //OCM
    // uint32_t buf_size = 1024 * 256;

    // cs_config_etr_mp(buf_addr, buf_size);

    // // clear the buffer
    // clear_buffer(buf_addr, buf_size);

    // // initialize ETM
    // config_etm_n(etms[0], 0, 1);

    // // config_pmu_enable_export();

    // // fork a child to execute the target application
    // for (int i = 0; i < 1; i++)
    // {
    //     target_pid = fork();
    //     if (target_pid == 0)
    //     {
    //         pin_to_core(i);
    //         uint64_t child_pid = (uint64_t) getpid();

    //         // further configure ETM. So that it will only trace the process with pid == child_pid/target_pid
    //         // with the program counter in the range of 0x400000 to 0x500000
    //         etm_set_contextid_cmp(etms[0], child_pid);
    //         etm_register_range(etms[0], 0x400000, 0x500000, 1);

    //         // etm_example_large_counter_fire_event(etms[0], L2D_CACHE_REFILL_T, 10000);

    //         perf_read(perf_prev_values);
    //         printf("Initial perf refill value %llu\n", perf_prev_values[0]);

    //         // Enable ETM, start trace session
    //         etm_enable(etms[0]);

    //         // execute target application
    //         execl("./hello_ETM", "hello_ETM", NULL);
    //         perror("execl failed. Target application failed to start.");
    //         exit(1);
    //     }
    //     else if (target_pid < 0)
    //     {
    //         perror("fork");
    //         return 1;
    //     }
    // }

    // // wait for target application to finish
    // int status;
    // waitpid(target_pid, &status, 0);

    // // Disable ETM, our trace session is done
    // etm_disable(etms[0]);

    // perf_read(perf_curr_values);
    // printf("Initial perf refill value %llu\n", perf_prev_values[0]);
    // printf("Final perf refill value %llu\n", perf_curr_values[0]);


    // perf_delta(perf_curr_values, perf_prev_values, perf_delta_values);

    // printf("L2D refill: %llu\n", perf_delta_values[0]);
    // printf("L2D write-back: %llu\n", perf_delta_values[1]);

    // munmap(etms[0], sizeof(ETM_interface));

    // dump_buffer(buf_addr, buf_size);

//     return 0;
// }