/*
 *  $Id: adaptation.c,v 1.5 2003/06/25 10:32:39 ajung Exp $
 *
 * SCTP implementation according to RFC 2960.
 * Copyright (C) 2000 by Siemens AG, Munich, Germany.
 *
 * Realized in co-operation between Siemens AG
 * and University of Essen, Institute of Computer Networking Technology.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * There are two mailinglists available at http://www.sctp.de which should
 * be used for any discussion related to this implementation.
 *
 * Contact: discussion@sctp.de
 *          Michael.Tuexen@icn.siemens.de
 *          ajung@exp-math.uni-essen.de
 *
 * Purpose: The adaption-module encapsulates the socket-interface.
 *          The adaption-module also handles timers in the SCTP and its ULP.
 *
 *          Changes by  Alfred Lupper (added defines for solaris, 27/06/01) and
 *             Gerd Bohnenstengel (added new socket API (RFC2292BIS) for Solaris8
 *             and FreeBSD 4.2 KAME, 04/07/01)
 */

#include "adaptation.h"
#include "timer_list.h"

#include <stdio.h>
#include <string.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip.h>
#ifdef HAVE_IPV6
    #if defined (LINUX)
        #include <netinet/ip6.h>
    #else
        /* include files for IPv6 header structs */
    #endif
#endif
#include <netdb.h>
#include <arpa/inet.h>      /* for inet_ntoa() under both SOLARIS/LINUX */
#include <sys/errno.h>

#include <errno.h>
#include <sys/uio.h>        /* for struct iovec */
#include <sys/param.h>
#include <sys/ioctl.h>

// #include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>

#if defined (LINUX)
    #define LINUX_PROC_IPV6_FILE "/proc/net/if_inet6"
    #include <asm/types.h>
    #include <linux/rtnetlink.h>
    #ifndef HAVE_PKTINFO
    /* the definition from <bits/in.h> */
    struct in_pktinfo
    {
        int ipi_ifindex;                    /* Interface index  */
        struct in_addr ipi_spec_dst;        /* Routing destination address  */
        struct in_addr ipi_addr;            /* Header destination address  */
    };

    #endif
#else /* this may not be okay for SOLARIS !!! */
    #define USES_BSD_4_4_SOCKET
    #include <net/if.h>
    #include <net/if_var.h>
    #include <net/if_dl.h>
    #include <net/if_types.h>
    #include <net/route.h>
    #include <machine/param.h>

    #define ROUNDUP(a, size) (((a) & ((size)-1)) ? (1 + ((a) | ((size)-1))) : (a))
    #define NEXT_SA(ap) ap = (struct sockaddr *) \
        ((caddr_t) ap + (ap->sa_len ? ROUNDUP(ap->sa_len, sizeof (u_long)) : sizeof(u_long)))
#endif

#define     IFA_BUFFER_LENGTH   1024


#ifdef HAVE_SYS_POLL_H
    #include <sys/poll.h>
#else
    #define POLLIN     0x001
    #define POLLPRI    0x002
    #define POLLOUT    0x004
    #define POLLERR    0x008
#endif

#ifdef LIBRARY_DEBUG
 #define ENTER_TIMER_DISPATCHER printf("Entering timer dispatcher.\n"); fflush(stdout);
 #define LEAVE_TIMER_DISPATCHER printf("Leaving  timer dispatcher.\n"); fflush(stdout);
 #define ENTER_EVENT_DISPATCHER printf("Entering event dispatcher.\n"); fflush(stdout);
 #define LEAVE_EVENT_DISPATCHER printf("Leaving  event dispatcher.\n"); fflush(stdout);
#else
 #define ENTER_TIMER_DISPATCHER
 #define LEAVE_TIMER_DISPATCHER
 #define ENTER_EVENT_DISPATCHER
 #define LEAVE_EVENT_DISPATCHER
#endif





#define POLL_FD_UNUSED     -1
#define NUM_FDS     20

#define    EVENTCB_TYPE_SCTP        1
#define    EVENTCB_TYPE_UDP        2
#define    EVENTCB_TYPE_USER        3
#define    EVENTCB_TYPE_ROUTING    4

/**
 *  Structure for callback events. The function "action" is called by the event-handler,
 *  when an event occurs on the file-descriptor.
 */
struct event_cb
{
    int sfd;
    int eventcb_type;
    /* pointer to possible arguments, associations etc. */
    void *arg1;
    void *arg2;
    void (*action) ();
    void* userData;
};



/*
 * An extended poll() implementation based on select()
 *
 * During the select() call, another thread may change the FD list,
 * a revision number keeps track that results are only reported
 * when the FD has already been registered before select() has
 * been called. Otherwise, the event will be reported during the
 * next select() call.
 * This solves the following problem:
 * - Thread #1 registers user callback for socket n
 * - Thread #2 starts select()
 * - A read event on socket n occurs
 * - extendedPoll() returns
 * - Thread #2 sends a notification (e.g. using pthread_condition) to thread #1
 * - Thread #2 again starts select()
 * - Since Thread #1 has not yet read the data, there is a read event again
 * - Now, the thread scheduler selects the next thread
 * - Thread #1 now gets CPU time, deregisters the callback for socket n
 *      and completely reads the incoming data. There is no more data to read!
 * - Thread #1 again registers user callback for socket n
 * - Now, thread #2 gets the CPU again and can send a notification
 *      about the assumed incoming data to thread #1
 * - Thread #1 gets the read notification and tries to read. There is no
 *      data, so the socket blocks (possibily forever!) or the read call
 *      fails.
 */

static long long revision = 0;

struct extendedpollfd {
   int       fd;
   short int events;
   short int revents;
   long long revision;
};

int extendedPoll(struct extendedpollfd* fdlist,
                 int*                   count,
                 int                    time,
                 void                   (*lock)(void* data),
                 void                   (*unlock)(void* data),
                 void*                  data)
{
   struct timeval    timeout;
   struct timeval*   to;
   fd_set            readfdset;
   fd_set            writefdset;
   fd_set            exceptfdset;
   int               fdcount;
   int               n;
   int               ret;
   unsigned int i;

   if(time < 0) {
      to = NULL;
   }
   else {
      to = &timeout;
      timeout.tv_sec  = time / 1000;
      timeout.tv_usec = (time % 1000) * 1000;
   }


   /* Initialize structures for select() */
   fdcount = 0;
   n = 0;
   FD_ZERO(&readfdset);
   FD_ZERO(&writefdset);
   FD_ZERO(&exceptfdset);

   for(i = 0; i < *count; i++) {
      if(fdlist[i].fd < 0) {
         continue;
      }
      n = MAX(n,fdlist[i].fd);
      if(fdlist[i].events & (POLLIN|POLLPRI)) {
         FD_SET(fdlist[i].fd, &readfdset);
      }
      if(fdlist[i].events & POLLOUT) {
         FD_SET(fdlist[i].fd, &writefdset);
      }
      if(fdlist[i].events & (POLLIN|POLLOUT)) {
         FD_SET(fdlist[i].fd, &exceptfdset);
      }
      fdcount++;
   }


   if(fdcount == 0) {
      ret = 0;
   }
   else {
      /*
       * Set the revision number of all entries to the current revision.
       */
      for(i = 0; i < *count; i++) {
         fdlist[i].revision = revision;
      }

      /*
       * Increment the revision number by one -> New entries made by
       * another thread during select() call will get this new revision number.
       */
      revision++;


      if(unlock) {
         unlock(data);
      }

      ret = select(n + 1, &readfdset, &writefdset, &exceptfdset, to);
      
      if(lock) {
         lock(data);
      }


      for(i = 0; i < *count; i++) {
         fdlist[i].revents = 0;
         if(fdlist[i].revision >= revision) {
            FD_CLR(fdlist[i].fd, &readfdset);
            FD_CLR(fdlist[i].fd, &writefdset);
            FD_CLR(fdlist[i].fd, &exceptfdset);
         }
      }

      if(ret > 0) {
         for(i = 0; i < *count; i++) {
            fdlist[i].revents = 0;
            /*
             * If fdlist's revision is equal the current revision, then the fdlist entry
             * has been added by another thread during the poll() call. If this is the
             * case, skip the results here (they will be reported again when select()
             * is called the next time).
             */
            if(fdlist[i].revision < revision) {
               if((fdlist[i].events & POLLIN) && FD_ISSET(fdlist[i].fd, &readfdset)) {
                  fdlist[i].revents |= POLLIN;
               }
               if((fdlist[i].events & POLLOUT) && FD_ISSET(fdlist[i].fd, &writefdset)) {
                  fdlist[i].revents |= POLLOUT;
               }
               if((fdlist[i].events & (POLLIN|POLLOUT)) && FD_ISSET(fdlist[i].fd, &exceptfdset)) {
                  fdlist[i].revents |= POLLERR;
               }
            }
         }
      }
   }

   return(ret);
}



/* a static counter - for stats we should have more counters !  */
static unsigned int number_of_sendevents = 0;
/* a static receive buffer  */
static unsigned char rbuf[MAX_MTU_SIZE + 20];
/* a static value that keeps currently treated timer id */
static unsigned int current_tid = 0;


static struct extendedpollfd poll_fds[NUM_FDS];
static int num_of_fds = 0;

static int sctp_sfd = -1;       /* socket fd for standard SCTP port....      */
static int rsfd = -1;           /* socket file descriptor for routing socket */

#ifdef HAVE_IPV6
static int sctpv6_sfd = -1;
#endif

/* will be added back later....
   static int icmp_sfd = -1;  */      /* socket fd for ICMP messages */

static struct event_cb *event_callbacks[NUM_FDS];

/**
 *  converts address-string (hex for ipv6, dotted decimal for ipv4
 *  to a sockunion structure
 *  @return 0 for success, else -1.
 */
int adl_str2sockunion(guchar * str, union sockunion *su)
{
    int ret;

    memset((void*)su, 0, sizeof(union sockunion));

    ret = inet_aton(str, &su->sin.sin_addr);
    if (ret > 0) {              /* Valid IPv4 address format. */
        su->sin.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
        su->sin.sin_len = sizeof(struct sockaddr_in);
#endif                          /* HAVE_SIN_LEN */
        return 0;
    }
#ifdef HAVE_IPV6
    ret = inet_pton(AF_INET6, str, &su->sin6.sin6_addr);
    if (ret > 0) {              /* Valid IPv6 address format. */
        su->sin6.sin6_family = AF_INET6;
#ifdef SIN6_LEN
        su->sin6.sin6_len = sizeof(struct sockaddr_in6);
#endif                          /* SIN6_LEN */
        return 0;
    }
#endif                          /* HAVE_IPV6 */
    return -1;
}


const int adl_sockunion2str(union sockunion *su, guchar * buf, size_t len)
{
    if (su->sa.sa_family == AF_INET){
        if (len > 16) len = 16;
        strncpy(buf, inet_ntoa(su->sin.sin_addr), len);
        return(1);
    }
#ifdef HAVE_IPV6
    else if (su->sa.sa_family == AF_INET6) {
        if (inet_ntop(AF_INET6, &su->sin6.sin6_addr, buf, len)==NULL) return 0;
        return (1);
    }
#endif                          /* HAVE_IPV6 */
    return 0;
}

boolean adl_equal_address(union sockunion * one, union sockunion * two)
{
#ifdef HAVE_IPV6
    unsigned int count;
#endif

    switch (sockunion_family(one)) {
    case AF_INET:
        if (sockunion_family(two) != AF_INET)
            return FALSE;
        return (sock2ip(one) == sock2ip(two));
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        if (sockunion_family(two) != AF_INET6)
            return FALSE;
        for (count = 0; count < 16; count++)
            if (sock2ip6(one)[count] != sock2ip6(two)[count])
                return FALSE;
        return TRUE;
        break;
#endif
    default:
        error_logi(ERROR_MAJOR, "Address family %d not supported", sockunion_family(one));
        return FALSE;
        break;
    }
}


int adl_setReceiveBufferSize(int sfd,int new_size)
{
    int ch = new_size;
    if (setsockopt (sfd, SOL_SOCKET, SO_RCVBUF, (void*)&ch, sizeof(ch)) < 0) {
        error_log(ERROR_MAJOR, "setsockopt: SO_RCVBUF failed !");
        return -1;
    }
    event_logi(INTERNAL_EVENT_0, "set receive buffer size to : %d bytes",ch);
    return 0;
}

gint adl_open_routing_socket()
{
    int sfd;
#if defined (LINUX)
    int res;
    struct sockaddr_nl snl;

    if ((sfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE))<0) {
        error_log(ERROR_FATAL, "Opening of the routing socket failed !");
        return -1;
    } else
        event_logi(INTERNAL_EVENT_0, "got routing socket %d ",sfd);

    memset (&snl, 0, sizeof snl);
    snl.nl_family = AF_NETLINK;
    snl.nl_groups = RTMGRP_IPV4_IFADDR|RTMGRP_IPV6_IFADDR;

    /* Bind the socket to the netlink structure for anything. */
    res = bind (sfd, (struct sockaddr *) &snl, sizeof(snl));
    if (res < 0)
    {
        close (sfd);
        error_log(ERROR_FATAL, "Binding of the routing socket failed !");
        return -1;
   }
#else
    if ((sfd = socket(AF_ROUTE, SOCK_RAW, 0))<0) {
        error_log(ERROR_FATAL, "Opening of the routing socket failed !");
    }
#endif
    return sfd;

}

#if defined (LINUX)
 /* ----------------------------------------------------------------------------------*/
 /* Utility function for parse rtattr. */
 static void
 netlink_parse_rtattr (struct rtattr **tb, int max, struct rtattr *rta, int len)
 {
   while (RTA_OK(rta, len))
     {
       if (rta->rta_type <= max)
         tb[rta->rta_type] = rta;
       rta = RTA_NEXT(rta,len);
     }
 }
/* ----------------------------------------------------------------------------------*/
/* Lookup interface IPv4/IPv6 address. */
int
netlink_interface_addr (struct nlmsghdr *h, struct sockaddr* sa)
{
    int len;
    struct ifaddrmsg *ifa;
    struct rtattr *tb [IFA_MAX + 1];
    void *addr = NULL;
    void *broad = NULL;
    char *label = NULL;
    struct sockaddr_in* si = (struct sockaddr_in*)sa;
#ifdef HAVE_IPV6
    unsigned char buf[111];
    struct sockaddr_in6* si6 = (struct sockaddr_in6*)sa;
#endif /* HAVE_IPV6 */

    ifa = NLMSG_DATA (h);
    memset (si, 0, sizeof(struct sockaddr_in));

    if (ifa->ifa_family != AF_INET
#ifdef HAVE_IPV6
          && ifa->ifa_family != AF_INET6
#endif /* HAVE_IPV6 */
      )
        return -1;

    if (h->nlmsg_type != RTM_NEWADDR && h->nlmsg_type != RTM_DELADDR)
        return -1;

    len = h->nlmsg_len - NLMSG_LENGTH(sizeof (struct ifaddrmsg));
    if (len < 0)
        return -1;

    memset (tb, 0, sizeof tb);
    netlink_parse_rtattr (tb, IFA_MAX, IFA_RTA (ifa), len);

    if (tb[IFA_ADDRESS] == NULL)
        tb[IFA_ADDRESS] = tb[IFA_LOCAL];

    if (tb[IFA_ADDRESS])
        addr = RTA_DATA (tb[IFA_ADDRESS]);
    else
        addr = NULL;

    if (tb[IFA_BROADCAST])
        broad = RTA_DATA(tb[IFA_BROADCAST]);
    else
        broad = NULL;

    /* Flags. */
    if (ifa->ifa_flags & IFA_F_SECONDARY)
        event_log(VERBOSE, "netlink_interface_addr: Secondary Flag set - Maybe ignore ?");

    /* Label */
    if (tb[IFA_LABEL]) {
        label = (char *) RTA_DATA (tb[IFA_LABEL]);
        event_logi(VERBOSE, "netlink_interface_addr: Label %s set",label);
    }

    /* Register interface address to the interface. */
    if (ifa->ifa_family == AF_INET)
    {
        si->sin_family = AF_INET;
        /* port, socklen ? */
        si->sin_addr = *((struct in_addr *) addr);
        event_logi(VERBOSE, "Changed Address: %s",inet_ntoa( * ((struct in_addr*)addr) ));
        event_logi(VERBOSE, "Prefixlen set to: %d",ifa->ifa_prefixlen);
        event_logi(VERBOSE, "Broadcast-Address: %s",inet_ntoa( * ((struct in_addr*)broad) ));
        return 0;
    }
#ifdef HAVE_IPV6
    if (ifa->ifa_family == AF_INET6)
    {
      sa->sa_family = AF_INET6;
      /* si6->sin6_port = ???; */
      /* si6->sin6_flowinfo = ???; */
      memcpy(&si6->sin6_addr, addr, sizeof(struct in6_addr));
      inet_ntop(AF_INET6, &si6->sin6_addr, buf, 100);
      event_logi(VERBOSE, "Address that changed: %s",buf);
      event_logi(VERBOSE, "Prefixlen set to: %d",ifa->ifa_prefixlen);
    }
#endif /* HAVE_IPV6*/

  return 0;
}

#else   /* not LINUX */

 void
 adl_get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
 {
     int     i;

     for (i = 0; i < RTAX_MAX; i++) {
         if (addrs & (1 << i)) {
             rti_info[i] = sa;
             NEXT_SA(sa);
         } else {
             rti_info[i] = NULL;
         }
     }
 }
#endif  /* if defined (LINUX) */



void routing_socket_cb() /* a dummy function */
{
    return;
}


int adl_read_socket_event(int rsock)
{
    unsigned char buffer[IFA_BUFFER_LENGTH];

#if defined (LINUX)
    struct nlmsghdr*  nlhd;
    struct sockaddr sa;
    int res = 0;
#else
    struct sockaddr*  sa, *rti_info[RTAX_MAX];
    struct ifa_msghdr* ifa;
    ifa = (struct ifa_msghdr *) buffer;
#endif

    read(rsock, buffer, IFA_BUFFER_LENGTH);

#if defined (LINUX)
    for (nlhd = (struct nlmsghdr *) buffer; NLMSG_OK (nlhd, res); nlhd = NLMSG_NEXT (nlhd, res))
    {
        /* Finish of reading. */
        if (nlhd->nlmsg_type == NLMSG_DONE) {
            /* Got NLMSG_DONE - read all */
            break;
        }
        /* Error handling. */
        if (nlhd->nlmsg_type == NLMSG_ERROR) {
            /* NL message: ERROR */
            break;
        }
        switch (nlhd->nlmsg_type)
        {
            case RTM_NEWADDR:
                event_log (VVERBOSE, "NetLink: ADDD ADDRESS");
                res = netlink_interface_addr (nlhd, &sa);
                mdi_addressChange((union sockunion*)&sa, TRUE);
                break;
            case RTM_DELADDR:
                event_log (VVERBOSE, "NetLink: DELETE ADDRESS");
                res = netlink_interface_addr (nlhd, &sa);
                mdi_addressChange((union sockunion*)&sa, FALSE);
                break;
            case RTM_NEWROUTE:
            case RTM_DELROUTE:
            case RTM_NEWLINK:
            case RTM_DELLINK:
            default:
                event_logi (VVERBOSE, "Other netlink nlmsg_type %d", nlhd->nlmsg_type);
                break;
        }
    }
#else

    sa = (struct sockaddr *) (ifa + 1);

    adl_get_rtaddrs(ifa->ifam_addrs, sa, rti_info);

    switch(ifa->ifam_type) {
        case RTM_NEWADDR:
            event_logi(VERBOSE, "Address %s added.", inet_ntoa(((struct sockaddr_in *)rti_info[RTAX_IFA])->sin_addr));
            mdi_addressChange((union sockunion*)rti_info[RTAX_IFA], TRUE);
            break;
        case RTM_DELADDR:
            event_logi(VERBOSE, "Address %s deleted.", inet_ntoa(((struct sockaddr_in *)rti_info[RTAX_IFA])->sin_addr));
            mdi_addressChange((union sockunion*)rti_info[RTAX_IFA], FALSE);
            break;
        default:
            break;
    }
#endif
    return 0;
}


gint adl_open_sctp_socket(int af, int* myRwnd)
{
    int sfd, protoid = 0, ch;
    int opt_size;

#if defined (LINUX)
    int result;
    struct protoent *proto;

    if (!(proto = getprotobyname("sctp"))) {
        error_log(ERROR_MINOR, "SCTP: unknown protocol sctp - using 132");
        protoid = IPPROTO_SCTP;
    }

    if (!protoid)
        protoid = proto->p_proto;
#else
    protoid = IPPROTO_SCTP;
#endif  

    if ((sfd = socket(af, SOCK_RAW, protoid)) < 0) {
        return sfd;
    }
    switch (af) {
        case AF_INET:
            *myRwnd = 0;
            opt_size=sizeof(*myRwnd);
            if (getsockopt (sfd, SOL_SOCKET, SO_RCVBUF, (void*)myRwnd, &opt_size) < 0) {
                error_log(ERROR_FATAL, "getsockopt: SO_RCVBUF failed !");
                *myRwnd = -1;
            }
            event_logi(INTERNAL_EVENT_0, "receive buffer size is : %d",*myRwnd);

#if defined (LINUX)
            result = adl_setReceiveBufferSize(sfd, 10*0xFFFF);
            if (result == -1) exit(1);

            ch = 0;
            if (setsockopt(sfd, IPPROTO_IP, IP_HDRINCL, (char *) &ch, sizeof(ch))< 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_HDRINCL failed !");
            }
            ch = 1;
            if (setsockopt (sfd, IPPROTO_IP, IP_PKTINFO, (char*)&ch, sizeof (ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_PKTINFO failed !");
            }

            ch = IP_PMTUDISC_DO;
            if (setsockopt(sfd, IPPROTO_IP, IP_MTU_DISCOVER, (char *) &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_PMTU_DISCOVER failed !");
            }
#else
    #if defined (USE_RFC2292BIS)
            ch = 1;
        /* Assemble IP header myself for setting IP TOS field */
        if (setsockopt(sfd, IPPROTO_IP, IP_HDRINCL, (char *) &ch, sizeof(ch))< 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_HDRINCL failed !");
        }
        /* Once we have found out how it works, it should be listed here */
    #else
            ch = 1;
            /* Assemble IP header myself for setting IP TOS field */
            if (setsockopt(sfd, IPPROTO_IP, IP_HDRINCL, (char *) &ch, sizeof(ch))< 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_HDRINCL failed !");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IP, IP_RECVDSTADDR, (char *) &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IP_RECVDSTADDR failed !");
            }

            error_log(ERROR_MINOR, "TODO : PATH MTU Discovery Disabled For Now !!!");
    #endif /* USE_RFC2292BIS */
#endif
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
            *myRwnd = 0;
            opt_size=sizeof(*myRwnd);
            if (getsockopt (sfd, SOL_SOCKET, SO_RCVBUF, (void*)myRwnd, &opt_size) < 0) {
                error_log(ERROR_FATAL, "getsockopt: SO_RCVBUF failed !");
                *myRwnd = -1;
            }
            event_logi(INTERNAL_EVENT_0, "receive buffer size is : %d",*myRwnd);
            /* also receive packetinfo on IPv6 sockets, for getting dest address */
#if defined (USE_RFC2292BIS)
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVPKTINFO,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVPKTINFO failed");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVHOPLIMIT failed");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVRTHDR,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVRTHDR failed");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVHOPOPTS,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVHOPOPTS failed");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVDSTOPTS,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVDESTOPTS failed");
            }
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_RECVRTHDRDSTOPTS,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_RECVRTHDRDSTOPTS failed");
            }
#else
            ch = 1;
            if (setsockopt(sfd, IPPROTO_IPV6, IPV6_PKTINFO,  &ch, sizeof(ch)) < 0) {
                error_log(ERROR_FATAL, "setsockopt: IPV6_PKTINFO failed");
            }
            /* break; */
#endif /* USE_RFC2292BIS */

#endif
        default:
            error_log(ERROR_MINOR, "TODO : PATH MTU Discovery Disabled For Now !!!");
            break;
    }
    event_logi(INTERNAL_EVENT_0, "Created raw socket %d with options\n", sfd);
    return (sfd);
}


gint adl_get_sctpv4_socket(void)
{
    /* this is a static variable ! */
    return sctp_sfd;
}


#ifdef HAVE_IPV6
gint adl_get_sctpv6_socket(void)
{
    /* this is a static variable ! */
    return sctpv6_sfd;
}
#endif


/**
 * This function creates a UDP socket bound to localhost, for asynchronous
 * interprocess communication with an Upper Layer process.
 * @return the socket file descriptor. Used to register a callback function
 */
int adl_open_udp_socket(union sockunion* me)
{
    guchar buf[1000];
    int ch, sfd;

    switch (sockunion_family(me)) {
        case AF_INET:
            if ((sfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_log(ERROR_FATAL, "SCTP: socket creation failed for UDP socket !");
            }
            ch = bind(sfd, (struct sockaddr *)me, sizeof(struct sockaddr_in));
            adl_sockunion2str(me, buf, SCTP_MAX_IP_LEN);
            event_logiii(VERBOSE,
                 " adl_open_udp_socket : Create socket %u, binding to address %s, result %d",sfd, buf, ch);
            if (ch == 0)
                return (sfd);
            return -1;
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
            if ((sfd = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                error_log(ERROR_FATAL, "SCTP: socket creation failed for UDPv6 socket");
            }
            ch = bind(sfd, (struct sockaddr *)me, sizeof(struct sockaddr_in6));
            adl_sockunion2str(me, buf, SCTP_MAX_IP_LEN);
            event_logiii(VERBOSE,
                 " adl_open_udp_socket : Create socket %u, binding to address %s, result %d",sfd, buf, ch);
            if (ch == 0)
                return (sfd);
            return -1;
            break;
#endif
        default:
            return -1;
            break;
     }

}

/**
 * function to be called when we get a message from a peer sctp instance in the poll loop
 * @param  sfd the socket file descriptor where data can be read...
 * @param  buf pointer to a buffer, where we data is stored
 * @param  len number of bytes to be sent, including the ip header !
 * @param  address, where data goes from
 * @param    dest_len size of the address
 * @return returns number of bytes actually sent, or error
 */
int adl_sendUdpData(int sfd, unsigned char* buf, int length,
                     unsigned char destination[], unsigned short dest_port)
{
    union sockunion dest_su;
    int dest_len;
    int result;

    if (sfd < 0) {
        error_log(ERROR_MAJOR, "You are trying to send UDP data on an invalid fd");
        return -1;
    }

    if ((sfd == sctp_sfd)
#ifdef HAVE_IPV6
        || (sfd == sctpv6_sfd)
#endif
                               ) {
        error_log(ERROR_MAJOR, "You are trying to send UDP data on a SCTP socket");
        return -1;
    }
    result = adl_str2sockunion(destination, &dest_su);

    if (result != 0) {
        error_logi(ERROR_MAJOR, "Invalid destination address in sctp_sendUdpData(%s)",destination);
        return -1;
    }
    if (buf == NULL) {
        error_log(ERROR_MAJOR, "Invalid buffer sctp_sendUdpData()");
        return -1;
    }
    if (dest_port == 0) {
        error_log(ERROR_MAJOR, "Invalid port in sctp_sendUdpData()");
        return -1;
    }
    switch (sockunion_family(&dest_su)) {
        case AF_INET:
            dest_su.sin.sin_port = htons(dest_port);
            dest_len = sizeof(struct sockaddr_in);
            result = sendto(sfd, buf, length, 0, (struct sockaddr *) &(dest_su.sin), dest_len);
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
            dest_su.sin6.sin6_port = htons(dest_port);
            dest_len = sizeof(struct sockaddr_in6);
            result = sendto(sfd, buf, length, 0, (struct sockaddr *) &(dest_su.sin6), dest_len);
            break;
#endif
        default :
            error_logi(ERROR_MAJOR, "Invalid address family in sctp_sendUdpData(%s)",destination);
            result = -1;
            break;
    }
    return result;
}



/**
 * function to be called when library sends a message on an SCTP socket
 * @param  sfd the socket file descriptor where data will be sent
 * @param  buf pointer to a buffer, where data to be sent is stored
 * @param  len number of bytes to be sent
 * @param  destination address, where data is to be sent
 * @param    dest_len size of the address
 * @return returns number of bytes actually sent, or error
 */
int adl_send_message(int sfd, void *buf, int len, union sockunion *dest, unsigned char tos)
{
    int txmt_len = 0, dest_len;

#if !defined (LINUX)
    unsigned char sendbuf[2000];
    struct ip *iph = (struct ip*)sendbuf;
#else
    unsigned char old_tos;
    int opt_len, tmp;
    unsigned char* sendbuf = buf;
#endif

#ifdef HAVE_IPV6
    guchar hostname[MAX_MTU_SIZE];
#endif

    switch (sockunion_family(dest)) {

    case AF_INET:
        number_of_sendevents++;
#if defined (LINUX)
        opt_len = sizeof(old_tos);
        tmp = getsockopt(sfd, IPPROTO_IP, IP_TOS, &old_tos, &opt_len);
        tmp = setsockopt(sfd, IPPROTO_IP, IP_TOS, &tos, sizeof(unsigned char));
        event_logii(VVERBOSE, "adl_send_message: set IP_TOS %u, result=%d", tos,tmp);
#else
        memset(sendbuf, 0, 2000);
        iph->ip_hl = (sizeof(struct ip)/4);
        iph->ip_v = IPVERSION;
        iph->ip_tos = tos;
        iph->ip_len = sizeof(struct ip) + len;
        iph->ip_id = 0;
        iph->ip_off = 0;
        iph->ip_ttl = 0xff;
        iph->ip_p = IPPROTO_SCTP;
        iph->ip_dst.s_addr = dest->sin.sin_addr.s_addr;
        iph->ip_src.s_addr = INADDR_ANY;
        iph->ip_sum = 0;
        /* this is ugly, but it should work */
        memcpy(&(sendbuf[sizeof(struct ip)]), buf, len);
        len += sizeof(struct ip); 
        iph->ip_sum = in_check(sendbuf, len);
#endif
        event_logiiii(VERBOSE,
                     "AF_INET : adl_send_message : sfd : %d, len %d, destination : %s, send_events %u",
                     sfd, len, inet_ntoa(dest->sin.sin_addr), number_of_sendevents);
        dest_len = sizeof(struct sockaddr_in);

        /* test -- start */
        /* if ((number_of_sendevents % 30) != 0)  { */
        /* test -- stop */

        txmt_len = sendto(sfd, sendbuf, len, 0, (struct sockaddr *) &(dest->sin), dest_len);
        
        /* test -- start */
        /* } else {
           event_log(VERBOSE, "XYZ : Dropping packet instead of sending it");
           txmt_len = len;
        } */
        /* test -- stop */

        if (txmt_len < 0) {
            // perror("AF_INET : Sendto returned error : ");
            error_logi(ERROR_MAJOR, "AF_INET : sendto()=%d !", txmt_len);
            // txmt_len = sendto(sfd, sendbuf, len, 0, (struct sockaddr *) &(dest->sin), dest_len);
            // error_logi(ERROR_MAJOR, "AF_INET : Second sendto() returned %d !", txmt_len);
        }
#if defined (LINUX)
        tmp = setsockopt(sfd, IPPROTO_IP, IP_TOS, &old_tos, sizeof(unsigned char));
#endif
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        number_of_sendevents++;
        inet_ntop(AF_INET6, sock2ip6(dest), hostname, MAX_MTU_SIZE);

        event_logiiii(VVERBOSE,
                     "AF_INET6: adl_send_message : sfd : %d, len %d, destination : %s, send_events: %u",
                        sfd, len, hostname, number_of_sendevents);

        dest_len = sizeof(struct sockaddr_in6);
        txmt_len = sendto(sfd, buf, len, 0, (struct sockaddr *)&(dest->sin6), dest_len);
        if (txmt_len < 0) {
            //perror("AF_INET6 : Sendto returned error: ");
            error_logi(ERROR_MAJOR, "AF_INET6 : sendto()=%d, retrying !", txmt_len);
            txmt_len = sendto(sfd, buf, len, 0, (struct sockaddr *)&(dest->sin6), dest_len);
            error_logi(ERROR_MAJOR, "AF_INET6 : Second sendto() returned %d !", txmt_len);
        }

        break;

#endif

    default:
        error_logi(ERROR_MAJOR,
                   "adl_send_message : Adress Family %d not supported here",
                   sockunion_family(dest));
        txmt_len = -1;
    }
    return txmt_len;
}

/**
 * function to assign an event mask to a certain poll
 */
void assign_poll_fd(int fd_index, int sfd, int event_mask)
{
    if (fd_index > NUM_FDS)
        error_log(ERROR_FATAL, "FD_Index bigger than NUM_FDS ! bye !\n");

    poll_fds[fd_index].fd = sfd; /* file descriptor */
    poll_fds[fd_index].events = event_mask;
    /*
     * Set the entry's revision to the current extendedPoll() revision.
     * If another thread is currently inside extendedPoll(), extendedPoll()
     * will notify that this entry is new and skip the possibly wrong results
     * until the next invocation.
     */
    poll_fds[fd_index].revision = revision;
    poll_fds[fd_index].revents  = 0;
}


/**
 * remove a sfd from the poll_list, and shift that list to the left
 * @return number of sfd's removed...
 */
int adl_remove_poll_fd(gint sfd)
{
    int i, tmp, counter = 0;
    for (i = 0, tmp = 0; i < NUM_FDS; i++, tmp++) {
        if (tmp < NUM_FDS) {
            poll_fds[i].fd = poll_fds[tmp].fd;
            poll_fds[i].events = poll_fds[tmp].events;
            poll_fds[i].revents = poll_fds[tmp].revents;
            poll_fds[i].revision = poll_fds[tmp].revision;
            event_callbacks[i] = event_callbacks[tmp];
        } else {
            poll_fds[i].fd = POLL_FD_UNUSED;
            poll_fds[i].events = 0;
            poll_fds[i].revents = 0;
            poll_fds[i].revision = 0;
            event_callbacks[i] = NULL;
        }
        if (poll_fds[i].fd == sfd) {
            tmp = i + 1;
            if (tmp < NUM_FDS) {
                poll_fds[i].fd = poll_fds[tmp].fd;
                poll_fds[i].events = poll_fds[tmp].events;
                poll_fds[i].revents = poll_fds[tmp].revents;
                poll_fds[i].revision = poll_fds[tmp].revision;
                free(event_callbacks[i]);
                event_callbacks[i] = event_callbacks[tmp];
            } else {
                poll_fds[i].fd = POLL_FD_UNUSED;
                poll_fds[i].events = 0;
                poll_fds[i].revents = 0;
                poll_fds[i].revision = 0;
                free(event_callbacks[i]);
                event_callbacks[i] = NULL;
            }
            counter++;
            num_of_fds -= 1;
        }
    }
    return (counter);
}

/**
 * function to register a file descriptor, that gets activated for certain read/write events
 * when these occur, the specified callback funtion is activated and passed the parameters
 * that are pointed to by the event_callback struct
 */
int
adl_register_fd_cb(int sfd, int eventcb_type, int event_mask,
                   void *action, void* userData)
{
     if (num_of_fds < NUM_FDS && sfd >= 0) {
        assign_poll_fd(num_of_fds, sfd, event_mask);
        event_callbacks[num_of_fds] = malloc(sizeof(struct event_cb));
        if (!event_callbacks[num_of_fds])
            error_log(ERROR_FATAL, "Could not allocate memory in  register_fd_cb \n");
        event_callbacks[num_of_fds]->sfd = sfd;
        event_callbacks[num_of_fds]->eventcb_type = eventcb_type;
        event_callbacks[num_of_fds]->action = action;
        event_callbacks[num_of_fds]->userData = userData;
        num_of_fds++;
        return num_of_fds;
    } else
        return (-1);
}

#ifndef CMSG_ALIGN
#ifdef ALIGN
#define CMSG_ALIGN ALIGN
#else
#define CMSG_ALIGN(len) ( ((len)+sizeof(long)-1) & ~(sizeof(long)-1) )
#endif
#endif

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#endif

/**
 * function to be called when we get an sctp message. This function gives also
 * the source and destination addresses.
 *
 * @param  sfd      the socket file descriptor where data can be read...
 * @param  dest     pointer to a buffer, where we can store the received data
 * @param  maxlen   maximum number of bytes that can be received with call
 * @param  from     address, where we got the data from
 * @param  to       destination address of that message
 * @return returns number of bytes received with this call
 */
int adl_receive_message(int sfd, void *dest, int maxlen, union sockunion *from, union sockunion *to)
{
    int len;
    struct msghdr rmsghdr;
    struct cmsghdr *rcmsgp;
    struct iovec  data_vec;

#if defined (USE_RFC2292BIS)   
    struct ip *iph;
#endif    
    
    
#if defined (LINUX)
    unsigned char mbuf[(CMSG_SPACE(sizeof (struct in_pktinfo)))];
    struct in_pktinfo *pktinfo;
#else
    unsigned char mbuf[(CMSG_SPACE(sizeof (struct in_addr)))];
    struct in_addr* dptr;
#endif

#ifdef HAVE_IPV6
    unsigned char m6buf[(CMSG_SPACE(sizeof (struct in6_pktinfo)))];
    struct in6_pktinfo *pkt6info;
#endif

    len = -1;
    if ((dest == NULL) || (from == NULL) || (to == NULL)) return -1;

    data_vec.iov_base = dest;
    data_vec.iov_len  = maxlen;

    if (sfd == sctp_sfd) {
        rcmsgp = (struct cmsghdr *)mbuf;
        /* receive control msg */
        rcmsgp->cmsg_level = IPPROTO_IP;

#if defined (LINUX)
        rcmsgp->cmsg_type = IP_PKTINFO;
        rcmsgp->cmsg_len = CMSG_LEN (sizeof (struct in_pktinfo));
        pktinfo = (struct in_pktinfo *)(CMSG_DATA(rcmsgp));
#else
        rcmsgp->cmsg_type = IP_RECVDSTADDR;
        rcmsgp->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
        dptr = (struct in_addr *)(CMSG_DATA(rcmsgp));
#endif
        rmsghdr.msg_iov = &data_vec;
        rmsghdr.msg_iovlen = 1;
        rmsghdr.msg_name = (caddr_t) &from->sin;
        rmsghdr.msg_namelen = sizeof (struct sockaddr_in);
        rmsghdr.msg_control = (caddr_t) mbuf;
        rmsghdr.msg_controllen = sizeof (mbuf);

        len = recvmsg (sfd, &rmsghdr, 0);
#if defined (USE_RFC2292BIS)
        iph = (struct ip *)dest;
#endif        

        to->sa.sa_family = AF_INET;
        to->sin.sin_port = htons(0);
#if defined (LINUX)
        to->sin.sin_addr.s_addr = pktinfo->ipi_addr.s_addr;
#else
    #if defined (USE_RFC2292BIS)
    to->sin.sin_addr.s_addr = iph->ip_dst.s_addr;
    #else
        to->sin.sin_addr.s_addr = dptr->s_addr;
    #endif
#endif

    }
#ifdef HAVE_IPV6
    if (sfd == sctpv6_sfd) {
        rcmsgp = (struct cmsghdr *)m6buf;
        pkt6info = (struct in6_pktinfo *)(CMSG_DATA(rcmsgp));
        /* receive control msg */
        rcmsgp->cmsg_level = IPPROTO_IPV6;
        rcmsgp->cmsg_type = IPV6_PKTINFO;
        rcmsgp->cmsg_len = CMSG_LEN (sizeof (struct in6_pktinfo));

        rmsghdr.msg_iov = &data_vec;
        rmsghdr.msg_iovlen = 1;
        rmsghdr.msg_name =      (caddr_t) &(from->sin6);
        rmsghdr.msg_namelen =   sizeof (struct sockaddr_in6);
        rmsghdr.msg_control = (caddr_t) m6buf;
        rmsghdr.msg_controllen = sizeof (m6buf);
        memset (from, 0, sizeof (struct sockaddr_in6));
        memset (to,   0, sizeof (struct sockaddr_in6));

        len = recvmsg (sfd, &rmsghdr, 0);

        /* Linux sets this, so we reset it, as we don't want to run into trouble if
           we have a port set on sending...then we would get INVALID ARGUMENT  */
        from->sin6.sin6_port = htons(0);

        to->sa.sa_family = AF_INET6;
        to->sin6.sin6_port = htons(0);
        to->sin6.sin6_flowinfo = htonl(0);
        memcpy(&(to->sin6.sin6_addr), &(pkt6info->ipi6_addr), sizeof(struct in6_addr));
    }
#endif

    if (len < 0) error_log(ERROR_MAJOR, "recvmsg()  failed in adl_receive_message() !");

    return len;
}


/**
 * function to be called when we get a message from a peer sctp instance in the poll loop
 * @param  sfd the socket file descriptor where data can be read...
 * @param  dest pointer to a buffer, where we can store the received data
 * @param  maxlen maximum number of bytes that can be received with call
 * @param  address, where we got the data from
 * @param    from_len size of the address
 * @return returns number of bytes received with this call
 */
int adl_get_message(int sfd, void *dest, int maxlen, union sockunion *from, int *from_len)
{
    int len;

    len = recvfrom(sfd, dest, maxlen, 0, (struct sockaddr *) from, from_len);
    if (len < 0)
        error_log(ERROR_FATAL, "recvfrom  failed in get_message(), aborting !");

    return len;
}

/**
 * this function is responsible for calling the callback functions belonging
 * to all of the file descriptors that have indicated an event !
 * TODO : check handling of POLLERR situation
 * @param num_of_events  number of events indicated by poll()
 */
void dispatch_event(int num_of_events)
{
    int i = 0;
    int length=0, src_len;
    union sockunion src, dest;
    struct sockaddr_in *src_in;
    guchar src_address[SCTP_MAX_IP_LEN];
    unsigned short portnum=0;

#if !defined (LINUX)
    struct ip *iph;
#else
    struct iphdr *iph;
#endif
    int hlen=0;
    ENTER_EVENT_DISPATCHER;
    for (i = 0; i < num_of_fds; i++) {

    if (!poll_fds[i].revents)
        continue;

        if (poll_fds[i].revents & POLLERR) {
            /* We must have specified this callback funtion for treating/logging the error */
            if (event_callbacks[i]->eventcb_type == EVENTCB_TYPE_USER) {
                event_logi(VERBOSE, "Poll Error Condition on user fd %d", poll_fds[i].fd);
                ((sctp_userCallback)*(event_callbacks[i]->action)) (poll_fds[i].fd, poll_fds[i].revents, &poll_fds[i].events, event_callbacks[i]->userData);
            } else {
                error_logi(ERROR_MINOR, "Poll Error Condition on fd %d", poll_fds[i].fd);
                ((sctp_socketCallback)*(event_callbacks[i]->action)) (poll_fds[i].fd, NULL, 0, NULL, 0);
            }
        }

        if ((poll_fds[i].revents & POLLPRI) || (poll_fds[i].revents & POLLIN) || (poll_fds[i].revents & POLLOUT)) {
            if (event_callbacks[i]->eventcb_type == EVENTCB_TYPE_USER) {
                    event_logi(VERBOSE, "Activity on user fd %d - Activating USER callback", poll_fds[i].fd);
                    ((sctp_userCallback)*(event_callbacks[i]->action)) (poll_fds[i].fd, poll_fds[i].revents, &poll_fds[i].events, event_callbacks[i]->userData);

            } else if (event_callbacks[i]->eventcb_type == EVENTCB_TYPE_UDP) {
                src_len = sizeof(src);
                length = adl_get_message(poll_fds[i].fd, rbuf, MAX_MTU_SIZE, &src, &src_len);
                event_logi(VERBOSE, "Message %d bytes - Activating UDP callback", length);
                adl_sockunion2str(&src, src_address, SCTP_MAX_IP_LEN);

                switch (sockunion_family(&src)) {
                    case AF_INET :
                        portnum = ntohs(src.sin.sin_port);
                        break;
#ifdef HAVE_IPV6
                    case AF_INET6:
                        portnum = ntohs(src.sin6.sin6_port);
                        break;
#endif
                    default:
                        portnum = 0;
                        break;
                }
                ((sctp_socketCallback)*(event_callbacks[i]->action)) (poll_fds[i].fd, rbuf, length, src_address, portnum);

            } else if (event_callbacks[i]->eventcb_type == EVENTCB_TYPE_SCTP) {

                length = adl_receive_message(poll_fds[i].fd, rbuf, MAX_MTU_SIZE, &src, &dest);
                if(length < 0) break;

                event_logiiii(VERBOSE, "SCTP-Message on socket %u , len=%d, portnum=%d, sockunion family %u",
                     poll_fds[i].fd, length, portnum, sockunion_family(&src));

                switch (sockunion_family(&src)) {
                case AF_INET:
                    src_in = (struct sockaddr_in *) &src;
                    event_logi(VERBOSE, "IPv4/SCTP-Message from %s -> activating callback",
                               inet_ntoa(src_in->sin_addr));
#if !defined (LINUX)
                    iph = (struct ip *) rbuf;
                    hlen = iph->ip_hl << 2;
#else
                    iph = (struct iphdr *) rbuf;
                    hlen = iph->ihl << 2;
#endif
                    if (length < hlen) {
                        error_logii(ERROR_MINOR,
                                    "dispatch_event : packet too short (%d bytes) from %s",
                                    length, inet_ntoa(src_in->sin_addr));
                    } else {
                        length -= hlen;
                        mdi_receiveMessage(poll_fds[i].fd, &rbuf[hlen],length, &src, &dest);
                    }
                    break;
#ifdef HAVE_IPV6
                case AF_INET6:
                    adl_sockunion2str(&src, src_address, SCTP_MAX_IP_LEN);
                    /* if we have additional options, we must parse them, and deduct the sizes :-( */
                    event_logii(VERBOSE, "IPv6/SCTP-Message from %s (%d bytes) -> activating callback",
                                   src_address, length);

                    mdi_receiveMessage(poll_fds[i].fd, &rbuf[hlen],length, &src, &dest);
                    break;

#endif                          /* HAVE_IPV6 */
                default:
                    error_logi(ERROR_MAJOR, "Unsupported Address Family Type %u ", sockunion_family(&src));
                    break;

                }
            } else if (event_callbacks[i]->eventcb_type == EVENTCB_TYPE_ROUTING) {
                adl_read_socket_event(poll_fds[i].fd);
            }
        }
        poll_fds[i].revents = 0;
    }                       /*   for(i = 0; i < num_of_fds; i++) */
    LEAVE_EVENT_DISPATCHER;
}


/**
 * function calls the respective callback funtion, that is to be executed as a timer
 * event, passing it two arguments
 */
void dispatch_timer(void)
{
    int tid, result;
    AlarmTimer* event;

    ENTER_TIMER_DISPATCHER;
    if (timer_list_empty()) {
        LEAVE_TIMER_DISPATCHER;
        return;
    }
    result = get_msecs_to_nexttimer();

    if (result == 0) {  /* i.e. a timer expired */
        result = get_next_event(&event);

        tid = event->timer_id;
        current_tid = tid;

        (*(event->action)) (tid, event->arg1, event->arg2);
        current_tid = 0;

        result = remove_timer(event);
        if (result) /* this can happen for a timeout that occurs on a deleted assoc ? */
            error_logi(ERROR_MAJOR, "remove_item returned %d", result);
    }
    LEAVE_TIMER_DISPATCHER;
    return;
}


void adl_add_msecs_totime(struct timeval *t, unsigned int msecs)
{
    long seconds = 0, microseconds = 0;
    struct timeval tmp, res;
    seconds = msecs / 1000;
    microseconds = (msecs % 1000) * 1000;
    tmp.tv_sec = seconds;
    tmp.tv_usec = microseconds;

    timeradd(t, &tmp, &res);
    memcpy(t, &res, sizeof(res));
    return;
}

/**
 * helper function for the sake of a cleaner interface :-)
 */
int adl_gettime(struct timeval *tv)
{
    return (gettimeofday(tv, (struct timezone *) NULL));
}

/**
 * function is to return difference in msecs between time a and b (i.e. a-b)
 * @param a later time (e.g. current time)
 * @param b earlier time
 * @return -1 if a is earlier than b, else msecs that passed from b to a
 */
int adl_timediff_to_msecs(struct timeval *a, struct timeval *b)
{
    struct timeval result;
    int retval;
    /* result = a-b */
    timersub(a, b, &result);
    retval = result.tv_sec * 1000 + result.tv_usec / 1000;
    event_logi(VVERBOSE, "Computed Time Difference : %d msecs", retval);
    return ((retval < 0) ? -1 : retval);
}

/**
 * function initializes the array of fds we want to use for listening to events
 * USE    POLL_FD_UNUSED to differentiate between used/unused fds !
 */
int init_poll_fds(void)
{
    int i;
    for (i = 0; i < NUM_FDS; i++) {
        assign_poll_fd(i, POLL_FD_UNUSED, 0);
    }
    num_of_fds = 0;
    return (0);
}




/**
 *  function to check for events on all poll fds (i.e. open sockets), or else
 *  execute the next timer event. Executed timer events are removed from the list.
 *  Wrapper to poll() -- returns after timeout or read event
 *  @return  number of events that where seen on the socket fds, 0 for timer event, -1 for error
 *  @author  ajung, dreibh
 */
int adl_extendedEventLoop(void (*lock)(void* data), void (*unlock)(void* data), void* data)
{
    int result;
    unsigned int u_res;
    int msecs;

    if(lock != NULL) {
       lock(data);
    }

    msecs = get_msecs_to_nexttimer();

    /* returns -1 if no timer in list */
    /* if (msecs > GRANULARITY || msecs < 0) */
    if (msecs < 0)
        msecs = GRANULARITY;
    if (msecs == 0) {
        dispatch_timer();
        if(unlock != NULL) {
           unlock(data);
        }
        return (0);
    }

    /*  print_debug_list(INTERNAL_EVENT_0); */
    result = extendedPoll(poll_fds, &num_of_fds, msecs, lock, unlock, data);
    switch (result) {
    case -1:
        result = 0;
        break;
    case 0:
        dispatch_timer();
        break;
    default:
        u_res = (unsigned int) result;
        event_logi(INTERNAL_EVENT_0,
                   "############### %d Read Event(s) occurred -> dispatch_event() #############",
                   u_res);
        dispatch_event(result);
        break;
    }

    if(unlock != NULL) {
        unlock(data);
    }
    return (result);
}

/**
 *  function to check for events on all poll fds (i.e. open sockets), or else
 *  execute the next timer event. Executed timer events are removed from the list.
 *  Wrapper to poll() -- returns after timeout or read event
 *    @return  number of events that where seen on the socket fds, 0 for timer event, -1 for error
 *  @author  ajung
 *  @author  dreibh
 */
int adl_eventLoop()
{
   return(adl_extendedEventLoop(NULL, NULL, NULL));
}


int adl_extendedGetEvents(void (*lock)(void* data), void (*unlock)(void* data), void* data)
{
   int result;
   unsigned int u_res;

   if(lock != NULL) {
     lock(data);
   }
   result = extendedPoll(poll_fds, &num_of_fds, 0, lock, unlock, data);
   if(unlock != NULL) {
     unlock(data);
   }

   switch (result) {
   case -1:
      result =  0;
     break;
   case 0:
      result =  0;
    break;
   default:
      u_res = (unsigned int) result;
      event_logi(INTERNAL_EVENT_0,
                 "############### %d Read Event(s) occurred -> dispatch_event()#############",
                 u_res);
      dispatch_event(result);
      result = 1;
    break;
   }

   return (result);
}


/**
 *     function to check for events on all poll fds (i.e. open sockets), or else
 *     execute the next timer event. Executed timer events are removed from the list.
 *  Wrapper to poll() -- returns at once or after a read event
 *    @return  0 if no file descriptor event occurred, -1 for error
 *    @author  ajung
 */
int adl_getEvents(void)
{
   return(adl_extendedGetEvents(NULL, NULL, NULL));
}

int adl_init_adaptation_layer(int * myRwnd)
{
#ifdef HAVE_IPV6
    int myRwnd6 = 32767;
#endif
    init_poll_fds();
    init_timer_list();
    /*  print_debug_list(INTERNAL_EVENT_0); */
    sctp_sfd = adl_open_sctp_socket(AF_INET, myRwnd);

    /* set a safe default */
    if (*myRwnd == -1) *myRwnd = 8192;

    if (sctp_sfd < 0) return sctp_sfd;
    /* we should - in a later revision - add back the a function that opens
       appropriate ICMP sockets (IPv4 and/or IPv6) and registers these with
       callback functions that also set PATH MTU correctly */
#ifdef HAVE_IPV6
    /* icmpv6_sfd = int adl_open_icmpv6_socket(); */
    sctpv6_sfd = adl_open_sctp_socket(AF_INET6, &myRwnd6);
    if (sctpv6_sfd < 0) {
        error_log(ERROR_MAJOR, "Could not open IPv6 socket - running IPv4 only !");
        sctpv6_sfd = -1;
    }

    /* adl_register_socket_cb(icmpv6_sfd, adl_icmpv6_cb); */

    /* set a safe default */
    if (myRwnd6 == -1) *myRwnd = 8192;
#endif

    rsfd = adl_open_routing_socket();
    if (rsfd < 0) {
        error_log(ERROR_MAJOR, "Could not open routing socket ! Did you exec <modprobe af_packet> under linux ?!");
        close (sctp_sfd);
#ifdef HAVE_IPV6
        close (sctpv6_sfd);
#endif
        return rsfd;
    }

    adl_register_fd_cb(rsfd, EVENTCB_TYPE_ROUTING, POLLIN | POLLPRI, routing_socket_cb, NULL);

    /* icmp_sfd = int adl_open_icmp_socket(); */
    /* adl_register_socket_cb(icmp_sfd, adl_icmp_cb); */

/* #if defined(HAVE_SETUID) && defined(HAVE_GETUID) */
     /* now we could drop privileges, if we did not use setsockopt() calls for IP_TOS etc. later */
     /* setuid(getuid()); */
/* #endif   */
     return 0;
}


/**
 * this function is supposed to open and bind a UDP socket listening on a port
 * to incoming udp pakets on a local interface (a local union sockunion address)
 * @param  me   pointer to a local address, that will trigger callback, if it receives UDP data
 * @param  scf  callback funtion that is called when data has arrived
 * @return new UDP socket file descriptor, or -1 if error ocurred
 */
int adl_registerUdpCallback(unsigned char me[],
                             unsigned short my_port,
                             sctp_socketCallback scf)
{
    int result, new_sfd;
    union sockunion my_address;

    if (ntohs(my_port) == 0) {
        error_log(ERROR_MAJOR, "Port 0 is not allowed ! Fix your program !");
        return -1;
    }
    if (adl_str2sockunion(me, &my_address) < 0) {
        error_logi(ERROR_MAJOR, "Could not convert address string %s !", me);
        return -1;
    }

    switch (sockunion_family(&my_address)) {
        case AF_INET:
            event_logi(VERBOSE, "Registering ULP-Callback for UDP socket on port %u",ntohs(my_port));
            my_address.sin.sin_port = htons(my_port);
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
            event_logi(VERBOSE, "Registering ULP-Callback for UDPv6 socket on port %u",ntohs(my_port));
            my_address.sin6.sin6_port = htons(my_port);
            break;
#endif
        default:
            error_log(ERROR_MINOR, "UNKNOWN ADDRESS TYPE - CHECK YOUR PROGRAM !");
            break;
    }

    new_sfd = adl_open_udp_socket(&my_address);

    if (new_sfd != -1) {
        result = adl_register_fd_cb(new_sfd, EVENTCB_TYPE_UDP, POLLIN | POLLPRI, scf, NULL);
        event_logi(INTERNAL_EVENT_0, "Registered ULP-Callback: now %d registered callbacks !!!",result);
        return  new_sfd;
    }
    return -1;
}



int adl_unregisterUdpCallback(int udp_sfd)
{
    if (udp_sfd <= 0) return -1;
    if (udp_sfd == sctp_sfd) return -1;
#ifdef HAVE_IPV6
    if (udp_sfd == sctpv6_sfd) return -1;
#endif
    return adl_remove_cb(udp_sfd);
}


/**
 * this function is supposed to register a callback function for catching
 * input from the Unix STDIN file descriptor. We expect this to be useful
 * in test programs mainly, so it is provided here for convenience.
 * @param  scf  callback funtion that is called (when return is hit)
 * @return 0, or -1 if error ocurred
 */
int adl_registerUserCallback(int fd, sctp_userCallback sdf, void* userData, short int eventMask)
{
    int result;
    /* 0 is the standard input ! */
    result = adl_register_fd_cb(fd, EVENTCB_TYPE_USER, eventMask, sdf, userData);
    if (result != -1) {
        event_logii(EXTERNAL_EVENT,"----------> Registered User Callback: fd=%d result=%d -------\n", fd, result);
    }
    return result;
}



int adl_unregisterUserCallback(int fd)
{
    adl_remove_poll_fd(fd);
    return 0;
}


int
adl_register_socket_cb(gint sfd, sctp_socketCallback scf)
{
    return (adl_register_fd_cb(sfd, EVENTCB_TYPE_SCTP, POLLIN | POLLPRI, scf, NULL));
}


/**
 *      This function adds a callback that is to be called some time from now. It realizes
 *      the timer (in an ordered list).
 *      @param      milliseconds  action is to be started in milliseconds ms from now
 *      @param      action        pointer to a function to be executed, when timer goes off
 *      @return     returns an id value, that can be used to cancel a timer
 *      @author     ajung
 */
unsigned int adl_startMicroTimer(unsigned int seconds, unsigned int microseconds,
                            sctp_timerCallback timer_cb, int ttype, void *param1, void *param2)
{
    unsigned int result = 0;
    int res = 0;
    AlarmTimer* item;
    struct timeval talarm, delta, now;

    delta.tv_sec = seconds;
    /* make sure, user cannot confuse us :-) */
    delta.tv_sec += (microseconds / 1000000);  /* usually 0 */
    delta.tv_usec = (microseconds % 1000000);

#ifdef HAVE_GETTIMEOFDAY
    res = gettimeofday(&now, NULL);
#endif
    item = malloc(sizeof(AlarmTimer));
    if (item == NULL) return 0;

    timeradd(&now, &delta, &talarm);
    item->timer_type = ttype;
    item->action_time = talarm;
    item->action = timer_cb;
    item->arg1 = param1;
    item->arg2 = param2;
    result = insert_item(item);

    return (result);
}

unsigned int
adl_startTimer(unsigned int milliseconds, sctp_timerCallback timer_cb, int ttype,
                void *param1, void *param2)
{
    unsigned int secs, usecs;
    unsigned int result = 0;

    secs = milliseconds / 1000;
    usecs =  (milliseconds - (secs * 1000))*1000;
    result = adl_startMicroTimer(secs, usecs, timer_cb, ttype, param1, param2);
    return result;
}

/**
 *      This function adds a callback that is to be called some time from now. It realizes
 *      the timer (in an ordered list).
 *      @param      tid        timer-id of timer to be removed
 *      @return     returns 0 on success, 1 if tid not in the list, -1 on error
 *      @author     ajung
 */
int adl_stopTimer(unsigned int tid)
{
    if (tid != current_tid)
        return (remove_item(tid));
    else
        return 0;
}

/**
 *      Restarts a timer currently running
 *      @param      timer_id   the value returned by set_timer for a certain timer
 *      @param      milliseconds  action is to be started in milliseconds ms from now
 *      @return     new timer id , zero when there is an error (i.e. no timer)
 *      @author     ajung
 */
unsigned int adl_restartTimer(unsigned int timer_id, unsigned int milliseconds)
{
    unsigned int result;
    result = update_item(timer_id, milliseconds);
    event_logiii(VVERBOSE,
                 "Restarted Timer : timer_id = %u, msecs = %u, result = %u",
                 timer_id, milliseconds, result);
    return result;
}

unsigned int adl_restartMicroTimer(unsigned int timer_id, unsigned int seconds, unsigned int microseconds)
{
    unsigned int result;
    result = micro_update_item(timer_id, seconds, microseconds);
    event_logiiii(VVERBOSE,
                 "Restarted Micro-Timer : timer_id = %u, secs = %u, usecs=%u result = %u",
                 timer_id, seconds, microseconds, result);
    return result;

}


/**
 *    function to close a bound socket from our list of socket descriptors
 *    @param    sfd    socket file descriptor to be closed
 *    @return  0 on success, -1 for error, 1 if socket was not bound
 *    @author  ajung
 */
int adl_remove_cb(int sfd)
{
    int result;
    result = close(sfd);
    if (result < 0)
        error_log(ERROR_FATAL, "Close Socket resulted in an error");
    adl_remove_poll_fd(sfd);
    return result;
}

/**
 * An address filtering function
 * @param newAddress  a pointer to a sockunion address
 * @param flags       bit mask hiding (i.e. filtering) address classes
 * returns TRUE if address is not filtered, else FALSE if address is filtered by mask
 */
gboolean adl_filterInetAddress(union sockunion* newAddress, AddressScopingFlags  flags)
{
    switch (sockunion_family(newAddress)) {
        case AF_INET :
            event_log(VERBOSE, "Trying IPV4 address");
            if (
                (IN_MULTICAST(ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideMulticast)) ||
                (IN_EXPERIMENTAL(ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideReserved)) ||
                (IN_BADCLASS(ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideReserved)) ||
                ((INADDR_BROADCAST == ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideBroadcast))||
                ((INADDR_LOOPBACK == ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideLoopback)) ||
                ((INADDR_LOOPBACK != ntohl(newAddress->sin.sin_addr.s_addr)) && (flags & flag_HideAllExceptLoopback))||
		(ntohl(newAddress->sin.sin_addr.s_addr) == INADDR_ANY)
                ) {
            event_log(VERBOSE, "Filtering IPV4 address");
            return FALSE;
         }
         break;
#ifdef HAVE_IPV6
      case AF_INET6 :
 #if defined (LINUX)
        if (
            (!IN6_IS_ADDR_LOOPBACK(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideAllExceptLoopback)) ||
            (IN6_IS_ADDR_LOOPBACK(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideLoopback)) ||
            (IN6_IS_ADDR_LINKLOCAL(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideLinkLocal)) ||
            (!IN6_IS_ADDR_LINKLOCAL(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideAllExceptLinkLocal)) ||
            (!IN6_IS_ADDR_SITELOCAL(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideAllExceptSiteLocal)) ||
            (IN6_IS_ADDR_SITELOCAL(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideSiteLocal)) ||
            (IN6_IS_ADDR_MULTICAST(&(newAddress->sin6.sin6_addr.s6_addr)) && (flags & flag_HideMulticast)) ||
             IN6_IS_ADDR_UNSPECIFIED(&(newAddress->sin6.sin6_addr.s6_addr))
                 ) {
            event_log(VERBOSE, "Filtering IPV6 address");
            return FALSE;
        }
 #else
        if (
            (!IN6_IS_ADDR_LOOPBACK(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideAllExceptLoopback)) ||
            (IN6_IS_ADDR_LOOPBACK(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideLoopback)) ||
            (!IN6_IS_ADDR_LINKLOCAL(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideAllExceptLinkLocal)) ||
            (!IN6_IS_ADDR_SITELOCAL(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideAllExceptSiteLocal)) ||
            (IN6_IS_ADDR_LINKLOCAL(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideLinkLocal)) ||
            (IN6_IS_ADDR_SITELOCAL(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideSiteLocal)) ||
            (IN6_IS_ADDR_MULTICAST(&(newAddress->sin6.sin6_addr)) && (flags & flag_HideMulticast)) ||
             IN6_IS_ADDR_UNSPECIFIED(&(newAddress->sin6.sin6_addr))
                 ) {
            event_log(VERBOSE, "Filtering IPV6 address");
            return FALSE;
        }
 #endif
         break;
#endif
      default :
        event_log(VERBOSE, "Default : Filtering Address");
        return FALSE;
        break;
    }
    return TRUE;
}


/*
 * this is an ugly part to code, so it was taken an adapted from the
 * SCTP reference implementation by Randy Stewart
 * see http://www.sctp.org
 * returns TRUE is successful, else FALSE
 *
 * Changed by Stefan Jansen <stefan.jansen@gmx.de>, Aug 1st, 2002.
 * When going through the ifreq array, numAlocAddr was used as upper bound.
 * But at this time numAlocAddr counts also IPv6 addresses from
 * /proc/net/if_inet6 and is therefore too much. Thus I introduced a new
 * counter named numAlocIPv4Addr.
 * This error lead to a kernel error message because the kernel tried to load
 * a kernel module when the non-existing network devices were accessed on
 * SuSE Linux 7.3, kernel 2.4.16-4GB, GCC 2.95.3, glibc-2.2.4-64.
 */
gboolean adl_gatherLocalAddresses(union sockunion **addresses,
     int *numberOfNets,
     int sctp_fd,
     gboolean with_ipv6,
     int *max_mtu,
     const AddressScopingFlags  flags)

{
#if defined (LINUX)
    int addedNets;
    char addrBuffer[256];
    FILE *v6list;
    struct sockaddr_in6 sin6;
    int numAlocIPv4Addr = 0;
#endif

    char addrBuffer2[64];
    /* unsigned short intf_flags; */
    struct ifconf cf;
    int pos=0,copSiz=0,numAlocAddr=0,ii;
    char buffer[8192];
    struct sockaddr *toUse;
    int saveMTU = 1500; /* default maximum MTU for now */
#ifdef HAS_SIOCGLIFADDR
    struct if_laddrreq lifaddr;
#endif
    struct ifreq local;
    struct ifreq *ifrequest,*nextif;
    int dup,xxx,tmp;
    union sockunion * localAddresses = NULL;

    cf.ifc_buf = buffer;
    cf.ifc_len = 8192;
    *max_mtu = 0;
    *numberOfNets = 0;

    /* Now gather the master address information */
    if(ioctl(sctp_fd, SIOCGIFCONF, (char *)&cf) == -1) {
        return(FALSE);
    }

#ifdef USES_BSD_4_4_SOCKET
    for (pos = 0; pos < cf.ifc_len; ) {
        ifrequest = (struct ifreq *)&buffer[pos];
        pos += (ifrequest->ifr_addr.sa_len + sizeof(ifrequest->ifr_name));
        if (ifrequest->ifr_addr.sa_len == 0) {
            /* if the interface has no address then you must
             * skip at a minium a sockaddr structure
             */
            pos += sizeof(struct sockaddr);
        }
        numAlocAddr++;
    }
#else
    numAlocAddr = cf.ifc_len / sizeof(struct ifreq);
    /* ????????????  numAlocAddr++; */
    ifrequest = cf.ifc_req;
#endif
#if defined  (LINUX)
    numAlocIPv4Addr = numAlocAddr;
    addedNets = 0;
    v6list = fopen(LINUX_PROC_IPV6_FILE,"r");
    if (v6list != NULL) {
        while(fgets(addrBuffer,sizeof(addrBuffer),v6list) != NULL){
            addedNets++;
        }
        fclose(v6list);
    }
    numAlocAddr += addedNets;
    event_logii(VERBOSE, "Found additional %d v6 addresses, total now %d\n",addedNets,numAlocAddr);
#endif
    /* now allocate the appropriate memory */
    localAddresses = calloc(numAlocAddr,sizeof(union sockunion));

    if(localAddresses == NULL){
        error_log(ERROR_MAJOR, "Out of Memory in adl_gatherLocalAddresses() !");
        return(FALSE);
    }

     pos = 0;
     /* Now we go through and pull each one */

#if defined (LINUX)
    v6list = fopen(LINUX_PROC_IPV6_FILE,"r");
    if(v6list != NULL){
        memset((char *)&sin6,0,sizeof(sin6));
        sin6.sin6_family = AF_INET6;

        while(fgets(addrBuffer,sizeof(addrBuffer),v6list) != NULL){
            if(strncmp(addrBuffer,"00000000000000000000000000000001",32) == 0) {
                event_log(VVERBOSE, "At least I found the local IPV6 address !");
                if(inet_pton(AF_INET6,"::1",(void *)&sin6.sin6_addr) > 0){
                    sin6.sin6_family = AF_INET6;
                    memcpy(&((localAddresses)[*numberOfNets]),&sin6,sizeof(sin6));
                    event_logiiiii(VVERBOSE, "copied the local IPV6 address %x:%x:%x:%x, family %x",
                        sin6.sin6_addr.s6_addr32[3], sin6.sin6_addr.s6_addr32[2], sin6.sin6_addr.s6_addr32[1],
                        sin6.sin6_addr.s6_addr32[0], sin6.sin6_family);
                    (*numberOfNets)++;
                }
                continue; 
            }
            memset(addrBuffer2,0,sizeof(addrBuffer2));
            strncpy(addrBuffer2,addrBuffer,4);
            addrBuffer2[4] = ':';
            strncpy(&addrBuffer2[5],&addrBuffer[4],4);
            addrBuffer2[9] = ':';
            strncpy(&addrBuffer2[10],&addrBuffer[8],4);
            addrBuffer2[14] = ':';
            strncpy(&addrBuffer2[15],&addrBuffer[12],4);
            addrBuffer2[19] = ':';
            strncpy(&addrBuffer2[20],&addrBuffer[16],4);
            addrBuffer2[24] = ':';
            strncpy(&addrBuffer2[25],&addrBuffer[20],4);
            addrBuffer2[29] = ':';
            strncpy(&addrBuffer2[30],&addrBuffer[24],4);
            addrBuffer2[34] = ':';
            strncpy(&addrBuffer2[35],&addrBuffer[28],4);

            if(inet_pton(AF_INET6,addrBuffer2,(void *)&sin6.sin6_addr) > 0){
                memcpy(&((localAddresses)[*numberOfNets]),&sin6,sizeof(sin6));
                (*numberOfNets)++;
            }else{
                error_logi(ERROR_FATAL, "Could not translate string %s",addrBuffer2);
            }
        }
        fclose(v6list);
    }
#endif

    /* set to the start, i.e. buffer[0] */
    ifrequest = (struct ifreq *)&buffer[pos];

#if defined (LINUX)
    for(ii=0; ii < numAlocIPv4Addr; ii++,ifrequest=nextif){
#else
    for(ii=0; ii < numAlocAddr; ii++,ifrequest=nextif){
#endif
#ifdef USES_BSD_4_4_SOCKET
        /* use the sa_len to calculate where the next one will be */
        pos += (ifrequest->ifr_addr.sa_len + sizeof(ifrequest->ifr_name));

        if (ifrequest->ifr_addr.sa_len == 0){
            /* if the interface has no address then you must
             * skip at a minium a sockaddr structure
             */
            pos += sizeof(struct sockaddr);
        }
        nextif = (struct ifreq *)&buffer[pos];
#else
        nextif = ifrequest + 1;
#endif

#ifdef _NO_SIOCGIFMTU_
        *max_mtu = DEFAULT_MTU_CEILING;
#else
        memset(&local, 0, sizeof(local));
        memcpy(local.ifr_name,ifrequest->ifr_name,IFNAMSIZ);
        event_logiii(VERBOSE, "Interface %d, NAME %s, Hex: %x",ii,local.ifr_name,local.ifr_name);

        if (ioctl(sctp_fd, SIOCGIFMTU, (char *)&local) == -1) {
            /* cant get the flags? */
            continue;
        }
        saveMTU = local.ifr_mtu;
        event_logii(VERBOSE, "Interface %d, MTU %d",ii,saveMTU);
#endif
        toUse = &ifrequest->ifr_addr;

        adl_sockunion2str((union sockunion*)toUse, addrBuffer2, SCTP_MAX_IP_LEN);
        event_logi(VERBOSE, "we are talking about the address %s", addrBuffer2);


        memset(&local, 0, sizeof(local));
        memcpy(local.ifr_name, ifrequest->ifr_name, IFNAMSIZ);

        if(ioctl(sctp_fd, SIOCGIFFLAGS, (char *)&local) == -1){
            /* can't get the flags, skip this guy */
            continue;
        }
        /* Ok get the address and save the flags */
        /*        intf_flags = local.ifr_flags; */

        if (flags & flag_HideLoopback){
            if (adl_filterInetAddress((union sockunion*)toUse, flag_HideLoopback) == FALSE){
                /* skip the loopback */
                event_logi(VERBOSE, "Interface %d, skipping loopback",ii);
                continue;
            }
        }
        if (adl_filterInetAddress((union sockunion*)toUse, flag_HideReserved) == FALSE) {
            /* skip reserved */
            event_logi(VERBOSE, "Interface %d, skipping reserved",ii);
            continue;
        }

        if(toUse->sa_family== AF_INET){
            copSiz = sizeof(struct sockaddr_in);
        } else if (toUse->sa_family == AF_INET6){
            copSiz = sizeof(struct sockaddr_in6);
        }
        if (*max_mtu < saveMTU) *max_mtu = saveMTU;

         /* Now, we may have already gathered this address, if so skip
          * it
          */
        event_logii(VERBOSE, "Starting checking for duplicates ! MTU = %d, nets: %d",saveMTU, *numberOfNets);

        if(*numberOfNets){
            tmp = *numberOfNets;
            dup = 0;
            /* scan for the dup */
            for(xxx=0; xxx < tmp; xxx++) {
                event_logi(VERBOSE, "duplicates loop xxx=%d",xxx);
                /* family's must match */
                if ((&(localAddresses[xxx]))->sa.sa_family != toUse->sa_family) {
                    continue;
                }

                if ((&(localAddresses[xxx]))->sa.sa_family == AF_INET){
                    event_logi(VERBOSE, "Tested address is Family AF_INET, %x", ((struct sockaddr_in *)(toUse))->sin_addr.s_addr);

                    if ( ((struct sockaddr_in *)(toUse))->sin_addr.s_addr ==
                        (&(localAddresses[xxx]))->sin.sin_addr.s_addr){
                        /* set the flag and break, it is a dup */
                        event_logi(VERBOSE, "Interface %d, found duplicate AF_INET",ii);
                        dup = 1;
                        break;
                    }
#ifdef HAVE_IPV6
                } else {
                    if(IN6_ARE_ADDR_EQUAL(&(((struct sockaddr_in6 *)(toUse))->sin6_addr),
                       &((&(localAddresses[xxx]))->sin6.sin6_addr))){
                        /* set the flag and break, it is a dup */
                        dup = 1;
                        event_logi(VERBOSE, "Interface %d, found duplicate AF_INET6",ii);
                        break;
                    }
#endif
                }

            }
            if(dup){
                /* skip the duplicate name/address we already have it*/
                continue;
            }
        }
        /* copy address */
                event_logi(VVERBOSE, "Copying %d bytes",copSiz);
        memcpy(&localAddresses[*numberOfNets],(char *)toUse,copSiz);
                event_log(VVERBOSE, "Setting Family");
        /* set family */
        (&(localAddresses[*numberOfNets]))->sa.sa_family = toUse->sa_family;

#ifdef USES_BSD_4_4_SOCKET
        /* copy the length */
        (&(localAddresses[*numberOfNets]))->sa.sa_len = toUse->sa_len;
#endif
        (*numberOfNets)++;
        event_logii(VERBOSE, "Interface %d, Number of Nets: %d",ii, *numberOfNets);
    }

    event_logi(VERBOSE, "adl_gatherLocalAddresses: Found %d addresses", *numberOfNets);
    for(ii = 0; ii < (*numberOfNets); ii++) {
        adl_sockunion2str(&(localAddresses[ii]), addrBuffer2, SCTP_MAX_IP_LEN);
        event_logii(VERBOSE, "adl_gatherAddresses : Address %d: %s",ii, addrBuffer2);

    }
    *addresses = localAddresses;
    return(TRUE);
}

