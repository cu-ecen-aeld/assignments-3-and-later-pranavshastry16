#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;

    if(thread_func_args == NULL)
    {
        return thread_param;
    }

    thread_func_args->thread_complete_success = false;

    // Sleep before obtaining mutex
    if(thread_func_args->wait_to_obtain_ms > 0)
    {
        usleep((useconds_t)thread_func_args->wait_to_obtain_ms * 1000);
    }

    // Obtain mutex
    if(pthread_mutex_lock(thread_func_args->mutex) != 0)
    {
        ERROR_LOG("pthread_mutex_lock failed");
        return thread_param;
    }

    // Hold mutex for specified time
    if(thread_func_args->wait_to_release_ms > 0)
    {
        usleep((useconds_t)thread_func_args->wait_to_release_ms * 1000);
    }

    // Release mutex
    if(pthread_mutex_unlock(thread_func_args->mutex) != 0)
    {
        ERROR_LOG("pthread_mutex_unlock failed");
        return thread_param;
    }

    thread_func_args->thread_complete_success = true;
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    if(thread == NULL || mutex == NULL)
    {
        return false;
    }

    struct thread_data *data = (struct thread_data *)malloc(sizeof(struct thread_data));
    if(data == NULL)
    {
        ERROR_LOG("malloc failed");
        return false;
    }

    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;

    int rc = pthread_create(thread, NULL, threadfunc, data);
    if(rc != 0)
    {
        ERROR_LOG("pthread_create failed (%d)", rc);
        free(data);
        return false;
    }

    return true;
}

