#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
// Code updated with assistance from DeepSeek: https://chat.deepseek.com/share/v7krojhn6u2k69cx2r

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
	struct thread_data* thread_func_args = (struct thread_data*) thread_param;
    
    // Set initial success flag to false
    thread_func_args->thread_complete_success = false;
    
    // 1. Wait before obtaining mutex
    DEBUG_LOG("Thread waiting %d ms before obtaining mutex", thread_func_args->wait_to_obtain_ms);
    usleep(thread_func_args->wait_to_obtain_ms * 1000);
    
    // 2. Obtain the mutex
    DEBUG_LOG("Thread attempting to obtain mutex");
    int lock_result = pthread_mutex_lock(thread_func_args->mutex);
    if (lock_result != 0) {
        ERROR_LOG("Failed to obtain mutex: %d", lock_result);
        thread_func_args->thread_complete_success = false;
        return thread_param;
    }
    
    // 3. Wait while holding mutex
    DEBUG_LOG("Thread holding mutex for %d ms", thread_func_args->wait_to_release_ms);
    usleep(thread_func_args->wait_to_release_ms * 1000);
    
    // 4. Release the mutex
    DEBUG_LOG("Thread releasing mutex");
    int unlock_result = pthread_mutex_unlock(thread_func_args->mutex);
    if (unlock_result != 0) {
        ERROR_LOG("Failed to release mutex: %d", unlock_result);
        thread_func_args->thread_complete_success = false;
        return thread_param;
    }
    
    // Set success flag to true
    thread_func_args->thread_complete_success = true;
    DEBUG_LOG("Thread completed successfully");
    
    return thread_param;
}


/**
* Start a thread which sleeps @param wait_to_obtain_ms number of milliseconds, then obtains the
* mutex in @param mutex, then holds for @param wait_to_release_ms milliseconds, then releases.
* The start_thread_obtaining_mutex function should only start the thread and should not block
* for the thread to complete.
* The start_thread_obtaining_mutex function should use dynamic memory allocation for thread_data
* structure passed into the thread.  The number of threads active should be limited only by the
* amount of available memory.
* The thread started should return a pointer to the thread_data structure when it exits, which can be used
* to free memory as well as to check thread_complete_success for successful exit.
* If a thread was started succesfully @param thread should be filled with the pthread_create thread ID
* coresponding to the thread which was started.
* @return true if the thread could be started, false if a failure occurred.
*/
bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    // Validate input parameters
    if (thread == NULL || mutex == NULL || wait_to_obtain_ms < 0 || wait_to_release_ms < 0) {
        ERROR_LOG("Invalid input parameters");
        return false;
    }
    
    // Allocate memory for thread_data structure
    struct thread_data* thread_data_ptr = (struct thread_data*)malloc(sizeof(struct thread_data));
    if (thread_data_ptr == NULL) {
        ERROR_LOG("Failed to allocate memory for thread_data");
        return false;
    }
    
    // Initialize thread_data structure
    thread_data_ptr->mutex = mutex;
    thread_data_ptr->wait_to_obtain_ms = wait_to_obtain_ms;
    thread_data_ptr->wait_to_release_ms = wait_to_release_ms;
    thread_data_ptr->thread_complete_success = false;  // Initially false
    
    // Create the thread
    int create_result = pthread_create(thread, NULL, threadfunc, (void*)thread_data_ptr);
    if (create_result != 0) {
        ERROR_LOG("Failed to create thread: %d", create_result);
        free(thread_data_ptr);
        return false;
    }
    
    DEBUG_LOG("Thread started successfully");
    return true;
}

