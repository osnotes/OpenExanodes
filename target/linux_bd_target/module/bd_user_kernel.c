/*
 * Copyright 2002, 2010 Seanodes Ltd http://www.seanodes.com. All rights
 * reserved and protected by French, UK, U.S. and other countries' copyright laws.
 * This file is part of Exanodes project and is subject to the terms
 * and conditions defined in the LICENSE file which is present in the root
 * directory of the project.
 */
#include "target/linux_bd_target/module/bd_user_kernel.h"

#include "target/linux_bd_target/module/bd_user_bd.h"

#include "target/linux_bd_target/module/bd_log.h"

#include "os/include/os_assert.h"

#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

/* if we wait for a new ack request for more than CLIENT_TIMEOUT secondes, i
 * stop to wait */
#define CLIENT_TIMEOUT 20 * HZ /* 20 seconds */

/* This file have all Event management, queue management function and
 * the core of BdAcqThread that map new request, end completed request
 * add new minor, change the status and remove other
 */

/*
 * bd_kernel_queue are chaine, element 0 and 1 are reserved to chaining request
 *	0 : free element*/
#define BD_FREE_QUEUE 0

#define NUM_MAPPED_PAGES(session) \
    (((session)->bd_max_queue * (session)->bd_buffer_size) / PAGE_SIZE)

/* BdEvent machanism us use to have processing consumption
 * and less deadlock, no more semaphore out of bound, the main idea is :
 * The event "receiver" test and process all possibely cause of event at each event,
 * so it is not needed to add multiple if N event is pendeing ONLY ONE MORE event need
 * to be received by the caller.
 */

static void bd_timedout(unsigned long arg)
{
    struct bd_event *bd_event = (struct bd_event *) arg;

    bd_new_event(bd_event, BD_EVENT_TIMEOUT);
}


/**
 * Wait for a new event, down on a semaphore if needed, exit if the Event was closed
 * If we wait motr than 15s, we will recieving a BD_EVENT_TIMEOUT
 * @param bd_event event structure that will be used
 * @param[out] BdType type of received event if the pointer is not null
 * @param[out] BdEventMsg pointer of the first "long" message received
 *
 * return 0 when all was ok; an event arrived.
 *        1 when interrupted (alias for EINTR ...)
 *        2 when exiting (cancelling events ?)
 *        3 when ???
 */
int bd_wait_event(struct bd_event *bd_event, unsigned long *bd_type,
                  struct bd_event_msg **bd_event_msg)
{
    unsigned long flags;
    bool wait;
    int int_val = 0;
    struct bd_event_msg *old_bd_event_msg = NULL;
    struct bd_event_msg *bd_event_msg_temp = NULL;

    OS_ASSERT(bd_type != NULL);
    *bd_type = 0;

    if (bd_event_msg != NULL)
        *bd_event_msg = NULL;

    do
    {
        wait = false;
        spin_lock_irqsave(&bd_event->bd_event_sl, flags);
        if (bd_event->bd_event_another != 2)
        {
            bd_event->bd_event_number++;
            old_bd_event_msg = bd_event->bd_old_msg;
            bd_event->bd_old_msg = NULL;
        }

        switch (bd_event->bd_event_another)
        {
        case 0:
            wait = true;
            break;

        case 1:
            bd_event->bd_event_another = 0;
            *bd_type = bd_event->bd_type;

            bd_event->bd_type = 0;
            if (bd_event_msg != NULL)
            {
                *bd_event_msg = bd_event->bd_msg;
                bd_event->bd_old_msg = bd_event->bd_msg;
                bd_event->bd_msg = NULL;
            }
            else if (bd_event->bd_msg != NULL)
                int_val = 3; /* BdMsg was not get, so we have messages lose ! */

            bd_event->bd_event_number++;
            break;

        case 2:
            int_val = 2;
            break;

        default:
            OS_ASSERT_VERBOSE(false, "ExaBd: Invalid value %d", bd_event->bd_event_another);
            break;
        }

        bd_event->bd_event_waiting = wait ? 1 : 0;

        spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);

        while (old_bd_event_msg != NULL)
        {
            bd_event_msg_temp = old_bd_event_msg->next; /* we use BdEventMsgTemp because OldBdEventMsg can be cleared after up ! */
            complete(&old_bd_event_msg->bd_event_completion);
            old_bd_event_msg = bd_event_msg_temp;
        }
        if (wait)
        {
            struct timer_list timeout;
            init_timer(&timeout);
            timeout.function = bd_timedout;
            timeout.data = (unsigned long) bd_event;
            timeout.expires = jiffies + CLIENT_TIMEOUT; /* seconds timer */
            add_timer(&timeout);
            if (down_interruptible(&bd_event->bd_event_sem) != 0)
            {
                int_val = 1;
                *bd_type = 0;

                if (bd_event_msg != NULL)
                    *bd_event_msg = NULL;

                /* down failed but if in a short time someone else add an
                 * event, he will perhaps do a up, so we must check this and
                 * say we don't wait more */
                spin_lock_irqsave(&bd_event->bd_event_sl, flags);

                wait = bd_event->bd_event_another == 1;
                bd_event->bd_event_waiting = 0;

                spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);

                /* another process add an event in the short time so down it !
                 * we don't read this event now, but it will be read in the next
                 * call of this function*/
                if (wait)
                    down(&bd_event->bd_event_sem);

                wait = false;
            }
            del_timer_sync(&timeout);
        }
    } while (wait);

    return int_val;
}


/** Add new event of type BdType wake up thread only if needed
 * @param bd_event event structure that will be used
 * @param[in] BdType type of event we send
 */
void bd_new_event(struct bd_event *bd_event, unsigned long bd_type)
{
    unsigned long flags;
    bool wait = false;

    spin_lock_irqsave(&bd_event->bd_event_sl, flags);

    if (bd_event->bd_event_another == 2)
    {
        spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);
        return;
    }

    bd_event->bd_type = bd_event->bd_type | bd_type;

    wait = bd_event->bd_event_waiting == 1 && bd_event->bd_event_another == 0;

    if (bd_event->bd_event_another == 0)
        bd_event->bd_event_another = 1;

    spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);

    if (wait)
        up(&bd_event->bd_event_sem);
}


/**
 * Add a message with waiting for it's have been received and processed by Event receiver
 * @param bd_event event structure that will be used
 * @param[in] Msg message we sent
 */
void bd_new_event_msg_wait_processed(struct bd_event *bd_event,
                                     struct bd_event_msg *msg)
{
    unsigned long flags;
    bool wait;

    if (msg == NULL)
        return;

    init_completion(&msg->bd_event_completion);

    spin_lock_irqsave(&bd_event->bd_event_sl, flags);

    if (bd_event->bd_event_another == 2)
    {
        spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);
        msg->bd_result = -1;
        return;
    }

    bd_event->bd_type = bd_event->bd_type | msg->bd_type;

    wait = bd_event->bd_event_waiting == 1 && bd_event->bd_event_another == 0;

    bd_event->bd_event_another = 1;
    msg->next = bd_event->bd_msg;
    bd_event->bd_msg = msg;

    spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);

    if (wait)
        up(&bd_event->bd_event_sem);

    wait_for_completion(&msg->bd_event_completion);
}


/** init an event structure
 * @param[out] BdEvent new event structure
 * @param[in] Name name of the event
 */
static void bd_event_init(struct bd_event *bd_event, char *name)
{
    bd_event->bd_event_waiting = 0;     /* no BdWaitEvent() waiting for an event */
    bd_event->bd_event_another = 0;     /* no other event posted */
    bd_event->bd_event_number = 0;      /* next event loop will be the first, only debug data */
    bd_event->bd_type = 0;
    bd_event->bd_msg = NULL;
    bd_event->bd_old_msg = NULL;
    spin_lock_init(&bd_event->bd_event_sl);
    sema_init(&bd_event->bd_event_sem, 0);
}


/** wake up all function waiting for an event and force all other to abort now
 * and in future
 * @param bd_eventevent structure
 */
static void bd_event_close(struct bd_event *bd_event)
{
    unsigned long flags;
    bool wait = false;
    struct bd_event_msg *temp;

    spin_lock_irqsave(&bd_event->bd_event_sl, flags);
    if (bd_event->bd_event_waiting == 1 && bd_event->bd_event_another == 0)
        wait = true;

    bd_event->bd_event_another = 2;
    spin_unlock_irqrestore(&bd_event->bd_event_sl, flags);
    while (bd_event->bd_msg != NULL)    /* end all waiting processed function */
    {
        temp = bd_event->bd_msg->next;
        complete(&bd_event->bd_msg->bd_event_completion);
        bd_event->bd_msg = temp;
    }
    while (bd_event->bd_old_msg != NULL) /* end all waiting processed function */
    {
        temp = bd_event->bd_old_msg->next;
        complete(&bd_event->bd_old_msg->bd_event_completion);
        bd_event->bd_old_msg = temp;
    }

    if (wait)
        up(&bd_event->bd_event_sem);    /* perhaps one waiting for event */
}

/*
 * bd_post_new_rq and BdAckRq are the two central function.
 *
 * There are no spin_lock in this file, because all was done in one thread,
 * and other task tell it to do something with a up to BdAcqRq.
 *
 * Only BdKillSession are called asynch.
 */

/* return -1 if queue is full */

/**
 * This function get a request, try allow Rq to handle a request it
 * and map the buffer of the request in user space
 * WARNING WARNING WARNING
 * FREE queue only modify BdPrev, so even if used is in request falled in
 * BD_FREE_QUEUE, it can continue to follow BdNext to the right queue in
 * bd_post_new_rq and bd_rq Remove THE ORDER OF OPERATION ARE VERY IMPORTANT
 * TO ASSUME USER AND KERNEL OPERATION ARE DONE RIGHT
 *
 * @param session   target session bd_post_new_rq() will try to add
 *                  session->pending_req with session->pending_info
 * @return 0   request was added to the bd_kernel_queue and mapped,
 *        -1   request cannot be added to bd_kernel_queue and mapped, so
 *             nothing was done
 */
long bd_post_new_rq(struct bd_session *session)
{
    struct bd_kernel_queue *bd_kq = session->bd_kernel_queue;
    int next;

    /* Search for a new free queue */
    next = bd_kq[BD_FREE_QUEUE].next; /* First search bd_kernel_queue free */

    if (next == BD_FREE_QUEUE)
        return -1; /* no rq free, so do nothing */

    bd_kq[next].bd_req = session->pending_req;
    session->bd_user_queue[next].bd_info = session->pending_info;

    /* will set BdBlkSize BdBlkNum */
    if (bd_prepare_request(&bd_kq[next]) == -1)
        return -1; /* request canot be prepared, there are no problem because
                    * the request not come out from free queue */

    bd_kq[BD_FREE_QUEUE].next = bd_kq[next].next;

    bd_kq[next].bd_use = BDUSE_USED;

    mb();

    session->bd_new_request.next_elt[*session->bd_new_request.last_index_add] = next;
    *session->bd_new_request.last_index_add =
        (*session->bd_new_request.last_index_add + 1) % session->bd_max_queue;

    /* one new request, so up the semaphore to call the user process */
    session->bd_in_rq++;
    bd_new_event(&session->bd_new_rq, BD_EVENT_ACK_NEW);
    return 0;
}


/**
 * Remove a request from the KernelQueue (also mapped in userspace) and end it
 * @param req_num index of the request to end in the bd_kernel_queue
 * @param session target session
 */
static void bd_rq_remove(int req_num, struct bd_session *session)
{
    struct bd_kernel_queue *bd_kq = session->bd_kernel_queue;

    session->bd_in_rq--;
    bd_end_request(&bd_kq[req_num], session->bd_user_queue[req_num].bd_result);

    bd_kq[req_num].bd_use = BDUSE_FREE;

    /* Add it in free queue, BdNext don't used */
    bd_kq[req_num].next = bd_kq[BD_FREE_QUEUE].next;
    bd_kq[BD_FREE_QUEUE].next = req_num;
}


/**
 * Finded all new ended request from the user and ending it in kernel mode
 *
 * The user application can be bad and can do bad thing, so
 * this function must not corrupt kernel even if user data are bad
 *
 * However, this function try to ack all completed request and add it to the
 * bd_kernel_queue (0) free kernel queue
 *
 * This function return 0 if nothing was done
 * @param session target session
 */
static void bd_ack_rq(struct bd_session *session)
{
    struct bd_barrillet_queue *bd_ack_req = &session->bd_ack_request;
    int *lirpo = bd_ack_req->last_index_read_plus_one;

    /* start at 2, because 0 and 1 are dummy : 0 is for a list of free entry
     * and 1 for the list of used entry */
    while (bd_ack_req->next_elt[*lirpo] != -1)
    {
        int i = bd_ack_req->next_elt[*lirpo];
        bd_ack_req->next_elt[*lirpo] = -1;

        *lirpo = (*lirpo + 1) % session->bd_max_queue;
        mb();

        if (i == BD_FREE_QUEUE || i > session->bd_max_queue - 1)
            continue;

        if (session->bd_kernel_queue[i].bd_use == BDUSE_SUSPEND)
            continue; /* don't do anything with suspended request */

        if (session->bd_kernel_queue[i].bd_use == BDUSE_FREE)
            continue; /* error, probably receive a request after a disk was DOWNed */

        bd_rq_remove(i, session);       /* ack the request with the error given by the user */
        bd_flush_q(session, 0);     /* add in user space as lot as we can the pending request */
    }
}


/**
 *  used to end all pending request mapped in user mode
 * @param session target session
 * @param minor minor of the device of this session to kill
 */
static void bd_ack_all_pending(struct bd_session *session, int minor)
{
    int *next_elt = session->bd_new_request.next_elt;
    int i, j;

    /* first remove request that is in bd_new_request */
    i = *session->bd_new_request.last_index_read_plus_one;
    j = i;
    do
    {
        next_elt[j] = next_elt[i];
        if (next_elt[i] == -1
            || (session->bd_kernel_queue[next_elt[i]].bd_minor != minor && minor != -1))
            j = (j + 1) % session->bd_max_queue;

        i = (i + 1) % session->bd_max_queue;
    } while (i != *session->bd_new_request.last_index_read_plus_one);

    while (j != *session->bd_new_request.last_index_read_plus_one)
    {
        next_elt[j] = -1;
        j = (j + 1) % session->bd_max_queue;
    }

    for (i = BD_FREE_QUEUE + 1; i < session->bd_max_queue; i++)
    {
        if (session->bd_kernel_queue[i].bd_use != BDUSE_FREE)
            if (session->bd_kernel_queue[i].bd_minor == minor || minor == -1)
            {
                session->bd_user_queue[i].bd_result = -EIO;
                bd_rq_remove(i, session);
            }
    }
}

/* if nothing arrive in 20 mn and some request pending, dump !
 * FIXME What is the purpose of this dump ? Actually, the dump is supposed to
 * happen if IOs are blocked for more than TIMEOUT_BEFORE_DUMP_IN_SECOND but
 * is that really the role of exa_bd to decide in his own to make the node
 * crash ? */
#define TIMEOUT_BEFORE_DUMP_IN_SECOND (20 * 60)

#define TIMEOUT_BEFORE_DUMP (TIMEOUT_BEFORE_DUMP_IN_SECOND /\
                             (CLIENT_TIMEOUT / HZ))

/**
 * For a session, unmap all pending request in user mode and ack them with
 * an error
 *
 * @param session   session to abort
 */
static void abort_all_pending_requests(struct bd_session *session)
{
    struct bd_minor *bd_minor = session->bd_minor;

    bd_ack_all_pending(session, -1);
    bd_flush_q(session, -EIO);
    while (bd_minor != NULL)
    {
        if (!bd_minor->dead)
        {
            bd_minor->dead = true;
            bd_close_list(&bd_minor->bd_list);
            /* after Dead==1, all new req will be ack with error,
             * so ack with error all pending
             */
            bd_ack_all_pending(session, bd_minor->minor);
            /* Removing all minor associated with this dev */
            bd_minor_remove(bd_minor);
        }
        bd_minor = bd_minor->bd_next;
    }

    bd_event_close(&session->bd_new_rq);
}

/**
 * One of this thread are created by Session, it handle all event send to BdThreadEvent
 * it exit when it reveiving BD_EVENT_KILL event
 *
 * @NOTE There was a legacy schedule in the bd_wait_event() call that was
 *       making this thread schedule 'sometimes'. After some investigations
 *       I found out that it was introduced by r24083 by XXXXXX XXXXXXX in
 *       he's usual commit of crap. As there is no more explanation in svn
 *       and no documentation about it, I got convinced that it was useless
 *       and I removed it.
 *
 * @param arg session pointer
 *
 * @return 0 (kernel thread API requires a return int)
 */

static int bd_ack_rq_thread(void *arg)
{
    unsigned long type;
    struct bd_event_msg *msg;
    struct bd_session *session = arg;
    int i;
    int nb_timeout = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
    /* The name of the thread of exa_bd in processes list will be of the form:
     * exa_bdev_<major> */
    daemonize("exa_bdev_%lld", session->bd_major);
#endif

    siginitsetinv(&current->blocked, sigmask(SIGKILL));
    do
    {
        int int_val = bd_wait_event(&session->bd_thread_event, &type, &msg);
        if (type != BD_EVENT_TIMEOUT || int_val == 1 || session->bd_in_rq == 0)
            nb_timeout = 0;
        else
            nb_timeout++;

        if ((nb_timeout == TIMEOUT_BEFORE_DUMP && session->bd_in_rq > 0)
            || int_val == 1)
        {
            type = BD_EVENT_ACK_NEW;

            bd_log_error("Core dumping \n");

            abort_all_pending_requests(session);

            {
                bd_log_error("exa_bd have detected a potentiel freeze\n"
                             "exa_bd stop all volumes\n"
                             "exa_bd sending SIGABRT to process %s\n",
                             session->bd_task->comm);
                {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
                    kill_proc(session->bd_task->pid, SIGABRT, 0);
#else
                    send_sig(SIGABRT, session->bd_task, 0);
#endif
                }
            }
        }

        if (int_val == 1)
            flush_signals(current);     /* in case of SIGUP or another thing, we must ignore it, only a release can kill this thread */

        if ((type & BD_EVENT_KILL) != 0)
        {
            bd_log_info("BD_EVENT_KILL\n");

            abort_all_pending_requests(session);

            for (i = 0; i < NUM_MAPPED_PAGES(session); i++)
            {
                if (session->bd_unaligned_buf[i] == NULL)
                    break;
                put_page(session->bd_unaligned_buf[i]);
            }

            vfree(session->bd_unaligned_buf);

            vfree(session->bd_kernel_queue);
            vfree(session->bd_user_queue);
            /* we ending, so all event queue is down, this Close also signal to the BD_EVENT_KILL
               that the messaged have been proceed */
            bd_event_close(&session->bd_thread_event);
            complete_and_exit(&session->bd_end_completion, 0);
            /* now all is done, so exit, the caller must vfree(Session) */
            return 0;
        }

        if ((type & BD_EVENT_ACK_NEW) != 0)
        {
            bd_ack_rq(session); /* wait for something and ack all user finished request */
            bd_flush_q(session, 0);     /* add in user space as lot as we can the pending request */
        }

        while (msg != NULL)
        {
            struct bd_minor *bd_minor = NULL;
            msg->bd_result = 0;
            if (msg->bd_type != BD_EVENT_NEW)
            {
                msg->bd_result = -1;
                bd_minor = session->bd_minor;
                while (bd_minor != NULL)
                {
                    if (bd_minor->minor == msg->bd_minor && !bd_minor->dead)
                    {
                        msg->bd_result = 0;
                        break;
                    }
                    bd_minor = bd_minor->bd_next;
                }
            }

            if (msg->bd_result == -1)
            {
                msg = msg->next;
                continue;
            }

            switch (msg->bd_type)
            {
            case BD_EVENT_NEW:
                msg->bd_result = bd_minor_add_new(session, msg->bd_minor,
                                                  msg->bd_minor_size_in512_bytes,
                                                  msg->bd_minor_readonly);
                break;

            case BD_EVENT_SETSIZE:
                msg->bd_result = bd_minor_set_size(bd_minor,
                                                   msg->bd_minor_size_in512_bytes);
                break;

            case BD_EVENT_DEL:
                if (!bd_minor->dead)
                {
                    if (atomic_read(&bd_minor->use_count) == 1)
                    {
                        /* after Dead==true, all new req will be ack with
                         * error, so ack with error all pending */
                        bd_minor->dead = true;
                        bd_close_list(&bd_minor->bd_list);
                        bd_ack_all_pending(session, msg->bd_minor);
                        msg->bd_result = bd_minor_remove(bd_minor);
                    }
                    else
                    {
                        /* The block device is in use. */
                        msg->bd_result = -1;
                    }
                }
                break;

            case BD_EVENT_IS_INUSE:
		/* FIXME why > 1 ? */
                msg->bd_result = (atomic_read(&bd_minor->use_count) > 1);
                break;
            }

            msg = msg->next;
        }
    } while (1);
    return 0;                   /* to avoid warning */
}


static void bd_get_session(struct bd_session *session)
{
    atomic_inc(&session->total_use_count);
}


/**
 * This function Launch a session thread and initialise and allocated all neede data.
 *  Warning this must be called from the target user VM
 * @param major major number that will be associated with all device managed in this session
 * @return addresse of a session, or NULL if an error occur
 */
struct bd_session *bd_launch_session(struct bd_init *init)
{
    struct bd_session *session = vmalloc(sizeof(struct bd_session));
    int i;

    if (!session)
    {
        bd_log_error("Exanodes BD: Error: not enough memory for one Session\n");
        return NULL;
    }

    if (init->bd_max_queue < 4
        || (init->bd_buffer_size & (PAGE_SIZE - 1)) != 0
        || init->bd_buffer_size < PAGE_SIZE)
    {
        bd_log_error("Exanodes BD open session failed with BD_MAX_QUEUE %d "
                     "and BD_BUFFER_SIZE %d\n", init->bd_max_queue,
                     init->bd_buffer_size);
        vfree(session);
        return NULL;
    }

    memset(session, 0, sizeof(struct bd_session));

    session->bd_max_queue = init->bd_max_queue;
    session->bd_page_size = init->bd_max_queue * sizeof(struct bd_kernel_queue);
    if (session->bd_page_size < init->bd_max_queue *
        sizeof(struct bd_user_queue) +
        (init->bd_max_queue + 2) * 2 * sizeof(int))
    {
        session->bd_page_size = init->bd_max_queue *
                                sizeof(struct bd_user_queue) +
                                (init->bd_max_queue + 2) * 2 * sizeof(int);
    }

    /* If page_size not alligned, align it */
    if ((session->bd_page_size & (PAGE_SIZE - 1)) != 0)
    {
        session->bd_page_size = session->bd_page_size + PAGE_SIZE -
                                (session->bd_page_size & (PAGE_SIZE - 1));
    }
    session->bd_buffer_size = init->bd_buffer_size;
    session->bd_barrier_enable = init->bd_barrier_enable;
    atomic_set(&session->total_use_count, 0);
    session->bd_major = 0;
    session->bd_task = current;

    bd_event_init(&session->bd_new_rq, "NewRq");
    bd_event_init(&session->bd_thread_event, "ThreadEvent");

    session->bd_unaligned_buf = vmalloc(session->bd_max_queue
                                        * session->bd_buffer_size / PAGE_SIZE
                                        * sizeof(char *));
    if (!session->bd_unaligned_buf)
        goto error;

    /* adjust to a page boundaries */

    session->bd_kernel_queue = vmalloc(session->bd_page_size);
    if (session->bd_kernel_queue == NULL)
        goto error;

    memset(session->bd_kernel_queue, 0, session->bd_page_size);

    session->bd_user_queue = vmalloc(session->bd_page_size);
    if (session->bd_user_queue == NULL)
        goto error;

    memset(session->bd_user_queue, 0, session->bd_page_size);

/* bd_user_queue :
 * New request from kernel to user :
 * 0..n-1                           bd_user_queue[0..(session->bd_max_queue-1)
 * n                                                            session->bd_new_request.last_index_add
 * n+1                              session->bd_new_request.last_index_read_plus_one
 * n+2..n+2+session->bd_max_queue-1                             next_elt[0..session->bd_max_queue]
 *
 * New request ended from user to kernel
 * n+2+session->bd_max_queue                        session->bd_ack_request.last_index_add
 * n+2+session->bd_max_queue+1                                  session->bd_ack_request.last_index_read_plus_one
 * n+2+session->bd_max_queue+2..
 *  n+2+session->bd_max_queue+2+session->bd_max_queue-1 session->bd_ack_request.next_elt
 *
 * each barillet have session->bd_max_queue element and there are a maximum of session->bd_max_queue-2 new elements
 * because there are only session->bd_max_queue in bd_user_queue and bd_kernel_queue maped in user and the element
 * 0 and 1 of these two queue is reserved.
 *
 * Algo to use barrillet : only one sender and only one receiver.
 * Adding :
 *   next_elt[last_index_add] = elt_to_send
 *   last_index_add++
 *
 * Removing
 *   elt_received = next_elt[last_read_plus_one]
 *   if elt_received == -1 then
 *        nothing received
 *   else
 *        received : elt_received
 *        next_elt[last_read_plus_one] = -1
 *        last_read_plus_one++
 */

    session->bd_new_request.last_index_add =
        (int *) &(session->bd_user_queue[session->bd_max_queue]);
    session->bd_new_request.last_index_read_plus_one =
        session->bd_new_request.last_index_add + 1;
    session->bd_new_request.next_elt = session->bd_new_request.last_index_add +
                                       2;
    session->bd_ack_request.last_index_add =
        session->bd_new_request.last_index_add + 2 + session->bd_max_queue;
    session->bd_ack_request.last_index_read_plus_one =
        session->bd_new_request.last_index_add + 2 + session->bd_max_queue + 1;
    session->bd_ack_request.next_elt = session->bd_new_request.last_index_add +
                                       2 + session->bd_max_queue + 2;

    *session->bd_new_request.last_index_read_plus_one = 0;
    *session->bd_ack_request.last_index_read_plus_one = 0,
    *session->bd_new_request.last_index_add = 0;
    *session->bd_ack_request.last_index_add = 0;

    for (i = 0; i < session->bd_max_queue; i++)
    {
        struct bd_kernel_queue *bd_kq = &session->bd_kernel_queue[i];
        bd_kq->bd_buf_user = init->buffer + session->bd_buffer_size * i;

        bd_kq->bd_use = BDUSE_FREE; /* not used */

        /* Used to create free list */
        bd_kq->next = i + 1;
        bd_kq->bd_session = session;

        session->bd_new_request.next_elt[i] = -1;
        session->bd_ack_request.next_elt[i] = -1;
    }

    /* Complete the creationg of the free list */
    session->bd_kernel_queue[BD_FREE_QUEUE].bd_use = BDUSE_FREE;
    session->bd_kernel_queue[BD_FREE_QUEUE].next = BD_FREE_QUEUE + 1;
    session->bd_kernel_queue[session->bd_max_queue - 1].next = BD_FREE_QUEUE;

    /* alloc buffer used to handle bio that don't fit one page */
    i = get_user_pages(current, current->mm, (unsigned long)init->buffer,
                       NUM_MAPPED_PAGES(session), 1, 0,
                       &session->bd_unaligned_buf[0], NULL);

    if (i < NUM_MAPPED_PAGES(session))
    {
        session->bd_unaligned_buf[i] = NULL;
        goto error;
    }

    if (bd_register_drv(session) != 0)
        goto error;

    init_completion(&session->bd_end_completion);

    /* Starting the BdAckThread */
    if (bd_init_root(2048, sizeof(struct bd_request), &session->bd_root) < 0)
        goto error;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0))
    if (kernel_thread(bd_ack_rq_thread, (void *) session, 0) <= 0)
#else
    if (IS_ERR(kthread_run(bd_ack_rq_thread, session, "exa_bdev_%ld", session->bd_major)))
#endif
    {
        bd_close_root(&session->bd_root);
        goto error;
    }

    bd_get_session(session);
    bd_log_info("Exanodes BD opened session with BD_MAX_QUEUE %d and "
                "BD_BUFFER_SIZE %d BD_PAGE_SIZE %ld\n",
                init->bd_max_queue, init->bd_buffer_size,
                session->bd_page_size);
    return session;
error:

    bd_log_error("Exanodes BD open session failed with BD_MAX_QUEUE %d and"
                 " BD_BUFFER_SIZE %d BD_PAGE_SIZE %d\n",
                 init->bd_max_queue, init->bd_buffer_size,
                 session->bd_page_size);

    bd_event_close(&session->bd_new_rq);
    bd_event_close(&session->bd_thread_event);

    if (session->bd_unaligned_buf)
    {
        for (i = 0; i < NUM_MAPPED_PAGES(session); i++)
        {
            if (session->bd_unaligned_buf[i] == NULL)
                break;

            put_page(session->bd_unaligned_buf[i]);
        }
        bd_log_error("Exanodes BD open session failed allocated only %d "
                     "buffer on %lu UnalignedBuf %p\n", i,
                     NUM_MAPPED_PAGES(session), session->bd_unaligned_buf);
        vfree(session->bd_unaligned_buf);
    }

    bd_log_error("Exanodes BD open session failed bd_user_queue %p "
                 "bd_kernel_queue %p\n", session->bd_user_queue,
                 session->bd_kernel_queue);

    if (session->bd_user_queue)
        vfree(session->bd_user_queue);

    if (session->bd_kernel_queue)
        vfree(session->bd_kernel_queue);

    vfree(session);
    return NULL;
}

void bd_put_session(struct bd_session **_session)
{
    struct bd_session *session = *_session;

    if (atomic_dec_and_test(&session->total_use_count))
    {
        struct bd_minor *bd_minor = session->bd_minor;

        while (bd_minor != NULL)
        {
            struct bd_minor *next = bd_minor->bd_next;
            kfree(bd_minor);
            bd_minor = next;
        }

        bd_unregister_drv(session);
        bd_close_root(&session->bd_root);
        vfree(session);
        *_session = NULL;
    }
}

