//////////////////////////////////////////////////////////////////////
//
//  server tracker for FlightGear
//  (c) 2006 Julien Pierru
//  (c) 2012 Rob Dosogne ( FreeBSD friendly )
//
//  Licenced under GPL
//
//////////////////////////////////////////////////////////////////////
#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <iostream>
#include <fstream>
#include <list>
#include <string>
#include <string.h>
#ifndef _MSC_VER
    #include <errno.h>
    #include <time.h>
    #include <stdint.h>
    #include <unistd.h>
    #include <sys/ipc.h>
    #include <sys/msg.h>
    #include <sys/types.h>
    #ifndef __FreeBSD__
        #include <endian.h>
    #endif
#endif
#include <unistd.h>
#include "common.h"
#include "fg_tracker.hxx"
#include "typcnvt.hxx"
#include <simgear/debug/logstream.hxx>
#include "daemon.hxx"

#define MAXLINE 4096
#ifndef DEF_TRACKER_SLEEP
    #define DEF_TRACKER_SLEEP 600   // try to connect each ten minutes
#endif // DEF_TRACKER_SLEEP

#ifdef _MSC_VER
    typedef int pid_t;
    int getpid(void)
    {
        return (int)GetCurrentThreadId();
    }
#else
    extern  cDaemon Myself;
#endif // !_MSC_VER

#ifndef DEF_DEBUG_OUTPUT
    #define DEF_DEBUG_OUTPUT false
#endif

extern bool RunAsDaemon;
static bool AddDebug = DEF_DEBUG_OUTPUT;

//////////////////////////////////////////////////////////////////////
//
//      Initilize to standard values
//
//////////////////////////////////////////////////////////////////////
FG_TRACKER::FG_TRACKER (int port, string server, int id)
{
    ipcid         = id;
    m_TrackerPort = port;
    strcpy (m_TrackerServer, server.c_str());
    if (!RunAsDaemon || AddDebug) 
        printf("FG_TRACKER::FG_TRACKER: Server: %s, Port: %d\n", m_TrackerServer, m_TrackerPort);
} // FG_TRACKER()
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//
//      terminate the tracker
//
//////////////////////////////////////////////////////////////////////
FG_TRACKER::~FG_TRACKER ()
{
} // ~FG_TRACKER()
//////////////////////////////////////////////////////////////////////

#ifdef USE_TRACKER_PORT
void *func_Tracker(void *vp)
{
    FG_TRACKER *pt = (FG_TRACKER *)vp;
    pt->Connect();
    pt->TrackerLoop();
    return ((void *)0xdead);
}

#endif // #ifdef USE_TRACKER_PORT

//////////////////////////////////////////////////////////////////////
//
//      Initilize the tracker
//
//////////////////////////////////////////////////////////////////////
int
FG_TRACKER::InitTracker ( pid_t *pPIDS )
{
#ifndef NO_TRACKER_PORT
#ifdef USE_TRACKER_PORT
    if (pthread_create( &thread, NULL, func_Tracker, (void*)this ))
    {
        SG_ALERT (SG_SYSTEMS, SG_ALERT, "# FG_TRACKER::InitTracker: can't create thread...");
        return 1;
    }
#else // !#ifdef USE_TRACKER_PORT
    pid_t ChildsPID;
    ChildsPID = fork ();
    if (ChildsPID < 0)
    {
        SG_ALERT (SG_SYSTEMS, SG_ALERT, "# FG_TRACKER::InitTracker: fork() FAILED!");
        return 1;
    }
    else if (ChildsPID == 0)
    {
        usleep(2500);
        Connect ();
        TrackerLoop ();
        exit (0);
    }
    else
    {
        pPIDS[0] = ChildsPID; // parent - store child PID
    }
#endif // #ifdef USE_TRACKER_PORT y/n
#endif // NO_TRACKER_PORT
    return (0);
} // InitTracker (int port, string server, int id, int pid)
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//
//  send the messages to the tracker server
//
//////////////////////////////////////////////////////////////////////
int
FG_TRACKER::TrackerLoop ()
{
    m_MsgBuffer buf;
    bool  sent;
    bool  waiting_reply;
    int   length;
    char  res[MAXLINE];
    pid_t pid = getpid();
    short int time_out_counter_l=0;
    unsigned int time_out_counter_u=0;
    short int time_out_fraction=20; /* 1000000/time_out_fraction must be integer*/
    int msgrcv_errno=0;
    waiting_reply= false;
    sent = true;
    strcpy ( res, "" );

    if (!RunAsDaemon || AddDebug)
        printf("[%d] FG_TRACKER::TrackerLoop entered\n",pid);
    for ( ; ; )
    {
        /*time-out issue*/
        usleep(1000000/time_out_fraction);
        time_out_counter_l++;
        if (time_out_counter_l==time_out_fraction)
        {
            time_out_counter_u++;
            time_out_counter_l=0;
        }
        if (time_out_counter_u%60==0 && time_out_counter_u >=180 && time_out_counter_l==0)
        {   /*Print warning*/
            if (!RunAsDaemon || AddDebug)
                printf("[%d] Warning: FG_TRACKER::TrackerLoop No data receive from server for %d seconds\n",pid,time_out_counter_u);
        }
        if (time_out_counter_u%300==0 && time_out_counter_l==0)
        {   /*Timed out - reconnect*/
            printf("[%d] FG_TRACKER::TrackerLoop Connection timed out...\n",pid);
            SG_LOG (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TrackerLoop: Connection timed out...");
            Connect ();
            waiting_reply=false;
            time_out_counter_l=1;
            time_out_counter_u=0;
            /*time-out issue End*/
        }
        /*Read socket and see if anything arrived*/
        if (SREAD (m_TrackerSocket,res,MAXLINE) > 0)
        {   /*ACK from server*/
            if ( strncmp( res, "OK", 2 ) == 0 )
            {
                time_out_counter_l=1;
                time_out_counter_u=0;
                sent = true;
                waiting_reply=false;
                strcpy ( res, "" );
            }
            else if ( strncmp( res, "PING", 4 ) == 0 )
            {
                /*reply PONG*/
                time_out_counter_l=1;
                time_out_counter_u=0;
                if (!RunAsDaemon || AddDebug)
                    printf("[%d] FG_TRACKER::TrackerLoop PING from server received\n",pid);
                SWRITE (m_TrackerSocket,"PONG",4);
                strcpy ( res, "" );
            }
            else
            {
                SG_LOG (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TrackerLoop: Responce " << res << " not OK! Send again...");
            }
        }
        if (sent)
        {   // get message from queue
            length = 0;
#ifndef NO_TRACKER_PORT
#ifdef USE_TRACKER_PORT
            pthread_mutex_lock( &msg_mutex );   // acquire the lock
            pthread_cond_wait( &condition_var, &msg_mutex );    // go wait for the condition
            VI vi = msg_queue.begin(); // get first message
            if (vi != msg_queue.end())
            {
                std::string s = *vi;
                msg_queue.erase(vi);    // remove from queue
                length = (int)s.size(); // should I worry about LENGTH???
                strcpy( buf.mtext, s.c_str() ); // mtext is 1024 bytes!!!
            }
            pthread_mutex_unlock( &msg_mutex ); // unlock the mutex
#else // !#ifdef USE_TRACKER_PORT
            length = msgrcv (ipcid, &buf, MAXLINE, 0, MSG_NOERROR | IPC_NOWAIT);
            msgrcv_errno=errno;
#endif // #ifdef USE_TRACKER_PORT y/n
#endif // NO_TRACKER_PORT
            buf.mtext[length] = '\0';
#ifdef ADD_TRACKER_LOG
            if (length)
                write_msg_log(&buf.mtext[0], length, (char *)"OUT: ");
#endif // #ifdef ADD_TRACKER_LOG
        }
        if ( length > 0 )
        {
            sent = false;
            // send message via tcp
            if (waiting_reply==false)
            {
                if (!RunAsDaemon || AddDebug) 
                {
                    printf("[%d] FG_TRACKER::TrackerLoop sending msg %d bytes\n",pid,length);
                    printf("[%d] FG_TRACKER::TrackerLoop Msg: %s\n",pid,buf.mtext); 
                }
                while (SWRITE (m_TrackerSocket,buf.mtext,strlen(buf.mtext)) < 0)
                {   // FIX20120812 - re-write the failed message now before wait reply!!!
                    printf("[%d] FG_TRACKER::TrackerLoop Can't write to server...\n",pid);
                    SG_LOG (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TrackerLoop: Can't write to server...");
                    Connect ();
                }
                waiting_reply=true;
            }
        }
#ifndef USE_TRACKER_PORT
        else if (msgrcv_errno==ENOMSG)
        {/*No Message - This error Should be ignored*/
        }
#endif
        else
        {
            // an error with the queue has occured
            // avoid an infinite loop
            // return (2);
            printf("[%d] FG_TRACKER::TrackerLoop: message queue error %d\n",pid,errno);
            sent = true;
        }
    }
    return (0);
} // TrackerLoop ()
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//
//  reconnect the tracker to its server
//
//////////////////////////////////////////////////////////////////////
int
FG_TRACKER::Connect ()
{
    bool connected = false;
    pid_t pid = getpid();
    //////////////////////////////////////////////////
    // close all inherited open sockets, but
    // leave cin, cout, cerr open
    //////////////////////////////////////////////////
#ifndef _MSC_VER
    for (int i=3; i<32; i++)
        SCLOSE (i);
#endif // !_MSC_VER
    if ( m_TrackerSocket > 0 )
        SCLOSE (m_TrackerSocket);
    if (!RunAsDaemon || AddDebug)
        printf("[%d] FG_TRACKER::Connect: Server: %s, Port: %d\n",pid, m_TrackerServer, m_TrackerPort);
    while (connected == false)
    {
        m_TrackerSocket = TcpConnect (m_TrackerServer, m_TrackerPort);
        if (m_TrackerSocket >= 0)
        {
            SG_LOG (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::Connect: success");
            connected = true;
        }
        else
        {
            SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::Connect: failed! sleep " << DEF_TRACKER_SLEEP << " secs");
            sleep (DEF_TRACKER_SLEEP);  // sleep DEF_TRACKER_SLEEP secconds and try again
        }
    }
    sleep (5);
    SWRITE(m_TrackerSocket,"REPLY",sizeof("REPLY"));
    if (!RunAsDaemon || AddDebug)
        printf("[%d] FG_TRACKER::Connect: Written 'REPLY'\n",pid);
    sleep (2);
    return (0);
} // Connect ()
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
//
//  disconnect the tracker from its server
//
//////////////////////////////////////////////////////////////////////
void
FG_TRACKER::Disconnect ()
{
    if ( m_TrackerSocket > 0 )
        SCLOSE (m_TrackerSocket);
    m_TrackerSocket = 0;
} // Disconnect ()
//////////////////////////////////////////////////////////////////////

#ifdef _MSC_VER
#if !defined(NTDDI_VERSION) || !defined(NTDDI_VISTA) || (NTDDI_VERSION < NTDDI_VISTA)   // if less than VISTA, provide alternative
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT    97      /* not present in errno.h provided with VC */
#endif
int inet_aton(const char *cp, struct in_addr *addr)
{
    addr->s_addr = inet_addr(cp);
    return (addr->s_addr == INADDR_NONE) ? -1 : 0;
}
int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET)
    {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton (src, (struct in_addr *)dst);
}
#endif // #if (NTDDI_VERSION < NTDDI_VISTA)
#endif // _MSC_VER

//////////////////////////////////////////////////////////////////////
//
//  creates a TCP connection
//
//////////////////////////////////////////////////////////////////////
int
FG_TRACKER::TcpConnect (char *server_address,int server_port)
{
    struct sockaddr_in serveraddr;
    int sockfd;
    pid_t pid = getpid();
    sockfd=socket(AF_INET, SOCK_STREAM, 0);
    if (SERROR(sockfd))
    {
        SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: can't get socket...");
        return -1;
    }
    bzero(&serveraddr,sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(server_port);
#ifdef _MSC_VER
    if ( inet_pton(AF_INET, server_address, &serveraddr.sin_addr) == -1 )
    {
        SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: inet_pton failed!");
        if (!SERROR(sockfd))
            SCLOSE(sockfd);
        return -1;
    }
#else
    inet_pton(AF_INET, server_address, &serveraddr.sin_addr);
#endif
    if (connect(sockfd, (SA *) &serveraddr, sizeof(serveraddr))<0 )
    {
        if (!SERROR(sockfd))
            SCLOSE(sockfd); // close the socket
        SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: connect failed!");
        return -1;
    }
    else
    {
        if(fcntl(sockfd, F_GETFL) & O_NONBLOCK) 
        {
            // socket is non-blocking
            SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: Socket is in non-blocking mode");
        }
        else
        {
            SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: Socket is in blocking mode");
            if(fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK) < 0)
            {
                SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: FAILED to set the socket to non-blocking mode");
            }
            else
                SG_ALERT (SG_SYSTEMS, SG_ALERT, "["<< pid <<"] FG_TRACKER::TcpConnect: Socket set to non-blocking mode");
        }
        return (sockfd);
    }
}  // TcpConnect  ()
//////////////////////////////////////////////////////////////////////
// vim: ts=4:sw=4:sts=0
