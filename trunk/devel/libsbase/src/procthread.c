#include "procthread.h"
#include "queue.h"
#include "logger.h"
#include "message.h"
#include "evtimer.h"
#include "chunk.h"

/* run procthread */
void procthread_run(void *arg)
{
    PROCTHREAD *pth = (PROCTHREAD *)arg;

    if(pth)
    {
        DEBUG_LOGGER(pth->logger, "Ready for running thread[%08x]", pth->threadid);
        pth->running_status = 1;
        while(pth->running_status)
        {
            pth->evbase->loop(pth->evbase, 0, NULL);
            if(QTOTAL(pth->message_queue) > 0)
                message_handler(pth->message_queue, pth->logger);
            usleep(pth->usec_sleep);
        }
    }
#ifdef HAVE_PTHREAD
    pthread_exit(NULL);
#endif
}

/* add new task */
int procthread_newtask(PROCTHREAD *pth, CALLBACK *task_handler, void *arg)
{
    MESSAGE msg = {0};
    int ret = -1;

    if(pth && pth->message_queue && task_handler)
    {
        msg.msg_id      = MESSAGE_TASK;
        msg.parent      = pth;
        msg.index       = -1;
        msg.handler     = task_handler;
        msg.arg         = arg;
        if(pth->lock == 0){QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);}
        DEBUG_LOGGER(pth->logger, "Added message task to procthreads[%d]", pth->index);
        ret = 0;
    }
    return ret;
}

/* add new transaction */
int procthread_newtransaction(PROCTHREAD *pth, CONN *conn, int tid)
{
    MESSAGE msg = {0};
    int ret = -1;

    if(pth && pth->message_queue && conn)
    {
        msg.msg_id = MESSAGE_TRANSACTION;
        msg.index       = -1;
        msg.fd = conn->fd;
        msg.tid = tid;
        msg.handler = conn;
        msg.parent  = (void *)pth;
        if(pth->lock == 0){QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);}
        DEBUG_LOGGER(pth->logger, "Added message transaction[%d] to %s:%d on %s:%d via %d total %d",
                tid, conn->remote_ip, conn->remote_port, conn->local_ip, conn->local_port, 
                conn->fd, QTOTAL(pth->message_queue));
        ret = 0;
    }
    return ret;
}

/* Add connection message */
int procthread_addconn(PROCTHREAD *pth, CONN *conn)
{
    MESSAGE msg = {0};
    int ret = -1;

    if(pth && pth->message_queue && conn)
    {
        msg.msg_id      = MESSAGE_NEW_SESSION;
        msg.index       = -1;
        msg.fd          = conn->fd;
        msg.handler     = (void *)conn;
        msg.parent      = (void *)pth;
        if(pth->lock == 0){QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);}
        DEBUG_LOGGER(pth->logger, "Ready for adding msg[%s] connection[%s:%d] on "
                "%s:%d via %d total %d", MESSAGE_DESC(MESSAGE_NEW_SESSION),
                conn->remote_ip, conn->remote_port, conn->local_ip, conn->local_port,
                conn->fd, QTOTAL(pth->message_queue));
        ret = 0;
    }
    return ret;
}

/* Add new connection */
int procthread_add_connection(PROCTHREAD *pth, CONN *conn)
{
    int ret = -1;

    if(pth && conn)
    {
        DEBUG_LOGGER(pth->logger, "Ready for add connection[%s:%d] on %s:%d via %d to pool",
        conn->remote_ip, conn->remote_port, conn->local_ip, conn->local_port, conn->fd);
        conn->message_queue = pth->message_queue;
        conn->evbase        = pth->evbase;
        conn->parent        = pth;
        if(conn->set(conn) == 0)
        {
            DEBUG_LOGGER(pth->logger, "Ready for add connection[%s:%d] on %s:%d via %d to pool",
                conn->remote_ip, conn->remote_port, conn->local_ip, conn->local_port, conn->fd);
            ret = pth->service->pushconn(pth->service, conn);
        }
    }
    return ret;
}

/* Terminate connection */
int procthread_terminate_connection(PROCTHREAD *pth, CONN *conn)
{
    int ret = -1;

    if(pth && conn)
    {
        ret = conn->terminate(conn);
        ret = pth->service->popconn(pth->service, conn);
        if(pth->lock)
        {
            conn->clean(&conn);
        }
        else
        {
            QUEUE_PUSH(pth->service->connection_queue, PCONN, &conn);
        }
        //conn->reset(conn);
    }
    return ret;
}

/* stop procthread */
void procthread_stop(PROCTHREAD *pth)
{
    MESSAGE msg = {0};

    if(pth && pth->message_queue)
    {
        msg.msg_id      = MESSAGE_STOP;
        msg.index       = -1;
        msg.parent      = (void *)pth;
        QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);
        DEBUG_LOGGER(pth->logger, "Ready for stopping procthread[%d]", pth->index);
        pth->lock       = 1;
    }
    return ;
}

/* Terminate procthread */
void procthread_terminate(PROCTHREAD *pth)
{
    if(pth)
    {
        DEBUG_LOGGER(pth->logger, "Ready for closing procthread[%d]", pth->index);
        pth->running_status = 0;
    }
    return ;
}

/* state */
void procthread_state(PROCTHREAD *pth,  CALLBACK *handler, void *arg)
{
    MESSAGE msg = {0};

    if(pth && pth->message_queue)
    {
        msg.msg_id      = MESSAGE_STATE;
        msg.parent      = (void *)pth;
        msg.handler     = (void *)handler;
        msg.index       = -1;
        msg.arg         = (void *)arg;
        if(pth->lock == 0){QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);}
        //DEBUG_LOGGER(pth->logger, "Ready for state connections on daemon procthread");
    }
    return ;
}

/* active heartbeat */
void procthread_active_heartbeat(PROCTHREAD *pth,  CALLBACK *handler, void *arg)
{
    MESSAGE msg = {0};

    if(pth && pth->message_queue)
    {
        msg.msg_id      = MESSAGE_HEARTBEAT;
        msg.parent      = (void *)pth;
        msg.handler     = (void *)handler;
        msg.arg         = (void *)arg;
        msg.index       = -1;
         if(pth->lock == 0){QUEUE_PUSH(pth->message_queue, MESSAGE, &msg);}
        //DEBUG_LOGGER(pth->logger, "Ready for activing heartbeat on daemon procthread");
    }
    return ;

}

/* clean procthread */
void procthread_clean(PROCTHREAD **ppth)
{
    CHUNK *cp = NULL;

    if(ppth && *ppth)
    {
        if((*ppth)->service->working_mode != WORKING_PROC)
        {
            (*ppth)->evbase->clean(&((*ppth)->evbase));
            QUEUE_CLEAN((*ppth)->message_queue);
        }
        if((*ppth)->chunks_queue)
        {
            while(QUEUE_POP((*ppth)->chunks_queue, PCHUNK, &cp) == 0)
            {
                CK_CLEAN(cp);
                cp = NULL;
            }
            QUEUE_CLEAN((*ppth)->chunks_queue);
        }
        free((*ppth));
        (*ppth) = NULL;
    }
}

/* Initialize procthread */
PROCTHREAD *procthread_init()
{
    PROCTHREAD *pth = NULL;

    if((pth = (PROCTHREAD *)calloc(1, sizeof(PROCTHREAD))))
    {
        QUEUE_INIT(pth->message_queue);
        QUEUE_INIT(pth->chunks_queue);
        pth->evbase                  = evbase_init();
        pth->run                     = procthread_run;
        pth->addconn                 = procthread_addconn;
        pth->add_connection          = procthread_add_connection;
        pth->newtask                 = procthread_newtask;
        pth->newtransaction          = procthread_newtransaction;
        pth->terminate_connection    = procthread_terminate_connection;
        pth->stop                    = procthread_stop;
        pth->terminate               = procthread_terminate;
        pth->state                   = procthread_state;
        pth->active_heartbeat        = procthread_active_heartbeat;
        pth->clean                   = procthread_clean;
    }
    return pth;
}