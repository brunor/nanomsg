/*
    Copyright (c) 2012-2013 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#if !defined NN_HAVE_WINDOWS

#include "cipc.h"
#include "sipc.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#include <string.h>
#include <unistd.h>
#include <sys/un.h>

#define NN_CIPC_STATE_IDLE 1
#define NN_CIPC_STATE_CONNECTING 2
#define NN_CIPC_STATE_STOPPING_USOCK 3
#define NN_CIPC_STATE_WAITING 4
#define NN_CIPC_STATE_STOPPING_TIMER 5
#define NN_CIPC_STATE_STOPPING_USOCK_FINAL 6
#define NN_CIPC_STATE_STOPPING_TIMER_FINAL 7
#define NN_CIPC_STATE_ACTIVE 8

#define NN_CIPC_EVENT_CONNECTING 1
#define NN_CIPC_EVENT_STOP 2

struct nn_cipc {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct nn_epbase epbase;

    /*  The underlying IPC socket. */
    struct nn_usock usock;

    /*  Used to wait before retrying to connect. */
    struct nn_backoff retry;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct nn_sipc sipc;
};

/*  nn_epbase virtual interface implementation. */
static void nn_cipc_stop (struct nn_epbase *self);
static void nn_cipc_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_cipc_epbase_vfptr = {
    nn_cipc_stop,
    nn_cipc_destroy
};

/*  Private functions. */
static void nn_cipc_handler (struct nn_fsm *self, void *source, int type);
static void nn_cipc_enter_connecting (struct nn_cipc *self);

int nn_cipc_create (void *hint, struct nn_epbase **epbase)
{
    struct nn_cipc *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_cipc), "cipc");
    alloc_assert (self);

    /*  Initialise the structure. */
    nn_epbase_init (&self->epbase, &nn_cipc_epbase_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_cipc_handler,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_CIPC_STATE_IDLE;
    nn_usock_init (&self->usock, &self->fsm);
    nn_backoff_init (&self->retry, 1000, 1000, &self->fsm);
    nn_sipc_init (&self->sipc, &self->epbase, &self->fsm);

    /*  Start the state machine. */
    nn_cipc_enter_connecting (self);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_cipc_stop (struct nn_epbase *self)
{
    struct nn_cipc *cipc;

    cipc = nn_cont (self, struct nn_cipc, epbase);

    nn_cipc_handler (&cipc->fsm, NULL, NN_CIPC_EVENT_STOP);
}

static void nn_cipc_destroy (struct nn_epbase *self)
{
    struct nn_cipc *cipc;

    cipc = nn_cont (self, struct nn_cipc, epbase);

    nn_sipc_term (&cipc->sipc);
    nn_backoff_term (&cipc->retry);
    nn_usock_term (&cipc->usock);
    nn_fsm_term (&cipc->fsm);
    nn_epbase_term (&cipc->epbase);

    nn_free (cipc);
}

static void nn_cipc_handler (struct nn_fsm *self, void *source, int type)
{
    struct nn_cipc *cipc;

    cipc = nn_cont (self, struct nn_cipc, fsm);

    switch (cipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_CIPC_STATE_IDLE:
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_CONNECTING:
                cipc->state = NN_CIPC_STATE_CONNECTING;
                return;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  CONNECTING state.                                                         */
/******************************************************************************/
    case NN_CIPC_STATE_CONNECTING:
        if (source == &cipc->usock) {
            switch (type) {
            case NN_USOCK_CONNECTED:
                nn_sipc_start (&cipc->sipc, &cipc->usock);
                cipc->state = NN_CIPC_STATE_ACTIVE;
                return;
            case NN_USOCK_ERROR:
                nn_usock_stop (&cipc->usock);
                cipc->state = NN_CIPC_STATE_STOPPING_USOCK;
                return;
            default:
                nn_assert (0);
            }
        }
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_STOP:
                nn_usock_stop (&cipc->usock);
                cipc->state = NN_CIPC_STATE_STOPPING_USOCK_FINAL;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/*****************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/******************************************************************************/
    case NN_CIPC_STATE_STOPPING_USOCK:
        if (source == &cipc->usock) {
            switch (type) {
            case NN_USOCK_STOPPED:
                nn_backoff_start (&cipc->retry);
                cipc->state = NN_CIPC_STATE_WAITING;
                return;
            default:
                nn_assert (0);
            }
        }
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_STOP:
                cipc->state = NN_CIPC_STATE_STOPPING_USOCK_FINAL;
                return;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we want exhaust       */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case NN_CIPC_STATE_WAITING:
        if (source == &cipc->retry) {
            switch (type) {
            case NN_TIMER_TIMEOUT:
                nn_backoff_stop (&cipc->retry);
                cipc->state = NN_CIPC_STATE_STOPPING_TIMER;
                return;
            default:
                nn_assert (0);
            }
        }
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_STOP:
                nn_backoff_stop (&cipc->retry);
                cipc->state = NN_CIPC_STATE_STOPPING_TIMER_FINAL;
                return;

            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  STOPPING_TIMER state.                                                     */
/*  Waiting before re-connecting is done. We are now cancelling the re-try    */
/*  timer.                                                                    */
/******************************************************************************/
    case NN_CIPC_STATE_STOPPING_TIMER:
        if (source == &cipc->retry) {
            switch (type) {
            case NN_BACKOFF_STOPPED:
                nn_cipc_enter_connecting (cipc);
                return;
            default:
                nn_assert (0);
            }
        }
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_STOP:
                cipc->state = NN_CIPC_STATE_STOPPING_TIMER_FINAL;
                return;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  STOPPING_USOCK_FINAL state.                                               */
/*  Stopping the timer before cipc object itself is stopped.                  */
/******************************************************************************/
    case NN_CIPC_STATE_STOPPING_USOCK_FINAL:
        if (source == &cipc->usock) {
            switch (type) {
            case NN_USOCK_STOPPED:
                cipc->state = NN_CIPC_STATE_IDLE;
                nn_epbase_stopped (&cipc->epbase);
                return;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  STOPPING_TIMER_FINAL state.                                               */
/*  Stopping the timer before cipc object itself is stopped.                  */
/******************************************************************************/
    case NN_CIPC_STATE_STOPPING_TIMER_FINAL:
        if (source == &cipc->retry) {
            switch (type) {
            case NN_BACKOFF_STOPPED:
                cipc->state = NN_CIPC_STATE_IDLE;
                nn_epbase_stopped (&cipc->epbase);
                return;
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the sipc state machine.          */
/******************************************************************************/
    case NN_CIPC_STATE_ACTIVE:
        if (source == &cipc->sipc) {
            switch (type) {
            case NN_SIPC_ERROR:
                nn_usock_stop (&cipc->usock);
                cipc->state = NN_CIPC_STATE_STOPPING_USOCK;
                return;
            default:
                nn_assert (0);
            }
        }
        if (source == NULL) {
            switch (type) {
            case NN_CIPC_EVENT_STOP:
                nn_assert (0);
            default:
                nn_assert (0);
            }
        }
        nn_assert (0);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_assert (0);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void nn_cipc_enter_connecting (struct nn_cipc *self)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;

    /*  Try to start the underlying socket. */
    rc = nn_usock_start (&self->usock, AF_UNIX, SOCK_STREAM, 0);
    if (nn_slow (rc < 0)) {
        nn_backoff_start (&self->retry);
        self->state = NN_CIPC_STATE_WAITING;
        return;
    }

    /*  Create the IPC address from the address string. */
    addr = nn_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    nn_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

    /*  Start connecting. */
    nn_usock_connect (&self->usock, (struct sockaddr*) &ss,
        sizeof (struct sockaddr_un));
    self->state = NN_CIPC_STATE_CONNECTING;
}

#endif
