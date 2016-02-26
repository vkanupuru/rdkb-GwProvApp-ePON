#define _GW_PROV_EPON_SM_C_

/*! \file gw_prov_epon_sm.c
    \brief gw epon provisioning
*/

/**************************************************************************/
/*      INCLUDES:                                                         */
/**************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ruli.h>
#include <sysevent/sysevent.h>
#include <syscfg/syscfg.h>
#include <pthread.h>
#include "stdbool.h"
#include "gw_prov_epon.h"

#ifdef HAVE_NETLINK_SUPPORT
#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/time.h>

static pthread_t netlink_tid;
#endif

/**************************************************************************/
/*      LOCAL VARIABLES:                                                  */
/**************************************************************************/

static int  factory_mode = 0;
static EPON_IpProvMode routerIpModeOverride = IpProvModeHonor;

static int sysevent_fd;
static token_t sysevent_token;
static int sysevent_fd_gs;
static token_t sysevent_token_gs;
static pthread_t sysevent_tid;

#define INFO  0
#define WARNING  1
#define ERROR 2

#ifdef _RDKLOG
#define GWPROVEPONLOG(x, ...) { if((x)==(INFO)){CcspTraceInfo((__VA_ARGS__));}else if((x)==(WARNING)){CcspTraceWarning((__VA_ARGS__));}else if((x)==(ERROR)){CcspTraceError((__VA_ARGS__));} }
#else
#define GWPROVEPONLOG(x, ...) {fprintf(stderr, "GwProvEponLog<%s:%d> ", __FUNCTION__, __LINE__);fprintf(stderr, __VA_ARGS__);}
#endif

#define IF_WANBRIDGE "wanbridge"
#define _DEBUG 1
#define THREAD_NAME_LEN 16 //length is restricted to 16 characters, including the terminating null byte

/**************************************************************************/
/*      LOCAL FUNCTIONS:                                                  */
/**************************************************************************/
static void GWPEpon_StartIPProvisioning();
static void GWPEpon_StopIPProvisioning();
static int GWPEpon_SysCfgSetInt(const char *name, int int_value);


#ifdef HAVE_NETLINK_SUPPORT
/*
*********************************************************************************
**  Function Name: netlink_read_event
**
**  PURPOSE:
**     .
**
**  PARAMETERS:
**      1. int sockint
**
**  RETURNS:
**     None.
**
**  NOTES:
**     None.
**
*********************************************************************************
*/
static int netlink_read_event (int sockint)
{
    int status;
    int ret = 0;
    char buf[4096];
    struct iovec iov = { buf, sizeof(buf) };
    struct sockaddr_nl snl;
    struct msghdr msg = { (void *) &snl, sizeof snl, &iov, 1, NULL, 0, 0 };
    struct nlmsghdr *h;
    struct ifinfomsg *ifi;
    struct ifaddrmsg *ifa;

    char netLinkStatus[12];
    unsigned int if_index = 0;
    char epon_ifname[32];

    status = recvmsg (sockint, &msg, 0);

    if (status < 0)
    {
        /* Socket non-blocking so bail out once we have read everything */
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return ret;

        /* Anything else is an error */
        GWPROVEPONLOG(ERROR, "read_netlink: Error recvmsg: %d\n", status);
        return status;
    }

    if (status == 0)
    {
        GWPROVEPONLOG(INFO, "read_netlink: EOF\n");
    }

    // We need to handle more than one message per 'recvmsg'
    for (h = (struct nlmsghdr *) buf; NLMSG_OK (h, (unsigned int) status);
       h = NLMSG_NEXT (h, status))
    {
        //Finish reading 
        if (h->nlmsg_type == NLMSG_DONE)
            return ret;

        // Message is some kind of error 
        if (h->nlmsg_type == NLMSG_ERROR)
        {
            GWPROVEPONLOG(ERROR, "read_netlink: Message is an error - decode TBD\n");
            return -1;        // Error
        }

        //get the interface index

        epon_ifname[0] = '\0';

        sysevent_get(sysevent_fd_gs, sysevent_token_gs, "epon_ifname", epon_ifname, sizeof(epon_ifname));

        if (strlen(epon_ifname) != 0)
            if_index = if_nametoindex(epon_ifname);
        else
            if_index = if_nametoindex(IF_WANBRIDGE);

        ifa = NLMSG_DATA (h);

        if (ifa->ifa_index == if_index)
        {
            switch(h->nlmsg_type)
            {
                case RTM_NEWLINK:
                    sysevent_set(sysevent_fd_gs, sysevent_token_gs, "epon_ifstatus", "up", 0);      	
                break;

                case RTM_DELLINK:
                    sysevent_set(sysevent_fd_gs, sysevent_token_gs, "epon_ifstatus", "down", 0);      	
                break;

                case RTM_NEWADDR:
                    if( ifa->ifa_family & AF_INET6 )
                        sysevent_set(sysevent_fd_gs, sysevent_token_gs, "ipv6-status", "up", 0);      	
                    else
                        sysevent_set(sysevent_fd_gs, sysevent_token_gs, "ipv4-status", "up", 0);      	
                break;

                case RTM_DELADDR:
                    if( ifa->ifa_family & AF_INET6 )
                        sysevent_set(sysevent_fd_gs, sysevent_token_gs, "ipv6-status", "down", 0);      	
                    else
                        sysevent_set(sysevent_fd_gs, sysevent_token_gs, "ipv4-status", "down", 0);      	
                break;
            }

            GWPROVEPONLOG(WARNING, "%s interface status modified with %s\n", IF_WANBRIDGE, netLinkStatus);
        }
    }

    return ret;
}

/*
*********************************************************************************
**  Function Name: GWPEpon_netlink_handler
**
**  PURPOSE:
**     .
**
**  PARAMETERS:
**      1. void *data
**
**  RETURNS:
**     None.
**
**  NOTES:
**     None.
**
*********************************************************************************
*/
static void *GWPEpon_netlink_handler(void *data)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    fd_set rfds, wfds;
    struct timeval tv;
    int retval;
    struct sockaddr_nl addr;

    int nl_socket = socket (AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl_socket < 0)
    {
        GWPROVEPONLOG(ERROR, "Netlink socket open error!")
    }
    else
    {
        memset ((void *) &addr, 0, sizeof (addr));

        addr.nl_family = AF_NETLINK;
        addr.nl_pid = getpid ();
        addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

        if (bind (nl_socket, (struct sockaddr *) &addr, sizeof (addr)) < 0)
        {
            GWPROVEPONLOG(ERROR, "Netlink Socket bind failed!\n");
        }
        else
        {
            while (1)
            {
                FD_ZERO (&rfds);
                FD_CLR (nl_socket, &rfds);
                FD_SET (nl_socket, &rfds);

                tv.tv_sec = 10;
                tv.tv_usec = 0;

                retval = select (FD_SETSIZE, &rfds, NULL, NULL, &tv);
                if (retval == -1)
                    GWPROVEPONLOG(ERROR, "Netlink select() error!\n");
                else if (retval)
                {
                    GWPROVEPONLOG(WARNING, "Netlink event recieved\n");
                    netlink_read_event (nl_socket);
                }
                //else
                //printf ("## Select TimedOut ## \n");
            }
        } 
    }   

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}
#endif //#ifdef HAVE_NETLINK_SUPPORT

/**************************************************************************/
/*! \fn int SetProvisioningStatus();
 **************************************************************************
 *  \brief Set Epon Provisioing Status
 *  \return 0
**************************************************************************/
static void SetProvisioningStatus(EPON_IpProvStatus status)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    const char * ip_status[] = { "None", "Ipv6", "NoIpv6", "Ipv4", "NoIpv4"};
    unsigned  char value[20]; //use unsigned char all over
    value[0] = '\0' ;

    int prov_status = 0;
    prov_status = GWPEpon_SyseventGetInt("prov_status");

    switch(status)
    {
        case EPON_OPER_IPV6_UP:
            prov_status |= 0x00000001;
        break;

        case EPON_OPER_IPV6_DOWN:
            prov_status &= ~(0x00000001);
        break;

        case EPON_OPER_IPV4_UP:
            prov_status |= 0x00000002;
        break;

        case EPON_OPER_IPV4_DOWN:
            prov_status &= ~(0x00000002);
        break;

        case EPON_OPER_NONE:
            prov_status = 0x00000000;
        break;
		
        default:
        break;
    }
	
    if (prov_status & 0x00000001) 
    {
        strcat(value, ip_status[EPON_OPER_IPV6_UP]);
    }
    else
    {
        strcat(value, ip_status[EPON_OPER_IPV6_DOWN]);
    }
	
    if (prov_status & 0x00000002) 
    {
        strcat(value, ip_status[EPON_OPER_IPV4_UP]);
    }
    else
    {
        strcat(value, ip_status[EPON_OPER_IPV4_DOWN]);
    }

    GWPEpon_SysCfgSetInt("factory_mode", 0); // We have been provisioned
    GWPEpon_SyseventSetInt("prov_status",prov_status);
    GWPROVEPONLOG(INFO, "epon_prov_status=%s %d\n",value, prov_status)
    //TODO:To be added in EPON gateway provisiong data model
    sysevent_set(sysevent_fd_gs, sysevent_token_gs, "epon_prov_status", value, sizeof(value));

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIfDown();
 **************************************************************************
 *  \brief If Down - Exit
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIfDown(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    GWPEpon_StopIPProvisioning();

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIfUp
 **************************************************************************
 *  \brief If up
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIfUp(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    GWPEpon_StartIPProvisioning();

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIpv4Down();
 **************************************************************************
 *  \brief IPv4 WAN Side Routing - Exit
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIpv4Down(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    SetProvisioningStatus(EPON_OPER_IPV4_DOWN);

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIpv4Up
 **************************************************************************
 *  \brief IPv4 WAN Side Routing
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIpv4Up(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    SetProvisioningStatus(EPON_OPER_IPV4_UP);

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIpV6Down()
 **************************************************************************
 *  \brief IPv6 WAN Side Routing - Exit
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIpv6Down(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    SetProvisioningStatus(EPON_OPER_IPV6_DOWN);

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn int GWPEpon_ProcessIpV6Up()
 **************************************************************************
 *  \brief IPv6 WAN Side Routing
 *  \return 0
**************************************************************************/
static int GWPEpon_ProcessIpv6Up(void)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    SetProvisioningStatus(EPON_OPER_IPV6_UP);

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**************************************************************************/
/*! \fn static STATUS GWPEpon_SyseventGetInt
 **************************************************************************
 *  \brief Get sysevent Integer Value
 *  \return int/-1
 **************************************************************************/
int GWPEpon_SyseventGetInt(const char *name)
{
   unsigned char out_value[20];
   int outbufsz = sizeof(out_value);

   if (!sysevent_get(sysevent_fd_gs, sysevent_token_gs, name, out_value,outbufsz))
   {
      return atoi(out_value);
   }
   else
   {
      GWPROVEPONLOG(INFO, "sysevent_get failed\n")
      return -1;
   }
}

/**************************************************************************/
/*! \fn static STATUS GWPEpon_SyseventSetInt
 **************************************************************************
 *  \brief Set sysevent Integer Value
 *  \return 0:success, <0: failure
 **************************************************************************/
int GWPEpon_SyseventSetInt(const char *name, int int_value)
{
   unsigned char value[20];
   sprintf(value, "%d", int_value);

   return sysevent_set(sysevent_fd_gs, sysevent_token_gs, name, value, sizeof(value));
}


/**************************************************************************/
/*! \fn static STATUS GWP_SysCfgGetInt
 **************************************************************************
 *  \brief Get Syscfg Integer Value
 *  \return int/-1
 **************************************************************************/
static int GWPEpon_SysCfgGetInt(const char *name)
{
   unsigned char out_value[20];
   int outbufsz = sizeof(out_value);

   if (!syscfg_get(NULL, name, out_value, outbufsz))
   {
      return atoi(out_value);
   }
   else
   {
      GWPROVEPONLOG(INFO, "syscfg_get failed\n")
      return -1;
   }
}

/**************************************************************************/
/*! \fn static STATUS GWP_SysCfgSetInt
 **************************************************************************
 *  \brief Set Syscfg Integer Value
 *  \return 0:success, <0: failure
 **************************************************************************/
static int GWPEpon_SysCfgSetInt(const char *name, int int_value)
{
   unsigned char value[20];
   sprintf(value, "%d", int_value);

   return syscfg_set(NULL, name, value);
}


/**************************************************************************/
/*! \fn static void GWPEpon_StopIPProvisioning
 **************************************************************************
 *  \brief Stop EPON IP Provisioning
 *  \return 0
 **************************************************************************/
static void GWPEpon_StopIPProvisioning()
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    system("systemctl stop dibbler.service");
    system("rm /tmp/.start_ipv4"); 
    system("systemctl stop udhcp.service");
    SetProvisioningStatus(EPON_OPER_NONE);

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}
/**************************************************************************/
/*! \fn static void GWPEpon_StartIPProvisioning
 **************************************************************************
 *  \brief Start EPON IP Provisioning
 *  \return 0
 **************************************************************************/
static void GWPEpon_StartIPProvisioning()
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    factory_mode = GWPEpon_SysCfgGetInt("factory_mode");
    routerIpModeOverride = GWPEpon_SysCfgGetInt("router_ip_mode_override");

    int prov_status = GWPEpon_SyseventGetInt("prov_status");	
    GWPROVEPONLOG(INFO, "prov_status=%d routerIpModeOverride=%d\n",prov_status, routerIpModeOverride)
    if (factory_mode) //First IP Initialization (Factory reset or Factory unit)
    {  
        routerIpModeOverride = IpProvModeHonor;

	 //Attempt address acquisition in IP-PREF mode
	prov_status = EPON_OPER_NONE;
    }
//    else //Subsequent IP Initialization
    {  
        if (routerIpModeOverride == IpProvModeIpv6Only)
        {
            //system("systemctl start dibbler.service");
            if (!(prov_status & EPON_OPER_IPV6_UP))
            {
                //dibbler (Ipv6) 
                system("systemctl start dibbler.service");
            }
        }
        else if(routerIpModeOverride == IpProvModeDualStack)
        {
            if (!(prov_status & EPON_OPER_IPV6_UP))
            {
                system("systemctl start dibbler.service");
            }

            if (!(prov_status & EPON_OPER_IPV4_UP))
            {
                system("touch /tmp/.start_ipv4");
                system("systemctl start udhcp.service");
            }
        }
        else if(routerIpModeOverride == IpProvModeHonor)
        {
         //check with scott : consider DHCP IP PREF
            if (!(prov_status & EPON_OPER_IPV6_UP))
            {
                system("systemctl start dibbler.service");
            }

            if (!(prov_status & EPON_OPER_IPV4_UP))
            {
                system("touch /tmp/.start_ipv4");
                system("systemctl start udhcp.service");            
            }
        }
    }
    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}

/**************************************************************************/
/*! \fn void *GWPEpon_sysevent_handler(void *data)
 **************************************************************************
 *  \brief Function to process sysevent event
 *  \return 0
**************************************************************************/
static void *GWPEpon_sysevent_handler(void *data)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    async_id_t epon_ifstatus_asyncid;
    async_id_t ipv4_status_asyncid;
    async_id_t ipv6_status_asyncid;
    static unsigned char firstBoot=1;

    sysevent_set_options(sysevent_fd, sysevent_token, "epon_ifstatus", TUPLE_FLAG_EVENT);
    sysevent_setnotification(sysevent_fd, sysevent_token, "epon_ifstatus", &epon_ifstatus_asyncid);
    sysevent_set_options(sysevent_fd, sysevent_token, "ipv4-status", TUPLE_FLAG_EVENT);
    sysevent_setnotification(sysevent_fd, sysevent_token, "ipv4-status",  &ipv4_status_asyncid);
    sysevent_set_options(sysevent_fd, sysevent_token, "ipv6-status", TUPLE_FLAG_EVENT);
    sysevent_setnotification(sysevent_fd, sysevent_token, "ipv6-status",  &ipv6_status_asyncid);

    sysevent_set_options(sysevent_fd, sysevent_token, "ipv6-status", TUPLE_FLAG_EVENT);
    sysevent_setnotification(sysevent_fd, sysevent_token, "ipv6-status",  &ipv6_status_asyncid);

    sysevent_set_options(sysevent_fd_gs, sysevent_token, "epon_prov_status", TUPLE_FLAG_EVENT);
    sysevent_set_options(sysevent_fd_gs, sysevent_token, "prov_status", TUPLE_FLAG_EVENT);

    
   for (;;)
   {
        unsigned char name[25], val[42];
        int namelen = sizeof(name);
        int vallen  = sizeof(val);
        int err;
        async_id_t getnotification_asyncid;

        if (firstBoot)
        {
           err = sysevent_get(sysevent_fd_gs, sysevent_token_gs, "epon_ifstatus", val, vallen);
           firstBoot = 0;
           strcpy(name,"epon_ifstatus");
        }
        else
           err = sysevent_getnotification(sysevent_fd, sysevent_token, name, &namelen,  val, &vallen, &getnotification_asyncid);

        if (err)
        {
           GWPROVEPONLOG(ERROR, "sysevent_getnotification failed with error: %d\n", err)
        }
        else
        {
            GWPROVEPONLOG(WARNING, "received notification event %s\n", name)

            if (strcmp(name, "epon_ifstatus")==0)
            {
                if (strcmp(val, "up")==0)
                {
                    GWPEpon_ProcessIfUp();
                }
                else if (strcmp(val, "down")==0)
                {
                    GWPEpon_ProcessIfDown();
                }
            }
            else if (strcmp(name, "ipv4-status")==0)
            {
                if (strcmp(val, "up")==0)
                {
                    GWPEpon_ProcessIpv4Up();
                }
                else if (strcmp(val, "down")==0)
                {
                    GWPEpon_ProcessIpv4Down();
                }
            }
            else if (strcmp(name, "ipv6-status")==0)
            {
                if (strcmp(val, "up")==0)
                {
                    GWPEpon_ProcessIpv6Up();
                }
                else if (strcmp(val, "down")==0)
                {
                    GWPEpon_ProcessIpv6Down();
                }
            }
            else
            {
               GWPROVEPONLOG(WARNING, "undefined event %s \n",name)
            }
			
            // committing syscfg values
            GWPROVEPONLOG(INFO, "committing syscfg values\n")
            syscfg_commit();
        }
    }

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}

static void GWPEpon_SetDefaults()
{
    char buf[10];
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    if (GWPEpon_SysCfgGetInt("factory_mode") < 0)
    {
        GWPROVEPONLOG(INFO, "setting default factory mode\n")
        GWPEpon_SysCfgSetInt("factory_mode", 0);   
    }

    if (GWPEpon_SysCfgGetInt("router_ip_mode_override") < 0)
    {
        GWPROVEPONLOG(INFO, "setting default router ip override mode\n")
        GWPEpon_SysCfgSetInt("router_ip_mode_override", 2);   
    }
   
    if (GWPEpon_SyseventGetInt ("prov_status") < 0)
    {
        GWPROVEPONLOG(INFO, "setting default prov_status\n")
        GWPEpon_SyseventSetInt("prov_status",0);
    }

    sysevent_get(sysevent_fd_gs, sysevent_token_gs, "epon_ifname", buf, sizeof(buf));
    if (buf[0] != '\0')
    {
        sysevent_set(sysevent_fd_gs, sysevent_token_gs, "epon_ifname", "wanbridge", 0);
    }
    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
}

static bool GWPEpon_Register_sysevent()
{
    bool status = true;
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)
    
    sysevent_fd = sysevent_open("127.0.0.1", SE_SERVER_WELL_KNOWN_PORT, SE_VERSION, "gw_prov_epon", &sysevent_token);
    if (sysevent_fd < 0)
    {
        GWPROVEPONLOG(ERROR, "gw_prov_epon failed to register with sysevent daemon\n");
        status = false;
    }
    else
    {  
        GWPROVEPONLOG(INFO, "gw_prov_epon registered with sysevent daemon successfully\n")
    }

    //Make another connection for gets/sets
    sysevent_fd_gs = sysevent_open("127.0.0.1", SE_SERVER_WELL_KNOWN_PORT, SE_VERSION, "gw_prov_epon-gs", &sysevent_token_gs);
    if (sysevent_fd_gs < 0)
    {
        GWPROVEPONLOG(ERROR, "gw_prov_epon-gs failed to register with sysevent daemon\n")
        status = false;
    }
    else
    {
        GWPROVEPONLOG(INFO, "gw_prov_epon-gs registered with sysevent daemon successfully\n")
    }

    if (status != false)
       GWPEpon_SetDefaults();

    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return status;
}

static int GWPEpon_Init()
{
    int status = 0;
    int thread_status = 0;
    char thread_name[THREAD_NAME_LEN];
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)

    if (GWPEpon_Register_sysevent() == false)
    {
        GWPROVEPONLOG(ERROR, "GWPEpon_Register_sysevent failed\n")
        status = -1;
    }
    else 
    {
        GWPROVEPONLOG(INFO, "GWPEpon_Register_sysevent Successful\n")
    
        thread_status = pthread_create(&sysevent_tid, NULL, GWPEpon_sysevent_handler, NULL);
        if (thread_status == 0)
        {
            GWPROVEPONLOG(INFO, "GWPEpon_sysevent_handler thread created successfully\n");

            memset( thread_name, '\0', sizeof(char) * THREAD_NAME_LEN );
            strcpy( thread_name, "GWPEponsysevent");

            if (pthread_setname_np(sysevent_tid, thread_name) == 0)
                GWPROVEPONLOG(INFO, "GWPEpon_sysevent_handler thread name %s set successfully\n", thread_name)
            else
                GWPROVEPONLOG(ERROR, "%s error occured while setting GWPEpon_sysevent_handler thread name\n", strerror(errno))
        }
        else
        {
            GWPROVEPONLOG(ERROR, "%s error occured while creating GWPEpon_sysevent_handler thread\n", strerror(errno))
            status = -1;
        }

#ifdef HAVE_NETLINK_SUPPORT
        GWPROVEPONLOG(INFO, "Entering HAVE_NETLINK_SUPPORT")
        thread_status = 0;
        thread_status = pthread_create(&netlink_tid, NULL, GWPEpon_netlink_handler, NULL);
        if (thread_status == 0)
        {
            GWPROVEPONLOG(INFO, "GWPEpon_netlink_handler thread created successfully\n");

            memset( thread_name, '\0', sizeof(char) * THREAD_NAME_LEN );
            strcpy( thread_name, "GWPEponnetlink" );

            if (pthread_setname_np(netlink_tid, thread_name) == 0)
                GWPROVEPONLOG(INFO, "GWPEpon_netlink_handler thread name '%s' set successfully\n", thread_name)
            else
                GWPROVEPONLOG(ERROR, "%s error occured while setting GWPEpon_netlink_handler thread name\n", strerror(errno))
        }
        else
            GWPROVEPONLOG(ERROR, "%s occured while creating GWPEpon_netlink_handler thread\n", strerror(errno)) 
#endif
    }
    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return status;
}

static bool checkIfAlreadyRunning(const char* name)
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)
    bool status = true;
	
    FILE *fp = fopen("/tmp/.gwprovepon.pid", "r"); 	
    if (fp == NULL) 
    {
        GWPROVEPONLOG(ERROR, "File /tmp/.gwprovepon.pid doesn't exist\n")
        FILE *pfp = fopen("/tmp/.gwprovepon.pid", "w"); 			
        if (pfp == NULL) 
        {
            GWPROVEPONLOG(ERROR, "Error in creating file /tmp/.gwprovepon.pid\n")
        }
        else
        {
            pid_t pid = getpid();
            fprintf(pfp, "%d", pid);
            fclose(pfp);
        }
        status = false;
    }
    else
    {
        fclose(fp);
    }
    GWPROVEPONLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return status;
}

static void daemonize(void) 
{
    GWPROVEPONLOG(INFO, "Entering into %s\n",__FUNCTION__)
    int fd;
    switch (fork()) {
    case 0:
      	GWPROVEPONLOG(ERROR, "In child pid=%d\n", getpid())
        break;
    case -1:
    	// Error
    	GWPROVEPONLOG(ERROR, "Error daemonizing (fork)! %d - %s\n", errno, strerror(errno))
    	exit(0);
    	break;
    default:
     	GWPROVEPONLOG(ERROR, "In parent exiting\n")
    	_exit(0);
    }

    //create new session and process group
    if (setsid() < 0) {
        GWPROVEPONLOG(ERROR, "Error demonizing (setsid)! %d - %s\n", errno, strerror(errno))
    	exit(0);
    }    

#ifndef  _DEBUG
    //redirect fd's 0,1,2 to /dev/null     
    fd = open("/dev/null", O_RDONLY);
    if (fd != 0) {
        dup2(fd, 0);
        close(fd);
    }
    fd = open("/dev/null", O_WRONLY);
    if (fd != 1) {
        dup2(fd, 1);
        close(fd);
    }
    fd = open("/dev/null", O_WRONLY);
    if (fd != 2) {
        dup2(fd, 2);
        close(fd);
    }
#endif	
}


/**************************************************************************/
/*! \fn int main(int argc, char *argv)
 **************************************************************************
 *  \brief Init and run the Provisioning process
 *  \param[in] argc
 *  \param[in] argv
 *  \return Currently, never exits
 **************************************************************************/
int main(int argc, char *argv[])
{
    int status = 0;
    const int max_retries = 6;
    int retry = 0;

    GWPROVEPONLOG(INFO, "Started gw_prov_epon\n")

    daemonize();

    if (checkIfAlreadyRunning(argv[0]) == true)
    {
        GWPROVEPONLOG(ERROR, "Process %s already running\n", argv[0])
        status = 1;
    }
    else
    {    
        while((syscfg_init() != 0) && (retry++ < max_retries))
        {
            GWPROVEPONLOG(ERROR, "syscfg init failed. Retry<%d> ...\n", retry)
            sleep(5);
        }

        if (retry < max_retries)
        {
            GWPROVEPONLOG(INFO, "syscfg init successful\n")

            if (GWPEpon_Init() != 0)
            {
                GWPROVEPONLOG(ERROR, "GWPEpon Initialization failed\n")
                status = 1;
            }
            else
            {
                GWPROVEPONLOG(INFO, "GwProvEpon initialization completed\n")

                //wait for sysevent_tid thread to terminate
                pthread_join(sysevent_tid, NULL);
                
                GWPROVEPONLOG(INFO,"sysevent_tid thread terminated\n")
            }
        }
        else
        {
            GWPROVEPONLOG(ERROR, "syscfg init failed permanently\n")
            status = 1;
        }
	GWPROVEPONLOG(INFO, "gw_prov_epon app terminated\n")
    }
    return status;
}
