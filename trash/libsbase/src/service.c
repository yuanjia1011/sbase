#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "sbase.h"
#include "xssl.h"
#include "logger.h"
#include "service.h"
#include "mutex.h"
#include "mmblock.h"
#include "message.h"
#include "evtimer.h"
#include "xmm.h"
#include "xqueue.h"
#ifndef UI
#define UI(_x_) ((unsigned int)(_x_))
#endif
#ifdef HAVE_SSL
#define SERVICE_CHECK_SSL_CLIENT(service)                                           \
do                                                                                  \
{                                                                                   \
    if(service->c_ctx == NULL)                                                      \
    {                                                                               \
        if((service->c_ctx = SSL_CTX_new(SSLv23_client_method())) == NULL)          \
        {                                                                           \
            ERR_print_errors_fp(stdout);                                            \
            _exit(-1);                                                              \
        }                                                                           \
    }                                                                               \
}while(0)
#else 
#define SERVICE_CHECK_SSL_CLIENT(service)
#endif
 
/* set service */
int service_set(SERVICE *service)
{
    int ret = -1, opt = 1, flag = 0;
    char *p = NULL;

    if(service)
    {
        p = service->ip;
        service->sa.sin_family = service->family;
        service->sa.sin_addr.s_addr = (p)? inet_addr(p):INADDR_ANY;
        service->sa.sin_port = htons(service->port);
        if(service->backlog <= 0) service->backlog = SB_CONN_MAX;
        SERVICE_CHECK_SSL_CLIENT(service);
        if(service->service_type == S_SERVICE)
        {
#ifdef HAVE_SSL
            if(service->is_use_SSL && service->cacert_file && service->privkey_file)
            {
                if((service->s_ctx = SSL_CTX_new(SSLv23_server_method())) == NULL)
                {
                    ERR_print_errors_fp(stdout);
                    _exit(-1);
                }
                /*load certificate */
                if(SSL_CTX_use_certificate_file(XSSL_CTX(service->s_ctx), service->cacert_file, 
                            SSL_FILETYPE_PEM) <= 0)
                {
                    ERR_print_errors_fp(stdout);
                    _exit(-1);
                }
                /*load private key file */
                if (SSL_CTX_use_PrivateKey_file(XSSL_CTX(service->s_ctx), service->privkey_file, 
                            SSL_FILETYPE_PEM) <= 0)
                {
                    ERR_print_errors_fp(stdout);
                    _exit(-1);
                }
                /*check private key file */
                if (!SSL_CTX_check_private_key(XSSL_CTX(service->s_ctx)))
                {
                    ERR_print_errors_fp(stdout);
                    exit(1);
                }
            }
#endif
            if((service->fd = socket(service->family, service->sock_type, 0)) > 0)
            {
                flag = fcntl(service->fd, F_GETFL, 0)|O_NONBLOCK;
                if(fcntl(service->fd, F_SETFL, flag) == 0
                        && setsockopt(service->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0
#ifdef SO_REUSEPORT
                        && setsockopt(service->fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0
#endif
                        && (ret = bind(service->fd, (struct sockaddr *)&(service->sa), 
                                sizeof(service->sa))) == 0)
                {
                    if(service->sock_type == SOCK_STREAM)
                        ret = listen(service->fd, service->backlog);
                }
            }
            else
            {
                fprintf(stderr, "new socket() failed, %s", strerror(errno));
                exit(1);
            }
        }
        else if(service->service_type == C_SERVICE)
        {
           ret = 0;
        }
    }
    return ret;
}

/* ignore SIGPIPE */
void sigpipe_ignore()
{
#ifndef WIN32
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGPIPE);
#ifdef HAVE_PTHREAD
    pthread_sigmask(SIG_BLOCK, &signal_mask, NULL);
#endif
#endif
    return ;
}

#ifdef HAVE_PTHREAD
#define NEW_PROCTHREAD(ns, id, pthid, pth, logger)                                          \
{                                                                                           \
    if(pthread_create((pthread_t *)&pthid, NULL, (void *)(pth->run), (void *)pth) == 0)     \
    {                                                                                       \
        DEBUG_LOGGER(logger, "Created %s[%d] ID[%p]", ns, id, (void*)((long)pthid));        \
    }                                                                                       \
    else                                                                                    \
    {                                                                                       \
        FATAL_LOGGER(logger, "Create %s[%d] failed, %s", ns, id, strerror(errno));          \
        exit(EXIT_FAILURE);                                                                 \
    }                                                                                       \
}
#else
#define NEW_PROCTHREAD(ns, id, pthid, pth, logger)
#endif
#ifdef HAVE_PTHREAD
#define PROCTHREAD_EXIT(id, exitid) pthread_join((pthread_t)id, exitid)
#else
#define PROCTHREAD_EXIT(id, exitid)
#endif
#define PROCTHREAD_SET(service, pth)                                                        \
{                                                                                           \
    pth->service = service;                                                                 \
    pth->logger = service->logger;                                                          \
    pth->usec_sleep = service->usec_sleep;                                                  \
    pth->use_cond_wait = service->use_cond_wait;                                            \
    pth->queue = service->queue;                                                  \
}

/* running */
int service_run(SERVICE *service)
{
    int ret = -1, i = 0, x = 0;
    char logfile[1024];
    CONN *conn = NULL;

    if(service)
    {
        //added to evtimer 
        if(service->heartbeat_interval > 0)
        {
            service->evid = EVTIMER_ADD(service->evtimer, service->heartbeat_interval, 
                    &service_evtimer_handler, (void *)service);
            DEBUG_LOGGER(service->logger, "Added service[%s] to evtimer[%p][%d] interval:%d",
                    service->service_name, service->evtimer, service->evid, 
                    service->heartbeat_interval);
        }
        //evbase setting 
        if(service->service_type == S_SERVICE && service->evbase)
        {
            event_set(&(service->event), service->fd, E_READ|E_PERSIST,
                    (void *)service, (void *)&service_event_handler);
            ret = service->evbase->add(service->evbase, &service->event);
        }
        //initliaze conns
        for(i = 0; i < service->init_conns; i++)
        {
            if((conn = conn_init()))
            {
                x = service->nqconns++;
                service->qconns[x] = conn;
                service->nconn++;
            }
            else break;
        }
        if(service->working_mode == WORKING_THREAD)
            goto running_threads;
        else 
            goto running_proc;
        return ret;
running_proc:
        //procthreads setting 
        if((service->daemon = procthread_init(0)))
        {
            PROCTHREAD_SET(service, service->daemon);
            service->daemon->use_cond_wait = 0;
            if(service->daemon->message_queue)
            {
                if(service->daemon->message_queue) qmessage_clean(service->daemon->message_queue);
                service->daemon->message_queue = service->message_queue;
                service->daemon->ioqmessage = service->message_queue;
                service->daemon->evbase = service->evbase;
            }
            DEBUG_LOGGER(service->logger, "sbase->q[%p] service->q[%p] daemon->q[%p]",
                    service->sbase->message_queue, service->message_queue, 
                    service->daemon->message_queue);
            service->daemon->service = service;
            ret = 0;
        }
        else
        {
            FATAL_LOGGER(service->logger, "Initialize procthread mode[%d] failed, %s",
                    service->working_mode, strerror(errno));
        }
        return ret;
running_threads:
#ifdef HAVE_PTHREAD
        sigpipe_ignore();
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(struct ip_mreq));
        mreq.imr_multiaddr.s_addr = inet_addr("239.239.239.239");
        mreq.imr_interface.s_addr = inet_addr("127.0.0.1");
        if((service->cond = socket(AF_INET, SOCK_DGRAM, 0)) < 0
            || setsockopt(service->cond, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, 
                        sizeof(struct ip_mreq)) != 0)
        {
            FATAL_LOGGER(service->logger, "new cond socket() failed, %s", strerror(errno));
            _exit(-1);
        }
        //iodaemon 
        /*
        if(service->use_iodaemon)
        {
            if((service->iodaemon = procthread_init(1)))
            {
                PROCTHREAD_SET(service, service->iodaemon);
                if(service->sbase->evlogfile && service->sbase->evlog_level > 0);
                {
                    sprintf(logfile, "%s_%s", service->sbase->evlogfile, service->service_name);
                    service->iodaemon->evbase->set_logfile(service->iodaemon->evbase, logfile);
                    service->iodaemon->evbase->set_log_level(service->iodaemon->evbase, service->sbase->evlog_level);
                }
                NEW_PROCTHREAD("iodaemon", 0, service->iodaemon->threadid, service->iodaemon, service->logger);
                ret = 0;
            }
            else
            {
                FATAL_LOGGER(service->logger, "Initialize new mode[%d] procthread failed, %s",
                        service->working_mode, strerror(errno));
                exit(EXIT_FAILURE);
                return -1;
            }
        }*/
        /* initialize iodaemons */
        if(service->niodaemons > SB_THREADS_MAX) service->niodaemons = SB_THREADS_MAX;
        if(service->niodaemons < 1) service->niodaemons = 1;
        if(service->niodaemons > 0)
        {
            for(i = 0; i < service->niodaemons; i++)
            {
                if((service->iodaemons[i] = procthread_init(service->cond)))
                {
                    PROCTHREAD_SET(service, service->iodaemons[i]);
                    if(service->sbase->evlogfile && service->sbase->evlog_level > 0)
                    {
                        sprintf(logfile, "%s_%s_iodemon%d", service->sbase->evlogfile, service->service_name, i);
                        service->iodaemons[i]->evbase->set_logfile(service->iodaemons[i]->evbase, logfile);
                        service->iodaemons[i]->evbase->set_log_level(service->iodaemons[i]->evbase, service->sbase->evlog_level);
                    }
                    ret = 0;
                }
                else
                {
                    FATAL_LOGGER(service->logger, "Initialize iodaemons pool failed, %s",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                    return -1;
                }
                NEW_PROCTHREAD("iodaemons", i, service->iodaemons[i]->threadid, 
                        service->iodaemons[i], service->logger);
            }
        }
        /* initialize threads  */
        if(service->nprocthreads > SB_THREADS_MAX) service->nprocthreads = SB_THREADS_MAX;
        if(service->nprocthreads < 1) service->nprocthreads = 2;
        if(service->nprocthreads > 0)
        {
            for(i = 0; i < service->nprocthreads; i++)
            {
                if((service->procthreads[i] = procthread_init(0)))
                {
                    PROCTHREAD_SET(service, service->procthreads[i]);
                    x = service->nprocthreads % service->niodaemons;
                    service->procthreads[i]->evbase = service->iodaemons[x]->evbase;
                    service->procthreads[i]->iodaemon = service->iodaemons[x];
                    service->procthreads[i]->ioqmessage = service->iodaemons[x]->message_queue;
                    ret = 0;
                }
                else
                {
                    FATAL_LOGGER(service->logger, "Initialize procthreads pool failed, %s",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                    return -1;
                }
                NEW_PROCTHREAD("procthreads", i, service->procthreads[i]->threadid, 
                        service->procthreads[i], service->logger);
            }
        }
        /* daemon */ 
        if((service->daemon = procthread_init(0)))
        {
            service->daemon->evtimer = service->etimer;
            PROCTHREAD_SET(service, service->daemon);
            service->daemon->use_cond_wait = 0;
            NEW_PROCTHREAD("daemon", 0, service->daemon->threadid, service->daemon, service->logger);
            ret = 0;
        }
        else
        {
            FATAL_LOGGER(service->logger, "Initialize procthread mode[%d] failed, %s",
                    service->working_mode, strerror(errno));
            exit(EXIT_FAILURE);
            return -1;
        }
        /* daemon worker threads */
        if(service->ndaemons > SB_THREADS_MAX) service->ndaemons = SB_THREADS_MAX;
        if(service->ndaemons > 0)
        {
            for(i = 0; i < service->ndaemons; i++)
            {
                if((service->daemons[i] = procthread_init(0)))
                {
                    PROCTHREAD_SET(service, service->daemons[i]);
                    ret = 0;
                }
                else
                {
                    FATAL_LOGGER(service->logger, "Initialize procthreads pool failed, %s",
                            strerror(errno));
                    exit(EXIT_FAILURE);
                    return -1;
                }
                NEW_PROCTHREAD("daemons", i, service->daemons[i]->threadid,
                        service->daemons[i], service->logger);
            }
        }
        return ret;
#else
        service->working_mode = WORKING_PROC;
        goto running_proc;
#endif
        return ret;
    }
    return ret;
}

/* set logfile  */
int service_set_log(SERVICE *service, char *logfile)
{
    if(service && logfile)
    {
        LOGGER_INIT(service->logger, logfile);
        DEBUG_LOGGER(service->logger, "Initialize logger %s", logfile);
        service->is_inside_logger = 1;
        return 0;
    }
    return -1;
}

/* set logfile level  */
int service_set_log_level(SERVICE *service, int level)
{
    if(service && service->logger)
    {
        LOGGER_SET_LEVEL(service->logger, level);
        return 0;
    }
    return -1;
}



/* event handler */
void service_event_handler(int event_fd, short flag, void *arg)
{
    char buf[SB_BUF_SIZE], *p = NULL, *ip = NULL;
    socklen_t rsa_len = sizeof(struct sockaddr_in);
    int fd = -1, port = -1, n = 0, opt = 1;
    SERVICE *service = (SERVICE *)arg;
    PROCTHREAD *pth = NULL;
    struct sockaddr_in rsa;
    CONN *conn = NULL;
    void *ssl = NULL;

    if(service)
    {
        if(event_fd == service->fd)
        {
            if(E_READ & flag)
            {
                if(service->sock_type == SOCK_STREAM)
                {
                    while((fd = accept(event_fd, (struct sockaddr *)&rsa, &rsa_len)) > 0)
                    {
                        ip = inet_ntoa(rsa.sin_addr);
                        port = ntohs(rsa.sin_port);
#ifdef HAVE_SSL
                        if(service->is_use_SSL && service->s_ctx)
                        {
                            if((ssl = SSL_new(XSSL_CTX(service->s_ctx))) && SSL_set_fd((SSL *)ssl, fd) > 0 
                                    && SSL_accept((SSL *)ssl) > 0)                                                   
                            {
                                goto new_conn;
                            }
                            else goto err_conn; 
                        }
#endif
new_conn:
                        /*
                        if((conn = service_addconn(service, service->sock_type, fd, ip, port, 
                                service->ip, service->port, &(service->session), CONN_STATUS_FREE)))
#ifdef HAVE_SSL
                            conn->ssl = ssl;
#endif
                        */
                        DEBUG_LOGGER(service->logger, "Accepted new connection[%s:%d]  via %d", ip, port, fd);
                        if(service->daemon && service->daemon->newconn(service->daemon,fd,ssl)==0)
                        {
                            return ;
                        }
                        else 
                        {
                            DEBUG_LOGGER(service->logger, "adding new connection[%s:%d] via %d failed, %s",ip, port, fd, strerror(errno));
                        }
err_conn:               
#ifdef HAVE_SSL
                        if(ssl)
                        {
                            SSL_shutdown((SSL *)ssl);
                            SSL_free((SSL *)ssl);
                            ssl = NULL;
                        }
#endif
                        if(fd > 0)
                        {
                            shutdown(fd, SHUT_RDWR);
                            close(fd);
                        }
                        return ;
                    }
                    /*
                    else
                    {
                        FATAL_LOGGER(service->logger, "Accept new connection failed, %s", 
                                strerror(errno));
                    }
                    */
                }
                else if(service->sock_type == SOCK_DGRAM)
                {
                    while((n = recvfrom(event_fd, buf, SB_BUF_SIZE, 
                            0, (struct sockaddr *)&rsa, &rsa_len)) > 0)
                    {
                        ip = inet_ntoa(rsa.sin_addr);
                        port = ntohs(rsa.sin_port);
                        if((fd = socket(AF_INET, SOCK_DGRAM, 0)) > 0 
                            && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0
#ifdef SO_REUSEPORT
                            && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0
#endif
                            && bind(fd, (struct sockaddr *)&(service->sa), 
                                sizeof(struct sockaddr_in)) == 0
                            && connect(fd, (struct sockaddr *)&rsa, 
                                sizeof(struct sockaddr_in)) == 0
                            && (conn = service_addconn(service, service->sock_type, fd, 
                                ip, port, service->ip, service->port, 
                                &(service->session), CONN_STATUS_FREE)))
                        {
                            p = buf;
                            MMB_PUSH(conn->buffer, p, n);
                            pth = (PROCTHREAD *)(conn->parent);
                            if(pth)
                            {
                                qmessage_push(pth->message_queue, 
                                    MESSAGE_INPUT, -1, conn->fd, -1, conn, pth, NULL);
                                MUTEX_SIGNAL(pth->mutex);
                            }
                            DEBUG_LOGGER(service->logger, "Accepted new connection[%s:%d] via %d"
                                    " buffer:%d", ip, port, fd, MMB_NDATA(conn->buffer));
                        }
                        else
                        {
                            shutdown(fd, SHUT_RDWR);
                            close(fd);
                            FATAL_LOGGER(service->logger, "Accept new connection failed, %s", 
                                    strerror(errno));
                        }
                    }
                    /*
                    else
                    {
                        FATAL_LOGGER(service->logger, "Accept new connection failed, %s", 
                                strerror(errno));
                    }
                    */
                }
            }
        }
    }
    return ;
}

/* new connection */
CONN *service_newconn(SERVICE *service, int inet_family, int socket_type, 
        char *inet_ip, int inet_port, SESSION *session)
{
    int fd = -1, family = -1, sock_type = -1, remote_port = -1, 
        local_port = -1, flag = 0, opt = 0, status = 0;
    char *local_ip = NULL, *remote_ip = NULL;
    struct sockaddr_in rsa, lsa;
    socklen_t lsa_len = sizeof(lsa);
    struct linger ling = {0};
    SESSION *sess = NULL;
    CONN *conn = NULL;
    void *ssl = NULL;

    if(service && service->lock == 0)
    {
        family  = (inet_family > 0 ) ? inet_family : service->family;
        sock_type = (socket_type > 0 ) ? socket_type : service->sock_type;
        remote_ip = (inet_ip) ? inet_ip : service->ip;
        remote_port  = (inet_port > 0 ) ? inet_port : service->port;
        sess = (session) ? session : &(service->session);
        if((fd = socket(family, sock_type, 0)) > 0)
        {
            //DEBUG_LOGGER(service->logger, "new_conn[%s:%d] via %d", remote_ip, remote_port, fd);
            rsa.sin_family = family;
            rsa.sin_addr.s_addr = inet_addr(remote_ip);
            rsa.sin_port = htons(remote_port);
#ifdef HAVE_SSL
            if(sess->is_use_SSL &&  sock_type == SOCK_STREAM && service->c_ctx)
            {
                if((ssl = SSL_new(XSSL_CTX(service->c_ctx))) 
                        && connect(fd, (struct sockaddr *)&rsa, sizeof(rsa)) == 0 
                        && SSL_set_fd((SSL *)ssl, fd) > 0 && SSL_connect((SSL *)ssl) >= 0)
                {
                    goto new_conn;
                }
                else goto err_conn;
            }
#endif
            ling.l_onoff = 1;ling.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(struct linger));
            opt = 1;setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
            //opt = 60;setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &opt, sizeof(opt));
            //opt = 5;setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &opt, sizeof(opt));
            //opt=3;setsockopt(fd, SOL_TCP, TCP_KEEPCNT, &opt, sizeof(opt)); 
#ifdef SOL_TCP
#ifdef TCP_NODELAY
            opt = 1;setsockopt(fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
#endif
#endif
            if(sess && (sess->flag & O_NONBLOCK))
            {
                flag = fcntl(fd, F_GETFL, 0)|O_NONBLOCK;
                if(fcntl(fd, F_SETFL, flag) != 0) goto err_conn;
                if((connect(fd, (struct sockaddr *)&rsa, sizeof(rsa)) == 0 
                    || errno == EINPROGRESS || errno == EALREADY || errno == EINTR))
                {
                    goto new_conn;
                }else goto err_conn;
            }
            else
            {
                if(connect(fd, (struct sockaddr *)&rsa, sizeof(rsa)) == 0)
                    goto new_conn;
                else goto err_conn;
            }
new_conn:
            getsockname(fd, (struct sockaddr *)&lsa, &lsa_len);
            local_ip    = inet_ntoa(lsa.sin_addr);
            local_port  = ntohs(lsa.sin_port);
            status = CONN_STATUS_FREE;
            if(flag != 0) status = CONN_STATUS_READY; 
            if((conn = service_addconn(service, sock_type, fd, remote_ip, 
                    remote_port, local_ip, local_port, sess, status)))
            {
#ifdef HAVE_SSL
                conn->ssl = (SSL *)ssl;
#endif
                return conn;
            }
            else
            {
                FATAL_LOGGER(service->logger, "connect to remote[%s:%d] via local[%s:%d] fd[%d] session[%p] failed, %s", remote_ip, remote_port, local_ip, local_port, fd, sess, strerror(errno));
            }
err_conn:
#ifdef HAVE_SSL
            if(ssl)
            {
                SSL_shutdown((SSL *)ssl);
                SSL_free((SSL *)ssl);
                ssl = NULL;
            }
#endif
            if(fd > 0)
            {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            FATAL_LOGGER(service->logger, "connect to %s:%d via %d session[%p] failed, %s",
                    remote_ip, remote_port, fd, sess, strerror(errno));
            return conn;
        }
        else
        {
            FATAL_LOGGER(service->logger, "socket(%d, %d, 0) failed, %s", 
                    family, sock_type, strerror(errno));
        }
    }
    return conn;
}

/* new connection */
CONN *service_newproxy(SERVICE *service, CONN *parent, int inet_family, int socket_type, 
        char *inet_ip, int inet_port, SESSION *session)
{
    CONN *conn = NULL;
    struct sockaddr_in rsa, lsa;
    socklen_t lsa_len = sizeof(lsa);
    int fd = -1, family = -1, sock_type = -1, remote_port = -1, local_port = -1;
    char *local_ip = NULL, *remote_ip = NULL;
    SESSION *sess = NULL;
    void *ssl = NULL;

    if(service && service->lock == 0 && parent)
    {
        if(parent && (conn = parent->session.child))
        {
            conn->session.parent = NULL;
            conn->over(conn);
            conn = NULL;
        }
        family  = (inet_family > 0 ) ? inet_family : service->family;
        sock_type = (socket_type > 0 ) ? socket_type : service->sock_type;
        remote_ip = (inet_ip) ? inet_ip : service->ip;
        remote_port  = (inet_port > 0 ) ? inet_port : service->port;
        sess = (session) ? session : &(service->session);
        rsa.sin_family = family;
        rsa.sin_addr.s_addr = inet_addr(remote_ip);
        rsa.sin_port = htons(remote_port);
        if((fd = socket(family, sock_type, 0)) > 0
                && connect(fd, (struct sockaddr *)&rsa, sizeof(rsa)) == 0)
        {
#ifdef HAVE_SSL
            if(sess->is_use_SSL && sock_type == SOCK_STREAM && service->c_ctx)
            {
                DEBUG_LOGGER(service->logger, "SSL_newproxy() to %s:%d",remote_ip, remote_port);
                if((ssl = SSL_new(XSSL_CTX(service->c_ctx))) 
                        && SSL_set_fd((SSL *)ssl, fd) > 0 && SSL_connect((SSL *)ssl) >= 0)
                {
                    goto new_conn;
                }
                else goto err_conn;
            }
#endif
new_conn:
            getsockname(fd, (struct sockaddr *)&lsa, &lsa_len);
            local_ip    = inet_ntoa(lsa.sin_addr);
            local_port  = ntohs(lsa.sin_port);
            if((conn = service_addconn(service, sock_type, fd, remote_ip, remote_port, 
                            local_ip, local_port, sess, CONN_STATUS_FREE)))
            {
                if(conn->session.timeout == 0)
                    conn->session.timeout = SB_PROXY_TIMEOUT;
                if(parent->session.timeout == 0)
                    parent->session.timeout = SB_PROXY_TIMEOUT;
                parent->session.packet_type |= PACKET_PROXY;
                conn->session.packet_type |= PACKET_PROXY;
                conn->session.parent = parent;
                conn->session.parentid = parent->index;
#ifdef HAVE_SSL
                conn->ssl = (SSL *)ssl;
#endif
                return conn;
            }
err_conn:
#ifdef HAVE_SSL
            if(ssl)
            {
                SSL_shutdown((SSL *)ssl);
                SSL_free((SSL *)ssl);
                ssl = NULL;
            }
#endif
            if(fd > 0)
            {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            return conn;
        }
        else
        {
            FATAL_LOGGER(service->logger, "connect to %s:%d via %d session[%p] failed, %s",
                    remote_ip, remote_port, fd, sess, strerror(errno));
        }
    }
    return conn;
}


/* add new connection */
CONN *service_addconn(SERVICE *service, int sock_type, int fd, char *remote_ip, int remote_port, 
        char *local_ip, int local_port, SESSION *session, int status)
{
    PROCTHREAD *procthread = NULL;
    CONN *conn = NULL;
    int index = 0;

    if(service && service->lock == 0 && fd > 0 && session)
    {
        DEBUG_LOGGER(service->logger, "Ready for add-conn remote[%s:%d] local[%s:%d] via %d", remote_ip, remote_port, local_ip, local_port, fd);
        if((conn = service_popfromq(service)))
        {
            //fprintf(stdout, "%s::%d OK\n", __FILE__, __LINE__);
            conn->fd = fd;
            conn->status = status;
            strcpy(conn->remote_ip, remote_ip);
            conn->remote_port = remote_port;
            strcpy(conn->local_ip, local_ip);
            conn->local_port = local_port;
            conn->sock_type = sock_type;
            conn->evtimer   = service->evtimer;
            conn->logger    = service->logger;
            conn->set_session(conn, session);
            /* add  to procthread */
            if(service->working_mode == WORKING_PROC)
            {
                if(service->daemon)
                {
                    //conn->parent    = service->daemon;
                    //conn->ioqmessage = service->daemon->message_queue;
                    //conn->message_queue = service->daemon->message_queue;
                    service->daemon->add_connection(service->daemon, conn);
                }
                else
                {
                    FATAL_LOGGER(service->logger, "can not add connection[%s:%d] on %s:%d "
                            "via %d  to service[%s]", remote_ip, remote_port, 
                            local_ip, local_port, fd, service->service_name);
                    service_pushtoq(service, conn);
                }
            }
            else if(service->working_mode == WORKING_THREAD && service->nprocthreads > 0)
            {
                index = fd % service->nprocthreads;
                if(service->procthreads && (procthread = service->procthreads[index])) 
                {
                    //conn->parent = procthread;
                    //conn->ioqmessage = procthread->ioqmessage;
                    //conn->message_queue = procthread->message_queue;
                    if(status == CONN_STATUS_FREE)
                    {
                        procthread->add_connection(procthread, conn);   
                    }
                    else
                    {
                        procthread->addconn(procthread, conn);
                    }
                    DEBUG_LOGGER(service->logger, "adding connection[%p][%s:%d] local[%s:%d] dstate:%d via %d", conn, conn->remote_ip, conn->remote_port, conn->local_ip, conn->local_port, conn->d_state, conn->fd);
                }
                else
                {
                    FATAL_LOGGER(service->logger, "can not add connection remote[%s:%d]-local[%s:%d] "
                            "via %d  to service[%s]->procthreads[%p][%d] nprocthreads:%d", remote_ip, remote_port, 
                            local_ip, local_port, fd, service->service_name, service->procthreads, index, service->nprocthreads);
                    service_pushtoq(service, conn);
                }
            }
            else
            {
                    service_pushtoq(service, conn);
            }
        }
    }
    return conn;
}

/* push connection to connections pool */
int service_pushconn(SERVICE *service, CONN *conn)
{
    int ret = -1, x = 0, id = 0, i = 0;
    CONN *parent = NULL;

    if(service && service->lock == 0 && conn && service->connections)
    {
        MUTEX_LOCK(service->mutex);
        DEBUG_LOGGER(service->logger, "start pushconn()");
        for(i = 1; i < service->connections_limit; i++)
        {
            if(service->connections[i] == NULL)
            {
                service->connections[i] = conn;
                conn->index = i;
                service->running_connections++;
                if((id = conn->groupid) > 0 && id < SB_GROUPS_MAX)
                {
                    x = 0;
                    while(x < SB_GROUP_CONN_MAX)
                    {
                        if(service->groups[id].conns_free[x] == 0)
                        {
                            service->groups[id].conns_free[x] = i;
                            ++(service->groups[id].nconns_free);
                            conn->gindex = x;
                            break;
                        }
                        ++x;
                    }
                }
                if(i >= service->index_max) service->index_max = i;
                ret = 0;
                DEBUG_LOGGER(service->logger, "Added new conn[%p][%s:%d] on %s:%d via %d "
                    "d_state:%d index[%d] of total %d", conn, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd, conn->d_state,
                    conn->index, service->running_connections);
                break;
            }
        }
        //for proxy
        if((conn->session.packet_type & PACKET_PROXY)
                && (parent = (CONN *)(conn->session.parent)) 
                && conn->session.parentid  >= 0 
                && conn->session.parentid < service->index_max 
                && conn->session.parent == service->connections[conn->session.parentid])
        {
            parent->bind_proxy(parent, conn);
        }
        DEBUG_LOGGER(service->logger, "over pushconn()");
        MUTEX_UNLOCK(service->mutex);
    }
    return ret;
}

/* pop connection from connections pool with index */
int service_popconn(SERVICE *service, CONN *conn)
{
    int ret = -1, id = 0, x = 0;

    if(service && service->lock == 0 && service->connections && conn)
    {
        MUTEX_LOCK(service->mutex);
        DEBUG_LOGGER(service->logger, "start popconn()");
        if(conn->index > 0 && conn->index <= service->index_max
                && service->connections[conn->index] == conn)
        {
            if((id = conn->groupid) > 0 && id < SB_GROUPS_MAX)
            {
                if((x = conn->gindex) >= 0 && x < SB_CONN_MAX 
                        && service->groups[id].conns_free[x] > 0
                        && service->groups[id].conns_free[x] == conn->index)
                {
                    service->groups[id].conns_free[x] = 0;
                    --(service->groups[id].nconns_free);
                }
                if(conn->status == CONN_STATUS_FREE)
                {
                    --(service->groups[id].nconnected);
                }
                --(service->groups[id].total);
            }
            service->connections[conn->index] = NULL;
            service->running_connections--;
            if(service->index_max == conn->index) service->index_max--;
            DEBUG_LOGGER(service->logger, "Removed connection[%s:%d] on %s:%d via %d "
                    "index[%d] of total %d", conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd, 
                    conn->index, service->running_connections);
            ret = 0;
        }
        else
        {
            FATAL_LOGGER(service->logger, "Removed connection[%s:%d] on %s:%d via %d "
                    "index[%d] of total %d failed", conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, conn->fd, 
                    conn->index, service->running_connections);
        }
        DEBUG_LOGGER(service->logger, "over popconn()");
        MUTEX_UNLOCK(service->mutex);
        //return service_pushtoq(service, conn);
    }
    return ret;
}

/* set connection status ok */
int service_okconn(SERVICE *service, CONN *conn)
{
    int id = -1;

    if(service && conn)
    {
        if((id = conn->groupid) > 0 && id <= service->ngroups)
        {
            service->groups[id].nconnected++;
        }
        conn->status = CONN_STATUS_FREE;
        return 0;
    }
    return -1;
}

/* get connection with free state */
CONN *service_getconn(SERVICE *service, int groupid)
{
    CONN *conn = NULL;
    int i = 0, x = 0;

    if(service && service->lock == 0)
    {
        MUTEX_LOCK(service->mutex);
        if(groupid > 0 && groupid <= service->ngroups)
        {
            x = 0;
            while(x < SB_GROUP_CONN_MAX && service->groups[groupid].nconns_free > 0)
            {
                if((i = service->groups[groupid].conns_free[x]) > 0 
                        && (conn = service->connections[i]))
                {
                    if(conn->status == CONN_STATUS_FREE && conn->d_state == D_STATE_FREE 
                            && conn->c_state == C_STATE_FREE)
                    {
                        conn->gindex = -1;
                        service->groups[groupid].conns_free[x] = 0;
                        --(service->groups[groupid].nconns_free);
                        DEBUG_LOGGER(service->logger, "get conn[%s:%d] from conns_free[%d]", conn->local_ip, conn->local_port, x);
                        conn->start_cstate(conn);
                        break;
                    }
                    else 
                    {
                        DEBUG_LOGGER(service->logger, "non-free conn[%s::%d] status:%d c_state:%d d_state:%d on conns_free[%d]", conn->local_ip, conn->local_port, conn->status, conn->c_state, conn->d_state, x);
                        conn = NULL;
                    }
                }
                ++x;
            }
        }
        else
        {
            while(service->nconns_free > 0)
            {
                x = --(service->nconns_free);
                if((i = service->conns_free[x]) >= 0 && i < SB_CONN_MAX 
                        && (conn = service->connections[i]))
                {
                    conn->start_cstate(conn);
                    break;
                }
            }
        }
        MUTEX_UNLOCK(service->mutex);
    }
    return conn;
}

/* freeconn */
int service_freeconn(SERVICE *service, CONN *conn)
{
    int id = 0, x = 0;

    if(service && conn)
    {
        MUTEX_LOCK(service->mutex);
        if((id = conn->groupid) > 0 && id < SB_GROUPS_MAX)
        {
            if(service->groups[id].limit <= 0)
            {
                conn->close(conn);
            }
            else
            {
                x = 0;
                while(x < SB_GROUP_CONN_MAX)
                {
                    if(service->groups[id].conns_free[x] == 0)
                    {
                        service->groups[id].conns_free[x] = conn->index;
                        ++(service->groups[id].nconns_free);
                        conn->gindex = x;
                        conn->over_cstate(conn);
                        DEBUG_LOGGER(service->logger, "free conn[%s:%d] to conns_free[%d]", conn->local_ip, conn->local_port, x);
                        break;
                    }
                    ++x;
                }
            }
        }
        else
        {
            x = service->nconns_free++; 
            service->conns_free[x] = conn->index;
        }
        MUTEX_UNLOCK(service->mutex);
        return 0;
    }
    return -1;
}

/* find connection as index */
CONN *service_findconn(SERVICE *service, int index)
{
    CONN *conn = NULL;
    if(service && service->lock == 0 && index >= 0 && index <= service->index_max)
    {
        conn = service->connections[index];
    }
    return conn;
}

/* service over conn */
void service_overconn(SERVICE *service, CONN *conn)
{
    PROCTHREAD *daemon = NULL;

    if(service && conn && (daemon = service->daemon))
    {
        qmessage_push(daemon->message_queue, MESSAGE_QUIT, conn->index, conn->fd, 
                -1, daemon, conn, NULL);
        MUTEX_SIGNAL(daemon->mutex);
    }
    return ;
}

/* pop chunk from service  */
CHUNK *service_popchunk(SERVICE *service)
{
    CHUNK *cp = NULL;
    int x = 0;

    if(service && service->lock == 0 && service->qchunks)
    {
        MUTEX_LOCK(service->mutex);
        if(service->nqchunks > 0 && (x = --(service->nqchunks)) >= 0 
                && (cp = service->qchunks[x]))
        {
            service->qchunks[x] = NULL;
            DEBUG_LOGGER(service->logger, "popchunk(%p) nchunks:%d", cp, service->nchunks);
        }
        else
        {
            if((cp = chunk_init()))
            {
                service->nchunks++;
                DEBUG_LOGGER(service->logger, "popchunk(%p) nchunks:%d", cp, service->nchunks);
            }

        }
        MUTEX_UNLOCK(service->mutex);
    }
    return cp;
}

/* push to qconns */
int service_pushtoq(SERVICE *service, CONN *conn)
{
    int x = 0;

    if(service && service->lock == 0 && conn)
    {
        MUTEX_LOCK(service->mutex);
        DEBUG_LOGGER(service->logger, "starting pushq(%d) running_conns:%d conn[%p]->d_state:%d", service->nqconns, service->running_connections, conn, conn->d_state);
        if(SB_QCONN_MAX > 0 && service->nqconns < SB_QCONN_MAX)
        {
            x = service->nqconns++;
            service->qconns[x] = conn;
        }
        else 
        {
            service->nfreeconns++;
            //WARN_LOGGER(service->logger, "Ready for clean conn[%p]->d_state:%d new:%d free:%d", conn,conn->d_state, service->nnewconns, service->nfreeconns);
            conn->clean(conn);
            service->nconn--;
        }
        DEBUG_LOGGER(service->logger, "over pushq(%d) conn[%p] nconn:%d", service->nqconns, conn, service->nconn);
        MUTEX_UNLOCK(service->mutex);
    }
    return x;
}

/* push to qconn */
CONN *service_popfromq(SERVICE *service)
{
    CONN *conn = NULL;
    int x = 0;

    if(service && service->lock == 0)
    {
        MUTEX_LOCK(service->mutex);
        DEBUG_LOGGER(service->logger, "starting popfromq(%d)", service->nqconns);
        if(service->nqconns > 0 && (x = --(service->nqconns)) >= 0 
                && (conn = service->qconns[x]))
        {
            service->qconns[x] = NULL;
        }
        else
        {
            if((conn = conn_init()))
            {
                service->nconn++;
                service->nnewconns++;
                DEBUG_LOGGER(service->logger, "conn_init(%d) conn[%p]->d_state:%d nconn:%d ", service->nqconns, conn, conn->d_state, service->nconn);
            }
        }
        //fprintf(stdout, "nqconns:%d\n", service->nqconns);
        if(conn)
        {
            DEBUG_LOGGER(service->logger, "over popfromq(%d) conn[%p]->d_state:%d nconn:%d", service->nqconns, conn, conn->d_state, service->nconn);
        }
        MUTEX_UNLOCK(service->mutex);
    }
    return conn;
}

/* push chunk to service  */
int service_pushchunk(SERVICE *service, CHUNK *cp)
{
    int ret = -1, x = 0;

    if(service && service->lock == 0 && service->qchunks && cp)
    {
        MUTEX_LOCK(service->mutex);
        if(service->nqchunks < SB_CHUNKS_MAX)
        {
            chunk_reset(cp);
            x = service->nqchunks++;
            service->qchunks[x] = cp;
            DEBUG_LOGGER(service->logger, "pushchunk(%p) nchunks:%d", cp, service->nchunks);
        }
        else 
        {
            ACCESS_LOGGER(service->logger, "pushchunk(%p) nchunks:%d", cp, service->nchunks);
            chunk_clean(cp);
            service->nchunks--;
        }
        MUTEX_UNLOCK(service->mutex);
        ret = 0;
    }
    return ret;
}

/* new chunk */
CB_DATA *service_newchunk(SERVICE *service, int len)
{
    CB_DATA *chunk = NULL;
    CHUNK *cp = NULL;

    if(service && service->lock == 0)
    {
        if((cp = service_popchunk(service)))
        {
            chunk_mem(cp, len);
            chunk = (CB_DATA *)cp;
        }
    }
    return chunk;
}

/* set service session */
int service_set_session(SERVICE *service, SESSION *session)
{
    if(service && session)
    {
        memcpy(&(service->session), session, sizeof(SESSION));
        return 0;
    }
    return -1;
}

/* add multicast */
int service_add_multicast(SERVICE *service, char *multicast_ip)
{
    struct ip_mreq mreq;
    int ret = -1;

    if(service && service->lock == 0 && service->sock_type == SOCK_DGRAM && multicast_ip 
            && service->ip && service->fd > 0)
    {
        memset(&mreq, 0, sizeof(struct ip_mreq));
        mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
        mreq.imr_interface.s_addr = inet_addr(service->ip);
        if((ret = setsockopt(service->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, 
                        (char*)&mreq, sizeof(struct ip_mreq))) == 0)
        {
            DEBUG_LOGGER(service->logger, "added multicast:%s to service[%p]->fd[%d]",
                    multicast_ip, service, service->fd);
        }
    }
    return ret;
}

/* drop multicast */
int service_drop_multicast(SERVICE *service, char *multicast_ip)
{
    struct ip_mreq mreq;
    int ret = -1;

    if(service && service->sock_type == SOCK_DGRAM && service->ip && service->fd)
    {
        memset(&mreq, 0, sizeof(struct ip_mreq));
        mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
        mreq.imr_interface.s_addr = inet_addr(service->ip);
        ret = setsockopt(service->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,(char*)&mreq, sizeof(mreq));
    }
    return ret;
}

/* broadcast */
int service_broadcast(SERVICE *service, char *data, int len)
{
    int ret = -1, i = 0;
    CONN *conn = NULL;

    if(service && service->lock == 0 && service->running_connections > 0)
    {
        MUTEX_LOCK(service->mutex);
        for(i = 0; i < service->index_max; i++)
        {
            if((conn = service->connections[i]))
            {
                conn->push_chunk(conn, data, len);
            }
        }
        MUTEX_UNLOCK(service->mutex);
        ret = 0;
    }
    return ret;
}

/* add group */
int service_addgroup(SERVICE *service, char *ip, int port, int limit, SESSION *session)
{
    int id = -1;
    if(service && service->lock == 0 && service->ngroups < SB_GROUPS_MAX)
    {
        MUTEX_LOCK(service->mutex);
        id = ++service->ngroups;
        strcpy(service->groups[id].ip, ip);
        service->groups[id].port = port;
        service->groups[id].limit = limit;
        memcpy(&(service->groups[id].session), session, sizeof(SESSION));
        //fprintf(stdout, "%s::%d service[%s]->group[%d]->session.data_handler:%p\n", __FILE__, __LINE__, service->service_name, id, service->groups[id].session.data_handler);
        MUTEX_UNLOCK(service->mutex);
    }
    return id;
}

/* add group */
int service_closegroup(SERVICE *service, int groupid)
{
    int i = 0, id = -1;
    CONN *conn = NULL;

    if(service && groupid < SB_GROUPS_MAX)
    {
        MUTEX_LOCK(service->mutex);
        service->groups[groupid].limit = 0;
        while((i = --(service->groups[groupid].nconns_free)) >= 0)
        {
            if((id = service->groups[groupid].conns_free[i]) >= 0
                && (conn = service->connections[id]))
            {
                conn->close(conn);
            }
        }
        MUTEX_UNLOCK(service->mutex);
    }
    return id;
}

/* group cast */
int service_castgroup(SERVICE *service, char *data, int len)
{
    CONN *conn = NULL;
    int i = 0;

    if(service && service->lock == 0 && data && len > 0 && service->ngroups > 0)
    {
        for(i = 1; i <= service->ngroups; i++)
        {
            if((conn = service_getconn(service, i)))
            {
                conn->start_cstate(conn);
                conn->groupid = i;
                conn->push_chunk(conn, data, len);
            }
        }
        return 0;
    }
    return -1;
}

/* state groups */
int service_stategroup(SERVICE *service)
{
    CONN *conn = NULL;
    int i = 0;

    if(service && service->lock == 0 && service->ngroups > 0)
    {
        for(i = 1; i <= service->ngroups; i++)
        {
            if(service->groups[i].total >= service->groups[i].limit && service->groups[i].nconnected <= 0) 
            {
                ACCESS_LOGGER(service->logger, "ignore stategroup(%d) total:%d nconnected:%d limit:%d", i, service->groups[i].total, service->groups[i].nconnected, service->groups[i].limit);
                continue;
            }
            while(service->groups[i].limit > 0  
                    && service->groups[i].total < service->groups[i].limit
                    && (conn = service_newconn(service, 0, 0, service->groups[i].ip,
                            service->groups[i].port, &(service->groups[i].session))))
            {
                conn->groupid = i;
                service->groups[i].total++;
                if(service->groups[i].nconnected <= 0) break;
            }
        }
        //DEBUG_LOGGER(service->logger, "over stategroup()");
        return 0;
    }
    return -1;
}

/* new task */
int service_newtask(SERVICE *service, CALLBACK *task_handler, void *arg)
{
    PROCTHREAD *pth = NULL;
    int index = 0, ret = -1;

    if(service && service->lock == 0)
    {
        /* Add task for procthread */
        if(service->working_mode == WORKING_PROC)
            pth = service->daemon;
        else if(service->working_mode == WORKING_THREAD && service->ndaemons > 0)
        {
            index = service->ntask % service->ndaemons;
            pth = service->daemons[index];
        }
        if(pth)
        {
            pth->newtask(pth, task_handler, arg);
            service->ntask++;
            ret = 0;
        }
    }
    return ret;
}

/* add new transaction */
int service_newtransaction(SERVICE *service, CONN *conn, int tid)
{
    PROCTHREAD *pth = NULL;
    int index = 0, ret = -1;

    if(service && service->lock == 0 && conn && conn->fd > 0)
    {
        /* Add transaction for procthread */
        if(service->working_mode == WORKING_PROC && service->daemon)
        {
            DEBUG_LOGGER(service->logger, "Adding transaction[%d] to %s:%d on %s:%d "
                    "to procthread[%d]", tid, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port, getpid());
            return service->daemon->newtransaction(service->daemon, conn, tid);
        }
        /* Add transaction to procthread pool */
        if(service->working_mode == WORKING_THREAD && service->nprocthreads > 0)
        {
            index = conn->fd % service->nprocthreads;
            pth = service->procthreads[index];
            DEBUG_LOGGER(service->logger, "Adding transaction[%d] to %s:%d on %s:%d "
                    "to procthreads[%d]", tid, conn->remote_ip, conn->remote_port, 
                    conn->local_ip, conn->local_port,index);
            if(pth && pth->newtransaction)
                return pth->newtransaction(pth, conn, tid);
        }
    }
    return ret;
}

/* stop service */
void service_stop(SERVICE *service)
{
    CONN *conn = NULL;
    int i = 0;

    if(service)
    {
        service->lock = 1;
        DEBUG_LOGGER(service->logger, "ready for stop service:%s running_connections:%d nconn:%d nqconns:%d nchunks:%d nqchunk:%d\n", service->service_name, service->running_connections, service->nconn, service->nqconns, service->nchunks, service->nqchunks);
        //stop all connections 
        if(service->connections && service->index_max >= 0)
        {
            //DEBUG_LOGGER(service->logger, "Ready for close connections[%d]",  service->index_max);
            for(i = 0; i <= service->index_max; i++)
            {
                if((conn = service->connections[i]))
                {
                    DEBUG_LOGGER(service->logger, "Ready for close connections[%d] pconn[%p] dstate:%d remote[%s:%d] via %d", i, conn, conn->d_state, conn->remote_ip, conn->remote_port, conn->fd); 
                    conn->close(conn);
                }
            }
        }
        //iodaemon
        /*
        if(service->iodaemon)
        {
            DEBUG_LOGGER(service->logger, "Ready for stop daemon");
            service->iodaemon->terminate(service->iodaemon);
            DEBUG_LOGGER(service->logger, "Ready for joinning daemon thread");
            PROCTHREAD_EXIT(service->iodaemon->threadid, NULL);
            DEBUG_LOGGER(service->logger, "Joinning daemon thread");
            DEBUG_LOGGER(service->logger, "over for stop daemon");
        }
        */
        //iodaemons
        if(service->niodaemons > 0)
        {
            DEBUG_LOGGER(service->logger, "Ready for stop iodaemons");
            for(i = 0; i < service->niodaemons; i++)
            {
                if(service->iodaemons[i])
                {
                    service->iodaemons[i]->stop(service->iodaemons[i]);
                    PROCTHREAD_EXIT(service->iodaemons[i]->threadid, NULL);
                }
            }
            DEBUG_LOGGER(service->logger, "over for stop iodaemons");
        }
        //threads
        if(service->nprocthreads > 0)
        {
            DEBUG_LOGGER(service->logger, "Ready for stop procthreads");
            for(i = 0; i < service->nprocthreads; i++)
            {
                if(service->procthreads[i])
                {
                    service->procthreads[i]->stop(service->procthreads[i]);
                    PROCTHREAD_EXIT(service->procthreads[i]->threadid, NULL);
                }
            }
            DEBUG_LOGGER(service->logger, "over for stop threads");
        }
        //daemons
        if(service->ndaemons > 0)
        {
            DEBUG_LOGGER(service->logger, "Ready for stop daemons");
            for(i = 0; i < service->ndaemons; i++)
            {
                if(service->daemons[i])
                {
                    service->daemons[i]->stop(service->daemons[i]);
                    PROCTHREAD_EXIT(service->daemons[i]->threadid, NULL);
                }
            }
            DEBUG_LOGGER(service->logger, "over for stop daemons");
        }
        //daemon
        if(service->daemon)
        {
            DEBUG_LOGGER(service->logger, "Ready for stop daemon");
            service->daemon->terminate(service->daemon);
            DEBUG_LOGGER(service->logger, "Ready for joinning daemon thread");
            PROCTHREAD_EXIT(service->daemon->threadid, NULL);
            DEBUG_LOGGER(service->logger, "Joinning daemon thread");
            DEBUG_LOGGER(service->logger, "over for stop daemon");
        }
        EVTIMER_DEL(service->evtimer, service->evid);
            DEBUG_LOGGER(service->logger, "Ready for remove event");
        //remove event
        event_destroy(&service->event);
        DEBUG_LOGGER(service->logger, "over for remove event");
        if(service->fd > 0)close(service->fd);
        if(service->cond > 0)close(service->cond);
        DEBUG_LOGGER(service->logger, "over for stop service[%s]", service->service_name);
    }
    return ;
}

/* state check */
void service_state(void *arg)
{
    SERVICE *service = (SERVICE *)arg;
    int n = 0;

    if(service)
    {
        if(service->service_type == C_SERVICE)
        {
            if(service->ngroups > 0)service_stategroup(service);
            else
            {
                if(service->running_connections < service->client_connections_limit)
                {
                    //DEBUG_LOGGER(service->logger, "Ready for state connection[%s:%d][%d] running:%d ",service->ip, service->port, service->client_connections_limit,service->running_connections);
                    n = service->client_connections_limit - service->running_connections;
                    while(n > 0)
                    {
                        if(service->newconn(service, -1, -1, NULL, -1, NULL) == NULL)
                        {
                            FATAL_LOGGER(service->logger, "connect to %s:%d failed, %s", 
                                    service->ip, service->port, strerror(errno));
                            break;
                        }
                        n--;
                    }
                }
            }
        }
    }
    return ;
}

/* heartbeat handler */
void service_set_heartbeat(SERVICE *service, int interval, CALLBACK *handler, void *arg)
{
    if(service)
    {
        service->heartbeat_interval = interval;
        service->heartbeat_handler = handler;
        service->heartbeat_arg = arg;
        if(service->evtimer)
        {
            service->evid = EVTIMER_ADD(service->evtimer, service->heartbeat_interval, 
                    &service_evtimer_handler, (void *)service);
        }
    }
    return ;
}

/* active heartbeat */
void service_active_heartbeat(void *arg)
{
    SERVICE *service = (SERVICE *)arg;

    if(service)
    {
        service_state(service);
        if(service->heartbeat_handler)
        {
            service->heartbeat_handler(service->heartbeat_arg);
        }
        //if(service->evid == 0)fprintf(stdout, "Ready for updating evtimer[%p][%d] [%p][%p] count[%d] q[%d]\n", service->evtimer, service->evid, PEVT_EVN(service->evtimer, service->evid)->prev, PEVT_EVN(service->evtimer, service->evid)->next, PEVT_NLIST(service->evtimer), PEVT_NQ(service->evtimer));
        //if(service->evid == 0) EVTIMER_LIST(service->evtimer, stdout);
        EVTIMER_UPDATE(service->evtimer, service->evid, service->heartbeat_interval, 
                &service_evtimer_handler, (void *)service);
        //if(service->evid == 0)fprintf(stdout, "Over for updating evtimer[%p][%d] [%p][%p] count[%d] q[%d]\n", service->evtimer, service->evid, PEVT_EVN(service->evtimer, service->evid)->prev, PEVT_EVN(service->evtimer, service->evid)->next, PEVT_NLIST(service->evtimer), PEVT_NQ(service->evtimer));
        //if(service->evid == 0) EVTIMER_LIST(service->evtimer, stdout);
    }
    return ;
}

/* active evtimer heartbeat */
void service_evtimer_handler(void *arg)
{
    SERVICE *service = (SERVICE *)arg;

    if(service && service->daemon)
    {
        //DEBUG_LOGGER(service->logger, "Ready for activing evtimer[%p][%d] count[%d] q[%d]", service->evtimer, service->evid, PEVT_NLIST(service->evtimer), PEVT_NQ(service->evtimer));
        service->daemon->active_heartbeat(service->daemon, 
                &service_active_heartbeat, (void *)service);
        //DEBUG_LOGGER(service->logger, "Over for activing evtimer[%p][%d] count[%d] q[%d]", service->evtimer, service->evid, PEVT_NLIST(service->evtimer), PEVT_NQ(service->evtimer));
    }
    return ;
}

/* service clean */
void service_clean(SERVICE *service)
{
    CONN *conn = NULL;
    CHUNK *cp = NULL;
    int i = 0;

    if(service)
    {
        
        event_clean(&(service->event)); 
        if(service->daemon) service->daemon->clean(service->daemon);
        if(service->etimer) {EVTIMER_CLEAN(service->etimer);}
        //if(service->iodaemon) service->iodaemon->clean(service->iodaemon);
        //clean procthreads
        if(service->nprocthreads > 0)
        {
            for(i = 0; i < service->nprocthreads; i++)
            {
                if(service->iodaemons[i])
                    service->iodaemons[i]->clean(service->iodaemons[i]);
                if(service->procthreads[i])
                    service->procthreads[i]->clean(service->procthreads[i]);
            }
            //xmm_free(service->iodaemons, sizeof(PROCTHREAD *) * service->nprocthreads);
            //xmm_free(service->procthreads, sizeof(PROCTHREAD *) * service->nprocthreads);
        }
        //clean daemons
        if(service->ndaemons > 0)
        {
            for(i = 0; i < service->ndaemons; i++)
            {
                if(service->daemons[i])
                {
                    service->daemons[i]->clean(service->daemons[i]);
                }
            }
            //xmm_free(service->daemons, sizeof(PROCTHREAD *) * service->ndaemons);
        }
        //clean connection_queue
        DEBUG_LOGGER(service->logger, "Ready for clean connection_chunk:%d", service->nqconns);
        if(service->nqconns > 0)
        {
            //fprintf(stdout, "nqconns:%d\n", service->nqconns);
            //DEBUG_LOGGER(service->logger, "Ready for clean connections");
            while((i = --(service->nqconns)) >= 0)
            {
                if((conn = (service->qconns[i]))) 
                {
                    DEBUG_LOGGER(service->logger, "Ready for clean conn[%p] fd[%d]", conn, conn->fd);
                    conn->clean(conn);
                    service->nconn--;
                }
            }
        }
        //clean chunks queue
        DEBUG_LOGGER(service->logger, "Ready for clean chunks_queue:%d", service->nqchunks);
        if(service->nqchunks > 0)
        {
            //DEBUG_LOGGER(service->logger, "Ready for clean chunks");
            while((i = --(service->nqchunks)) >= 0)
            {
                if((cp = service->qchunks[i]))
                {
                    DEBUG_LOGGER(service->logger, "Ready for clean conn[%p]", cp);
                    chunk_clean(cp);
                }
            }
        }
        /* SSL */
#ifdef HAVE_SSL
        if(service->s_ctx) SSL_CTX_free(XSSL_CTX(service->s_ctx));
        if(service->c_ctx) SSL_CTX_free(XSSL_CTX(service->c_ctx));
#endif
        MUTEX_DESTROY(service->mutex);
        if(service->is_inside_logger) 
        {
            LOGGER_CLEAN(service->logger);
        }
        xqueue_clean(service->queue);
        xmm_free(service, sizeof(SERVICE));
    }
    return ;
}

/* service close */
void service_close(SERVICE *service)
{
    if(service)
    {
        service_stop(service);
        service->sbase->remove_service(service->sbase, service);
        service_clean(service);
    }
    return ;
}

/* Initialize service */
SERVICE *service_init()
{
    SERVICE *service = NULL;
    if((service = (SERVICE *)xmm_mnew(sizeof(SERVICE))))
    {
        MUTEX_RESET(service->mutex);
        service->queue              = xqueue_init();
        service->etimer             = EVTIMER_INIT();
        service->set                = service_set;
        service->run                = service_run;
        service->set_log            = service_set_log;
        service->set_log_level      = service_set_log_level;
        service->stop               = service_stop;
        service->newproxy           = service_newproxy;
        service->newconn            = service_newconn;
        service->okconn             = service_okconn;
        service->addconn            = service_addconn;
        service->pushconn           = service_pushconn;
        service->popconn            = service_popconn;
        service->getconn            = service_getconn;
        service->freeconn           = service_freeconn;
        service->findconn           = service_findconn;
        service->overconn           = service_overconn;
        service->popchunk           = service_popchunk;
        service->pushchunk          = service_pushchunk;
        service->newchunk           = service_newchunk;
        service->set_session        = service_set_session;
        service->add_multicast      = service_add_multicast;
        service->drop_multicast     = service_drop_multicast;
        service->broadcast          = service_broadcast;
        service->addgroup           = service_addgroup;
        service->closegroup         = service_closegroup;
        service->castgroup          = service_castgroup;
        service->stategroup         = service_stategroup;
        service->newtask            = service_newtask;
        service->newtransaction     = service_newtransaction;
        service->set_heartbeat      = service_set_heartbeat;
        service->clean              = service_clean;
        service->close              = service_close;
    }
    return service;
}
