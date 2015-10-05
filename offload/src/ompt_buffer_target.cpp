#include "offload.h"
#include "compiler_if_target.h"
#include "ompt_callback_target.h"
#include <common/COIEvent_common.h>
#include <sys/time.h>
#include <pthread.h>
#include <ompt.h>

ompt_get_thread_id_t my_ompt_get_thread_id;

__thread ompt_record_t* ompt_target_event_buffer;
__thread uint64_t ompt_buffer_size = 0;

// FIXME: We have to avoid using global variables.
ompt_record_t* ompt_target_event_buffer_g;
uint64_t ompt_buffer_size_g = 0;

COIEVENT* ompt_buffer_request_events;
COIEVENT* ompt_buffer_complete_events;
uint64_t* ompt_buffer_pos;

pthread_mutex_t mutex_buffer_request;
pthread_mutex_t mutex_buffer_complete;

pthread_cond_t waiting_buffer_request;
pthread_cond_t waiting_buffer_complete;
pthread_mutex_t mutex_waiting_buffer_request;
pthread_mutex_t mutex_waiting_buffer_complete;
bool buffer_request_condition = false;
bool buffer_full_condition = false;

bool tracing = false;


uint64_t ompt_get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return 1000000 * tv.tv_sec + tv.tv_usec;
}

void ompt_buffer_add_target_event(ompt_record_t event) {
    // The OMPT thread IDs start with 1 such that we will have to shift tid
    // by 1 to get the right array elements.
    ompt_thread_id_t tid = my_ompt_get_thread_id();

    if (tracing) {
        if (ompt_buffer_size == 0) {
            // request buffer, send signal to host
            pthread_mutex_lock(&mutex_buffer_request);
            pthread_mutex_lock(&mutex_waiting_buffer_request);
            COIEventSignalUserEvent(ompt_buffer_request_events[tid-1]);

            // wait for tool allocating buffer on host
            while (!buffer_request_condition) 
                pthread_cond_wait(&waiting_buffer_request, &mutex_waiting_buffer_request);
            buffer_request_condition = false;

            // copy buffer size and pointer to TLS
            ompt_buffer_size = ompt_buffer_size_g;

            ompt_target_event_buffer = ompt_target_event_buffer_g;
            pthread_mutex_unlock(&mutex_waiting_buffer_request);
            pthread_mutex_unlock(&mutex_buffer_request);
        }

        event.thread_id = tid;
        event.dev_task_id = 0;
        ompt_target_event_buffer[ompt_buffer_pos[tid-1]] = event;
        ompt_buffer_pos[tid-1]++;

        if (ompt_buffer_pos[tid-1] >= ompt_buffer_size) {
            pthread_mutex_lock(&mutex_buffer_complete);
            pthread_mutex_lock(&mutex_waiting_buffer_complete);
            COIEventSignalUserEvent(ompt_buffer_complete_events[tid-1]);
        
            // wait for tool truncating the buffer on host
            while (!buffer_full_condition)
                pthread_cond_wait(&waiting_buffer_complete, &mutex_waiting_buffer_complete); 
            buffer_full_condition = false;

            ompt_buffer_pos[tid-1] = 0;
            pthread_mutex_unlock(&mutex_waiting_buffer_complete);
            pthread_mutex_unlock(&mutex_buffer_complete);
        }
    }
}



COINATIVELIBEXPORT
void ompt_target_start_tracing(
    uint32_t  buffer_count,
    void**    buffers,
    uint64_t* buffers_len,
    void*     misc_data,
    uint16_t  misc_data_len,
    void*     return_data,
    uint16_t  return_data_len
)
{
    // initialize mutexes and condition variables
    pthread_cond_init(&waiting_buffer_request, NULL);
    pthread_cond_init(&waiting_buffer_complete, NULL);
    pthread_mutex_init(&mutex_buffer_request, NULL);
    pthread_mutex_init(&mutex_buffer_complete, NULL);
    pthread_mutex_init(&mutex_waiting_buffer_request, NULL);
    pthread_mutex_init(&mutex_waiting_buffer_complete, NULL);
    tracing = true;

    // FIXME: get buffers from host and save in global variables is ugly
    ompt_buffer_request_events = (COIEVENT*) buffers[0];
    ompt_buffer_complete_events = (COIEVENT*) buffers[1];
    ompt_buffer_pos = (uint64_t*) buffers[2];
}

COINATIVELIBEXPORT
void ompt_target_stop_tracing(
    uint32_t  buffer_count,
    void**    buffers,
    uint64_t* buffers_len,
    void*     misc_data,
    uint16_t  misc_data_len,
    void*     return_data,
    uint16_t  return_data_len
)
{
    tracing = false;
}

COINATIVELIBEXPORT
void ompt_signal_buffer_allocated(
    uint32_t  buffer_count,
    void**    buffers,
    uint64_t* buffers_len,
    void*     misc_data,
    uint16_t  misc_data_len,
    void*     return_data,
    uint16_t  return_data_len
)
{
    // FIXME: get buffer from host and save in global variable is ugly
    memcpy(&ompt_buffer_size_g, misc_data, sizeof(uint64_t));
    //ompt_buffer_size_g = *buffers_len / sizeof(ompt_record_t);
    ompt_buffer_size_g /= sizeof(ompt_record_t);

    ompt_target_event_buffer_g = (ompt_record_t*) buffers[0];

    pthread_mutex_lock(&mutex_waiting_buffer_request);
    buffer_request_condition = true;
    pthread_cond_signal(&waiting_buffer_request);
    pthread_mutex_unlock(&mutex_waiting_buffer_request);

        
}

COINATIVELIBEXPORT
void ompt_signal_buffer_truncated(
    uint32_t  buffer_count,
    void**    buffers,
    uint64_t* buffers_len,
    void*     misc_data,
    uint16_t  misc_data_len,
    void*     return_data,
    uint16_t  return_data_len
)
{
    pthread_mutex_lock(&mutex_waiting_buffer_complete);
    buffer_full_condition = true;
    pthread_cond_signal(&waiting_buffer_complete);
    pthread_mutex_unlock(&mutex_waiting_buffer_complete);
}


/* Register OMPT callbacks on device */

/*******************************************************************
 * Function declaration
 *******************************************************************/

#define OMPT_FN_TYPE(fn) fn ## _t 
#define OMPT_FN_LOOKUP(lookup,fn) fn = (OMPT_FN_TYPE(fn)) lookup(#fn)
#define OMPT_FN_DECL(fn) OMPT_FN_TYPE(fn) fn

OMPT_FN_DECL(ompt_set_callback);
OMPT_FN_DECL(ompt_get_thread_id);

/*******************************************************************
 * required events 
 *******************************************************************/

TEST_THREAD_TYPE_CALLBACK(ompt_event_thread_begin)
TEST_THREAD_TYPE_CALLBACK(ompt_event_thread_end)
TEST_NEW_PARALLEL_CALLBACK(ompt_event_parallel_begin)
TEST_PARALLEL_CALLBACK(ompt_event_parallel_end)
TEST_NEW_TASK_CALLBACK(ompt_event_task_begin)
TEST_NEW_TASK_CALLBACK(ompt_event_task_end)
TEST_CONTROL_CALLBACK(ompt_event_control)
TEST_CALLBACK(ompt_event_runtime_shutdown)

/*******************************************************************
 * optional events
 *******************************************************************/

/* Blameshifting events */
TEST_THREAD_CALLBACK(ompt_event_idle_begin)
TEST_THREAD_CALLBACK(ompt_event_idle_end)
TEST_PARALLEL_CALLBACK(ompt_event_wait_barrier_begin)
TEST_PARALLEL_CALLBACK(ompt_event_wait_barrier_end)
TEST_PARALLEL_CALLBACK(ompt_event_wait_taskwait_begin)
TEST_PARALLEL_CALLBACK(ompt_event_wait_taskwait_end)
TEST_PARALLEL_CALLBACK(ompt_event_wait_taskgroup_begin)
TEST_PARALLEL_CALLBACK(ompt_event_wait_taskgroup_end)
TEST_WAIT_CALLBACK(ompt_event_release_lock)
TEST_WAIT_CALLBACK(ompt_event_release_nest_lock_last)
TEST_WAIT_CALLBACK(ompt_event_release_critical)
TEST_WAIT_CALLBACK(ompt_event_release_ordered)
TEST_WAIT_CALLBACK(ompt_event_release_atomic)

/* synchronous events */
TEST_PARALLEL_CALLBACK(ompt_event_implicit_task_begin)
TEST_PARALLEL_CALLBACK(ompt_event_implicit_task_end)
TEST_PARALLEL_CALLBACK(ompt_event_initial_task_begin)
TEST_PARALLEL_CALLBACK(ompt_event_initial_task_end)
TEST_TASK_SWITCH_CALLBACK(ompt_event_task_switch)
TEST_WAIT_CALLBACK(ompt_event_init_lock)
TEST_WAIT_CALLBACK(ompt_event_init_nest_lock)
TEST_WAIT_CALLBACK(ompt_event_destroy_lock)
TEST_WAIT_CALLBACK(ompt_event_destroy_nest_lock)
TEST_NEW_WORKSHARE_CALLBACK(ompt_event_loop_begin)
TEST_PARALLEL_CALLBACK(ompt_event_loop_end)
TEST_NEW_WORKSHARE_CALLBACK(ompt_event_sections_begin)
TEST_PARALLEL_CALLBACK(ompt_event_sections_end)
TEST_NEW_WORKSHARE_CALLBACK(ompt_event_single_in_block_begin)
TEST_PARALLEL_CALLBACK(ompt_event_single_in_block_end)
TEST_PARALLEL_CALLBACK(ompt_event_single_others_begin)
TEST_PARALLEL_CALLBACK(ompt_event_single_others_end)
TEST_NEW_WORKSHARE_CALLBACK(ompt_event_workshare_begin)
TEST_PARALLEL_CALLBACK(ompt_event_workshare_end)
TEST_PARALLEL_CALLBACK(ompt_event_master_begin)
TEST_PARALLEL_CALLBACK(ompt_event_master_end)
TEST_PARALLEL_CALLBACK(ompt_event_barrier_begin)
TEST_PARALLEL_CALLBACK(ompt_event_barrier_end)
TEST_PARALLEL_CALLBACK(ompt_event_taskwait_begin)
TEST_PARALLEL_CALLBACK(ompt_event_taskwait_end)
TEST_PARALLEL_CALLBACK(ompt_event_taskgroup_begin)
TEST_PARALLEL_CALLBACK(ompt_event_taskgroup_end)
TEST_WAIT_CALLBACK(ompt_event_wait_lock)
TEST_WAIT_CALLBACK(ompt_event_acquired_lock)
TEST_WAIT_CALLBACK(ompt_event_wait_nest_lock)
TEST_WAIT_CALLBACK(ompt_event_acquired_nest_lock_first)
TEST_PARALLEL_CALLBACK(ompt_event_release_nest_lock_prev)
TEST_PARALLEL_CALLBACK(ompt_event_acquired_nest_lock_next)
TEST_WAIT_CALLBACK(ompt_event_wait_critical)
TEST_WAIT_CALLBACK(ompt_event_acquired_critical)
TEST_WAIT_CALLBACK(ompt_event_wait_ordered)
TEST_WAIT_CALLBACK(ompt_event_acquired_ordered)
TEST_WAIT_CALLBACK(ompt_event_wait_atomic)
TEST_WAIT_CALLBACK(ompt_event_acquired_atomic)
TEST_THREAD_CALLBACK(ompt_event_flush)

/*******************************************************************
 * Register the events
 *******************************************************************/

#define CHECK(EVENT) ompt_set_callback(EVENT, (ompt_callback_t) my_##EVENT); 

int ompt_initialize(ompt_function_lookup_t lookup, const char *runtime_version, unsigned int ompt_version) {
    printf("Initializing OMPT on device...\n");

    my_ompt_get_thread_id = (ompt_get_thread_id_t) lookup("ompt_get_thread_id");


    /* look up and bind OMPT API functions */

    OMPT_FN_LOOKUP(lookup,ompt_set_callback);
    OMPT_FN_LOOKUP(lookup,ompt_get_thread_id);

    /* required events */

    CHECK(ompt_event_parallel_begin);
    CHECK(ompt_event_parallel_end);
    CHECK(ompt_event_task_begin);
    CHECK(ompt_event_task_end);
    CHECK(ompt_event_thread_begin);
    CHECK(ompt_event_thread_end);
    CHECK(ompt_event_control);
    CHECK(ompt_event_runtime_shutdown);

    /* optional events, "blameshifting" */

    CHECK(ompt_event_idle_begin);
    CHECK(ompt_event_idle_end);
    CHECK(ompt_event_wait_barrier_begin);
    CHECK(ompt_event_wait_barrier_end);
    CHECK(ompt_event_wait_taskwait_begin);
    CHECK(ompt_event_wait_taskwait_end);
    CHECK(ompt_event_wait_taskgroup_begin);
    CHECK(ompt_event_wait_taskgroup_end);
    CHECK(ompt_event_release_lock);
    CHECK(ompt_event_release_nest_lock_last);
    CHECK(ompt_event_release_critical);
    CHECK(ompt_event_release_atomic);
    CHECK(ompt_event_release_ordered);

    /* optional events, synchronous */

    CHECK(ompt_event_implicit_task_begin);
    CHECK(ompt_event_implicit_task_end);
    CHECK(ompt_event_master_begin);
    CHECK(ompt_event_master_end);
    CHECK(ompt_event_barrier_begin);
    CHECK(ompt_event_barrier_end);
    CHECK(ompt_event_task_switch);
    CHECK(ompt_event_loop_begin);
    CHECK(ompt_event_loop_end);
    CHECK(ompt_event_sections_begin);
    CHECK(ompt_event_sections_end);
    CHECK(ompt_event_single_in_block_begin);
    CHECK(ompt_event_single_in_block_end);
    CHECK(ompt_event_single_others_begin);
    CHECK(ompt_event_single_others_end);
    CHECK(ompt_event_taskwait_begin);
    CHECK(ompt_event_taskwait_end);
    CHECK(ompt_event_taskgroup_begin);
    CHECK(ompt_event_taskgroup_end);
    CHECK(ompt_event_release_nest_lock_prev);
    CHECK(ompt_event_wait_lock);
    CHECK(ompt_event_wait_nest_lock);
    CHECK(ompt_event_wait_critical);
    CHECK(ompt_event_wait_atomic);
    CHECK(ompt_event_wait_ordered);
    CHECK(ompt_event_acquired_lock);
    CHECK(ompt_event_acquired_nest_lock_first);
    CHECK(ompt_event_acquired_nest_lock_next);
    CHECK(ompt_event_acquired_critical);
    CHECK(ompt_event_acquired_atomic);
    CHECK(ompt_event_acquired_ordered);
    CHECK(ompt_event_init_lock);
    CHECK(ompt_event_init_nest_lock);
    CHECK(ompt_event_destroy_lock);
    CHECK(ompt_event_destroy_nest_lock);
    CHECK(ompt_event_flush);
    
    return 1;
}

