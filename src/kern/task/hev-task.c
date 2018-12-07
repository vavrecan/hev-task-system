/*
 ============================================================================
 Name        : hev-task.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 everyone.
 Description :
 ============================================================================
 */

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>

#include "hev-task.h"
#include "hev-task-private.h"
#include "kern/core/hev-task-system-private.h"
#include "mm/api/hev-memory-allocator-api.h"

#define STACK_OVERFLOW_DETECTION_TAG (0xdeadbeefu)

#define HEV_TASK_STACK_SIZE (64 * 1024)

#define ALIGN_DOWN(addr, align) ((addr) & ~((typeof(addr))align - 1))

HevTask *
hev_task_new (int stack_size)
{
    HevTask *self;
    uintptr_t stack_addr;

    self = hev_malloc0 (sizeof (HevTask));
    if (!self)
        return NULL;

    self->ref_count = 1;
    self->next_priority = HEV_TASK_PRIORITY_DEFAULT;

    if (stack_size == -1)
        stack_size = HEV_TASK_STACK_SIZE;

    self->stack = hev_malloc (stack_size);
    if (!self->stack) {
        hev_free (self);
        return NULL;
    }
#ifdef ENABLE_STACK_OVERFLOW_DETECTION
    *(unsigned int *)self->stack = STACK_OVERFLOW_DETECTION_TAG;
#endif

    stack_addr = (uintptr_t) (self->stack + stack_size);
    self->stack_top = (void *)ALIGN_DOWN (stack_addr, 16);
    self->stack_size = stack_size;
    self->sched_entity.task = self;

    return self;
}

HevTask *
hev_task_ref (HevTask *self)
{
    self->ref_count++;

    return self;
}

void
hev_task_unref (HevTask *self)
{
    self->ref_count--;
    if (self->ref_count)
        return;

#ifdef ENABLE_STACK_OVERFLOW_DETECTION
    assert (*(unsigned int *)self->stack == STACK_OVERFLOW_DETECTION_TAG);
#endif
    hev_free (self->stack);
    hev_free (self);
}

HevTask *
hev_task_self (void)
{
    return hev_task_system_get_context ()->current_task;
}

HevTaskState
hev_task_get_state (HevTask *self)
{
    return self->state;
}

void
hev_task_set_priority (HevTask *self, int priority)
{
    if (priority < HEV_TASK_PRIORITY_MIN)
        priority = HEV_TASK_PRIORITY_MIN;
    else if (priority > HEV_TASK_PRIORITY_MAX)
        priority = HEV_TASK_PRIORITY_MAX;

    self->next_priority = priority;
}

int
hev_task_get_priority (HevTask *self)
{
    return self->next_priority;
}

static inline unsigned int
hev_task_fd_events_to_reactor (unsigned int events)
{
    HevTaskIOReactorEvents reactor_events = 0;

    if (events & POLLIN)
        reactor_events |= HEV_TASK_IO_REACTOR_EV_RO;
    if (events & POLLOUT)
        reactor_events |= HEV_TASK_IO_REACTOR_EV_WO;
    if (events & POLLERR)
        reactor_events |= HEV_TASK_IO_REACTOR_EV_ER;

    return reactor_events;
}

int
hev_task_add_fd (HevTask *self, int fd, unsigned int events)
{
    HevTaskIOReactor *reactor;
    HevTaskIOReactorSetupEvent event;

    reactor = hev_task_system_get_context ()->reactor;
    hev_task_io_reactor_setup_event_set (&event, fd, HEV_TASK_IO_REACTOR_OP_ADD,
                                         hev_task_fd_events_to_reactor (events),
                                         &self->sched_entity);
    return hev_task_io_reactor_setup (reactor, &event, 1);
}

int
hev_task_mod_fd (HevTask *self, int fd, unsigned int events)
{
    HevTaskIOReactor *reactor;
    HevTaskIOReactorSetupEvent event;

    reactor = hev_task_system_get_context ()->reactor;
    hev_task_io_reactor_setup_event_set (&event, fd, HEV_TASK_IO_REACTOR_OP_MOD,
                                         hev_task_fd_events_to_reactor (events),
                                         &self->sched_entity);
    return hev_task_io_reactor_setup (reactor, &event, 1);
}

int
hev_task_del_fd (HevTask *self, int fd)
{
    HevTaskIOReactor *reactor;
    HevTaskIOReactorSetupEvent event;

    reactor = hev_task_system_get_context ()->reactor;
    hev_task_io_reactor_setup_event_set (&event, fd, HEV_TASK_IO_REACTOR_OP_DEL,
                                         0, NULL);
    return hev_task_io_reactor_setup (reactor, &event, 1);
}

int
hev_task_res_fd (HevTask *self, int fd, unsigned int events)
{
    if (!hev_task_io_reactor_get_oneshot ())
        return 0;

    return hev_task_mod_fd (self, fd, events);
}

void
hev_task_wakeup (HevTask *task)
{
    hev_task_system_wakeup_task (task);
}

void
hev_task_yield (HevTaskYieldType type)
{
    hev_task_system_schedule (type);
}

unsigned int
hev_task_sleep (unsigned int milliseconds)
{
    return hev_task_usleep (milliseconds * 1000) / 1000;
}

unsigned int
hev_task_usleep (unsigned int microseconds)
{
    HevTaskSystemContext *ctx;

    if (microseconds == 0)
        return 0;

    ctx = hev_task_system_get_context ();
    return hev_task_timer_wait (ctx->timer, microseconds, ctx->current_task);
}

void
hev_task_run (HevTask *self, HevTaskEntry entry, void *data)
{
    /* Skip to run task that already running */
    if (self->state != HEV_TASK_STOPPED)
        return;

    self->entry = entry;
    self->data = data;
    self->priority = self->next_priority;
    self->sched_key = self->next_priority;

    hev_task_system_run_new_task (self);
}

void
hev_task_exit (void)
{
    hev_task_system_kill_current_task ();
}

void *
hev_task_get_data (HevTask *self)
{
    return self->data;
}
