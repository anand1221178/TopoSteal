#include "deque.h"

void deque_init(deque_t *q)
{

    /* INitlaise with the atomics */
    atomic_init(&q->top, 0);
    atomic_init(&q->bottom,0);
    pthread_mutex_init(&q->bottom_lock, NULL);
}

void deque_push(deque_t *q, task_t t)
{
    /* See bottom_lock comment in deque.h: this serializes against a
     * concurrent pop (including from a non-owner thread, as
     * toposteal_submit does) - without it the two racing read-modify-write
     * updates to `bottom` can silently drop a task. */
    pthread_mutex_lock(&q->bottom_lock);

    /* Read the bottom index*/
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);

    /* Store the taks */
    /* Put the task t into the array at that index */
    q->tasks[b % DEQUE_MAX_TASKS] = t;

    /* Publish the new bottom  - increment the bottom to b + 1 and release it to memory so other threads can see it*/
    atomic_store_explicit(&q->bottom, b+1, memory_order_release);

    pthread_mutex_unlock(&q->bottom_lock);
}


/* Since th eowner pops from the bottom, it doesnt have to worry about the Theieves, but if there is one item left there can be an issue!
 *
 * bottom_lock (see deque.h) serializes this against a concurrent push -
 * needed because toposteal_submit() pushes from a non-owner thread, which
 * without this lock can race with an owner's pop on the same `bottom`
 * index and silently lose a task. Steal (top-side) is unaffected/still
 * lock-free. */
int deque_pop(deque_t *q, task_t *t_out)
{
    pthread_mutex_lock(&q->bottom_lock);

    /* Shrink the deque claim the bottom */
    //Read the bottom
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);

    // Empty check before decrementing — prevents unsigned underflow
    size_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    if (b <= t) { pthread_mutex_unlock(&q->bottom_lock); return 0; }

    b = b - 1;
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);

    //We need to make sure the cpu weite the new bottom to RAM before twe check the top
    atomic_thread_fence(memory_order_seq_cst);

    /* Now we can read the top */
    //Check if the theives have taken anything
    t = atomic_load_explicit(&q->top, memory_order_relaxed);

    /* Check the state of the deque -> we have 3 scenariaos here: */
    //1 dequeu was empty
    if(b< t)
    {
        atomic_store_explicit(&q->bottom, t, memory_order_relaxed);
        pthread_mutex_unlock(&q->bottom_lock);
        return 0;
    }

    //If we make it past the above function, the taks is either ours or its a tie, so we copy it
    *t_out = q->tasks[b % DEQUE_MAX_TASKS];

    //Now if there is no race confditions:
    if(b > t)
    {
        //success
        pthread_mutex_unlock(&q->bottom_lock);
        return 1;
    }

    /* Case of a race condition! */
    if (b ==t) // we have exactly one item left!
    {
        //Mark the deque as empty for the furutre
        atomic_store_explicit(&q->bottom, t+1, memory_order_relaxed);

        //Do the CAS
        if (atomic_compare_exchange_strong_explicit(&q->top,
            &t, //value we expect
            t+1, // new value if we win
            memory_order_seq_cst,  // Order if we win (global)
            memory_order_relaxed)) //Order if we lose
        {
            pthread_mutex_unlock(&q->bottom_lock);
            return 1;
        }
        pthread_mutex_unlock(&q->bottom_lock);
        return 0;
    }

    //Final fallback
    pthread_mutex_unlock(&q->bottom_lock);
    return 0;
}

size_t deque_size(deque_t *q)
{
    /* Heuristic snapshot only - see comment in deque.h */
    size_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);
    size_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    return (b > t) ? (b - t) : 0;
}

int deque_steal(deque_t *q, task_t *t_out)
{
    /* First we red the indexes to acquire */
    size_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    //have to use an acquire fence here to sync mem wit the owner
    atomic_thread_fence(memory_order_seq_cst);
    //read bottom
    size_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);

    
    /* Empty check */
    if(b <= t)
    {
        return 0;
    }

    //If we get here it is not exmpty so we grab!= since we are srealing we have to use the top!
    *t_out = q->tasks[t % DEQUE_MAX_TASKS];

    /* Cas check for race condiftions */
    if (atomic_compare_exchange_strong_explicit(&q->top,
            &t, //value we expect
            t+1, // new value if we win
            memory_order_seq_cst,  // Order if we win (global)
            memory_order_relaxed)) //Order if we lose
    {
        return 1;
    }
        return 0;
}