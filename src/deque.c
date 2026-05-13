#include "deque.h"

void deque_init(deque_t *q)
{

    /* INitlaise with the atomics */
    atomic_init(&q->top, 0);
    atomic_init(&q->bottom,0);
}

void deque_push(deque_t *q, task_t t)
{
    /* Read the bottom index*/
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);

    /* Store the taks */
    /* Put the task t into the array at that index */
    q->tasks[b % DEQUE_MAX_TASKS] = t;

    /* Publish the new bottom  - increment the bottom to b + 1 and release it to memory so other threads can see it*/
    atomic_store_explicit(&q->bottom, b+1, memory_order_release);
}


/* Since th eowner pops from the bottom, it doesnt have to worry about the Theieves, but if there is one item left there can be an issue! */
int deque_pop(deque_t *q, task_t *t_out)
{
    /* Shrink the deque claim the bottom */
    //Read the bottom
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);

    //We need to make sure the cpu weite the new bottom to RAM before twe check the top
    atomic_thread_fence(memory_order_seq_cst);

    /* Now we can read the top */
    //Check if the theives have taken anything
    size_t t = atomic_load_explicit(&q->top, memory_order_relaxed);

    /* Check the state of the deque -> we have 3 scenariaos here: */
    //1 dequeu was empty
    if(b< t)
    {
        atomic_store_explicit(&q->bottom, t, memory_order_relaxed);
        return 0; 
    }
    
    //If we make it past the above function, the taks is either ours or its a tie, so we copy it 
    *t_out = q->tasks[b % DEQUE_MAX_TASKS];

    //Now if there is no race confditions:
    if(b > t)
    {
        //success
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
            return 1;
        }
        return 0;
    }

    //Final fallback
    return 0;
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