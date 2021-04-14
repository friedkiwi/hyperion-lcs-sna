/* CTC_LCS.C    (C) Copyright James A. Pierson, 2002-2012            */
/*              (C) Copyright "Fish" (David B. Trout), 2002-2011     */
/*              (C) and others 2013-2021                             */
/*              Hercules LAN Channel Station Support                 */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

#include "hstdinc.h"

/* jbs 10/27/2007 added _SOLARIS_ */
#if !defined(__SOLARIS__)

#include "hercules.h"
#include "ctcadpt.h"
#include "tuntap.h"
#include "opcode.h"
#include "herc_getopt.h"

#define SIZEOF_BAFFLE 8

//-----------------------------------------------------------------------------
//  DEBUGGING: use 'ENABLE_TRACING_STMTS' to activate the compile-time
//  "TRACE" and "VERIFY" debug macros. Use 'NO_LCS_OPTIMIZE' to disable
//  compiler optimization for this module (to make setting breakpoints
//  and stepping through code more reliable).
//
//  Use the "-d" device statement option (or the "ctc debug on" panel
//  command) to activate logmsg debugging (verbose debug log messages).
//
//  Use the 'ptt {lcs1|lcs2}' panel command to activate debug tracing
//  via the PTT facility.
//-----------------------------------------------------------------------------

//#define  ENABLE_TRACING_STMTS   1       // (Fish: DEBUGGING)
//#include "dbgtrace.h"                   // (Fish: DEBUGGING)
//#define  NO_LCS_OPTIMIZE                // (Fish: DEBUGGING) (MSVC only)
#define  LCS_NO_950_952                 // (Fish: DEBUGGING: HHC00950 and HHC00952 are rarely interesting)

#if defined( _MSVC_ ) && defined( NO_LCS_OPTIMIZE )
  #pragma optimize( "", off )           // disable optimizations for reliable breakpoints
#endif

// PROGRAMMING NOTE: in addition to normal logmsg debugging (i.e. the "-d"
// device statement option which activates the displaying of log messages)
// we also support debugging via the PTT facility to help with debugging
// otherwise hard to find race conditions. PTT tracing is always enabled,
// but is never activated except by demand via the 'ptt' panel command.

#undef  PTT_TIMING
#define PTT_TIMING      PTT_LCS1        // LCS Timing Debug
#undef  PTT_DEBUG
#define PTT_DEBUG       PTT_LCS2        // LCS General Debugging (including
                                        // LCS device lock and event tracing)

//-----------------------------------------------------------------------------
/* The following CCW codes are immediate commands. */
/*   0x03 - No-Operation                           */
/*   0x17 - Control                                */
/*   0x43 - Set Basic Mode                         */
/*   0xC3 - Set Extended Mode                      */

static BYTE  CTC_Immed_Commands [256] =
{
/* 0 1 2 3 4 5 6 7 8 9 A B C D E F */
   0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0, /* 00 */
   0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0, /* 10 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 20 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 30 */
   0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0, /* 40 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 50 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 60 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 70 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 80 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 90 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* A0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* B0 */
   0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0, /* C0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* D0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* E0 */
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0  /* F0 */
};

// First three octets of Multicast MAC address
static const BYTE mcast3[ 3 ] = { 0x01, 0x00, 0x5e };
static const MAC  zeromac     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// ====================================================================
//                       Declarations
// ====================================================================

static void     LCS_Startup       ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_Shutdown      ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_StartLan      ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_StopLan       ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_QueryIPAssists( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_LanStats      ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_AddMulticast  ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_DelMulticast  ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );
static void     LCS_DefaultCmdProc( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen );

static void     LCS_StartLan_SNA  ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres );
static void     LCS_StopLan_SNA   ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres );
static void     LCS_LanStats_SNA  ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres );
static void     LCS_DefaultCmd_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres );
static void     LCS_Baffle_SNA    ( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres );

static void*    LCS_PortThread( void* arg /*PLCSPORT pLCSPORT */ );
static void*    LCS_AttnThread( void* arg /*PLCSBLK pLCSBLK */ );

static void     LCS_EnqueueEthFrame     ( PLCSDEV pLCSDEV, BYTE bPort, BYTE* pData, size_t iSize );
static int      LCS_DoEnqueueEthFrame   ( PLCSDEV pLCSDEV, BYTE bPort, BYTE* pData, size_t iSize );

static void     LCS_EnqueueReplyFrame   ( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres );
static int      LCS_DoEnqueueReplyFrame ( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres );

//  static void     LCS_EnqueueBaffleFrame   ( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres );
//  static int      LCS_DoEnqueueBaffleFrame ( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres );

static int      BuildOAT( char* pszOATName, PLCSBLK pLCSBLK );
static char*    ReadOAT( char* pszOATName, FILE* fp, char* pszBuff );
static int      ParseArgs( DEVBLK* pDEVBLK, PLCSBLK pLCSBLK,
                           int argc, char** argv );

// ====================================================================
//                       Helper macros
// ====================================================================

#define INIT_REPLY_FRAME( pReply, iReplyLen, pCmdFrame, iCmdLen )    \
    do                                                               \
    {                                                                \
        if ( (iCmdLen) >= (iReplyLen) )                              \
        {                                                            \
            memcpy( (pReply), (pCmdFrame), (iReplyLen) );            \
        }                                                            \
        else                                                         \
        {                                                            \
            memset( (pReply), 0, (iReplyLen) );                      \
            memcpy( (pReply), (pCmdFrame), (iCmdLen) );              \
            (iReplyLen) = (iCmdLen);                                 \
        }                                                            \
        STORE_HW( (pReply)->bLCSCmdHdr.bLCSHdr.hwOffset, 0x0000 );   \
        STORE_HW( (pReply)->bLCSCmdHdr.hwReturnCode, 0x0000 );       \
    }                                                                \
    while (0)

#define ENQUEUE_REPLY_FRAME( pLCSDEV, pReply, iReplyLen, bBafflePres )                   \
    LCS_EnqueueReplyFrame( (pLCSDEV), (PLCSCMDHDR) (pReply), (iReplyLen), (bBafflePres) )

#define SET_CPKTTYPE( ethtyp, pkttyp )                                              \
    do                                                                              \
    {                                                                               \
        if ( (ethtyp) >= ETH_TYPE)                                                  \
        {                                                                           \
                 if ( (ethtyp) == ETH_TYPE_IP   ) STRLCPY( (pkttyp), "IPv4"    );   \
            else if ( (ethtyp) == ETH_TYPE_IPV6 ) STRLCPY( (pkttyp), "IPv6"    );   \
            else if ( (ethtyp) == ETH_TYPE_ARP  ) STRLCPY( (pkttyp), "ARP"     );   \
            else if ( (ethtyp) == ETH_TYPE_RARP ) STRLCPY( (pkttyp), "RARP"    );   \
            else if ( (ethtyp) == ETH_TYPE_SNA  ) STRLCPY( (pkttyp), "SNA"     );   \
            else                                  STRLCPY( (pkttyp), "unknown" );   \
        }                                                                           \
        else                                      STRLCPY( (pkttyp), "802.3" );     \
    }                                                                               \
    while (0)

// ====================================================================
//                    find_group_device
// ====================================================================

static DEVBLK * find_group_device(DEVGRP *group, U16 devnum)
{
    int i;

    for (i = 0; i < group->acount; i++)
        if (group->memdev[i]->devnum == devnum)
            return group->memdev[i];

    return NULL;
}

// ====================================================================
//                          LCS_Init
// ====================================================================

int  LCS_Init( DEVBLK* pDEVBLK, int argc, char *argv[] )
{
    PLCSBLK     pLCSBLK;
    PLCSDEV     pLCSDev;
    PLCSPORT    pLCSPORT;
    int         i;
    int         rc;
    BYTE        bMode;   /* Either LCSDEV_MODE_IP or LCSDEV_MODE_SNA */
    struct in_addr  addr;               // Work area for addresses
    char        thread_name[32];        // Thread name


    pDEVBLK->devtype = 0x3088;

    pDEVBLK->excps   = 0;

    // Return when an existing group has been joined but is still incomplete
    if (!group_device( pDEVBLK, 0 ) && pDEVBLK->group)
        return 0;

    // We need to create a group, and as such determine the number of devices
    if (!pDEVBLK->group)
    {

        // Housekeeping
        pLCSBLK = malloc( sizeof( LCSBLK ));
        if (!pLCSBLK)
        {
            char buf[40];
            MSGBUF(buf, "malloc(%d)", (int) sizeof( LCSBLK ));
            // "%1d:%04X %s: error in function %s: %s"
            WRMSG( HHC00900, "E", SSID_TO_LCSS( pDEVBLK->ssid ),
                pDEVBLK->devnum, pDEVBLK->typname, buf, strerror(errno) );
            return -1;
        }
        memset( pLCSBLK, 0, sizeof( LCSBLK ));

        // Initialize locking and event mechanisms
        initialize_lock( &pLCSBLK->AttnLock );
        initialize_lock( &pLCSBLK->AttnEventLock );
        initialize_condition( &pLCSBLK->AttnEvent );

        for (i=0; i < LCS_MAX_PORTS; i++)
        {
            pLCSPORT = &pLCSBLK->Port[i];
            memset( pLCSPORT, 0, sizeof ( LCSPORT ));

            pLCSPORT->bPort   = i;
            pLCSPORT->pLCSBLK = pLCSBLK;

            // Initialize locking and event mechanisms
            initialize_lock( &pLCSPORT->PortDataLock );
            initialize_lock( &pLCSPORT->PortEventLock );
            initialize_condition( &pLCSPORT->PortEvent );
        }

        // Parse configuration file statement
        rc = ParseArgs( pDEVBLK, pLCSBLK, argc, (char**) argv );
        if (rc < 0)
        {
            free( pLCSBLK );
            pLCSBLK = NULL;
            return -1;
        }
        bMode = rc;

        if (pLCSBLK->pszOATFilename)
        {
            // If an OAT file was specified, Parse it and build the
            // OAT table.
            if (BuildOAT( pLCSBLK->pszOATFilename, pLCSBLK ) != 0)
            {
                free( pLCSBLK );
                pLCSBLK = NULL;
                return -1;
            }
        }
        else
        {
            // Otherwise, build an OAT based on the address specified
            // in the config file with an assumption of IP mode.
            pLCSBLK->pDevices = malloc( sizeof( LCSDEV ));

            memset( pLCSBLK->pDevices, 0, sizeof( LCSDEV ));
            pLCSBLK->pDevices->sAddr        = pDEVBLK->devnum;
            pLCSBLK->pDevices->bPort        = 0;
            pLCSBLK->pDevices->pNext        = NULL;

            if (bMode == LCSDEV_MODE_IP)
            {
                if (pLCSBLK->pszIPAddress)
                {
                    pLCSBLK->pDevices->pszIPAddress = strdup( pLCSBLK->pszIPAddress );
                    inet_aton( pLCSBLK->pDevices->pszIPAddress, &addr );
                    pLCSBLK->pDevices->lIPAddress = addr.s_addr; // (network byte order)
                    pLCSBLK->pDevices->bType    = LCSDEV_TYPE_NONE;
                }
                else
                    pLCSBLK->pDevices->bType    = LCSDEV_TYPE_PRIMARY;

                pLCSBLK->pDevices->bMode        = LCSDEV_MODE_IP;

                pLCSBLK->icDevices = 2;
            }
            else
            {
                pLCSBLK->pDevices->bMode        = LCSDEV_MODE_SNA;

                pLCSBLK->icDevices = 1;
            }
        }

        // Now we must create the group
        if (!group_device( pDEVBLK, pLCSBLK->icDevices ))
        {
            pDEVBLK->group->grp_data = pLCSBLK;
            return 0;
        }
        else
            pDEVBLK->group->grp_data = pLCSBLK;

    }
    else
        pLCSBLK = pDEVBLK->group->grp_data;

    // When this code is reached the last devblk has been allocated.

    // Now build the LCSDEV's...

    // If an OAT is specified, the addresses that were specified in the
    // hercules.cnf file must match those that are specified in the OAT.

    for (pLCSDev = pLCSBLK->pDevices; pLCSDev; pLCSDev = pLCSDev->pNext)
    {
        pLCSDev->pDEVBLK[0] = find_group_device( pDEVBLK->group, pLCSDev->sAddr );

        if (!pLCSDev->pDEVBLK[0])
        {
            // "%1d:%04X CTC: lcs device %04X not in configuration"
            WRMSG( HHC00920, "E", SSID_TO_LCSS( pDEVBLK->group->memdev[0]->ssid ) ,
                  pDEVBLK->group->memdev[0]->devnum, pLCSDev->sAddr );
            return -1;
        }

        // Establish SENSE ID and Command Information Word data.
        SetSIDInfo( pLCSDev->pDEVBLK[0], 0x3088, 0x60, 0x3088, 0x01 );
//      SetCIWInfo( pLCSDev->pDEVBLK[0], 0, 0, 0x72, 0x0080 );
//      SetCIWInfo( pLCSDev->pDEVBLK[0], 1, 1, 0x83, 0x0004 );
//      SetCIWInfo( pLCSDev->pDEVBLK[0], 2, 2, 0x82, 0x0040 );

        pLCSDev->pDEVBLK[0]->ctctype  = CTC_LCS;
        pLCSDev->pDEVBLK[0]->ctcxmode = 1;
        pLCSDev->pDEVBLK[0]->dev_data = pLCSDev;
        pLCSDev->pLCSBLK              = pLCSBLK;
        STRLCPY( pLCSDev->pDEVBLK[0]->filename, pLCSBLK->pszTUNDevice );

        // If this is an IP Passthru address, we need a write address
        if (pLCSDev->bMode == LCSDEV_MODE_IP)
        {
            // (the write device is the inverse of the read device)
            pLCSDev->pDEVBLK[1] = find_group_device( pDEVBLK->group, pLCSDev->sAddr ^ 1 );

            if (!pLCSDev->pDEVBLK[1])
            {
                // "%1d:%04X CTC: lcs device %04X not in configuration"
                WRMSG( HHC00920, "E", SSID_TO_LCSS( pDEVBLK->group->memdev[0]->ssid ),
                      pDEVBLK->group->memdev[0]->devnum, pLCSDev->sAddr ^ 1 );
                return -1;
            }

            // Establish SENSE ID and Command Information Word data.
            SetSIDInfo( pLCSDev->pDEVBLK[1], 0x3088, 0x60, 0x3088, 0x01 );
//          SetCIWInfo( pLCSDev->pDEVBLK[1], 0, 0, 0x72, 0x0080 );
//          SetCIWInfo( pLCSDev->pDEVBLK[1], 1, 1, 0x83, 0x0004 );
//          SetCIWInfo( pLCSDev->pDEVBLK[1], 2, 2, 0x82, 0x0040 );

            pLCSDev->pDEVBLK[1]->ctctype  = CTC_LCS;
            pLCSDev->pDEVBLK[1]->ctcxmode = 1;
            pLCSDev->pDEVBLK[1]->dev_data = pLCSDev;

            STRLCPY( pLCSDev->pDEVBLK[1]->filename, pLCSBLK->pszTUNDevice );
        }

        // Initialize the buffer size. See the programming note re
        // frame buffer size in ctcadpt.h, and note that the LCS_Startup
        // command might reduce the value in pLCSDEV->iMaxFrameBufferSize.
        // For SNA, the LCS_Startup command seems to be not used.
        pLCSDev->iMaxFrameBufferSize = sizeof(pLCSDev->bFrameBuffer);

        // Indicate that the DEVBLK(s) have been create sucessfully
        pLCSDev->fDevCreated = 1;

        // Initialize locking and event mechanisms
        initialize_lock( &pLCSDev->DevDataLock );
        initialize_lock( &pLCSDev->DevEventLock );
        initialize_condition( &pLCSDev->DevEvent );

        // Create the TAP interface (if not already created by a
        // previous pass. More than one interface can exist on a port.

        pLCSPORT = &pLCSBLK->Port[ pLCSDev->bPort ];

        if (!pLCSPORT->fPortCreated)
        {
            int  rc;

            rc = TUNTAP_CreateInterface( pLCSBLK->pszTUNDevice,
                                         IFF_TAP | IFF_NO_PI,
                                         &pLCSPORT->fd,
                                         pLCSPORT->szNetIfName );

            if (rc < 0)
            {
                // "%1d:%04X %s: error in function %s: %s"
                WRMSG( HHC00900, "E", SSID_TO_LCSS( pLCSDev->pDEVBLK[0]->ssid),
                    pLCSDev->pDEVBLK[0]->devnum, pLCSDev->pDEVBLK[0]->typname,
                    "TUNTAP_CreateInterface", strerror( rc ));
                return -1;
            }

            // "%1d:%04X %s: interface %s, type %s opened"
            WRMSG( HHC00901, "I", SSID_TO_LCSS( pLCSDev->pDEVBLK[0]->ssid ),
                                  pLCSDev->pDEVBLK[0]->devnum,
                                  pLCSDev->pDEVBLK[0]->typname,
                                  pLCSPORT->szNetIfName, "TAP");

#if defined(OPTION_W32_CTCI)

            // Set the specified driver/dll i/o buffer sizes..
            {
                struct tt32ctl tt32ctl;

                memset( &tt32ctl, 0, sizeof( tt32ctl ));
                STRLCPY( tt32ctl.tt32ctl_name, pLCSPORT->szNetIfName );

                tt32ctl.tt32ctl_devbuffsize = pLCSBLK->iKernBuff;

                if (TUNTAP_IOCtl( pLCSPORT->fd, TT32SDEVBUFF, (char*) &tt32ctl ) != 0)
                {
                    // "%1d:%04X %s: ioctl %s failed for device %s: %s"
                    WRMSG( HHC00902, "W", SSID_TO_LCSS( pLCSDev->pDEVBLK[0]->ssid ),
                          pLCSDev->pDEVBLK[0]->devnum,
                          pLCSDev->pDEVBLK[0]->typname,
                          "TT32SDEVBUFF", pLCSPORT->szNetIfName, strerror( errno ));
                }

                tt32ctl.tt32ctl_iobuffsize = pLCSBLK->iIOBuff;

                if (TUNTAP_IOCtl( pLCSPORT->fd, TT32SIOBUFF, (char*) &tt32ctl ) != 0)
                {
                    // "%1d:%04X %s: ioctl %s failed for device %s: %s"
                    WRMSG( HHC00902, "W", SSID_TO_LCSS( pLCSDev->pDEVBLK[0]->ssid ),
                          pLCSDev->pDEVBLK[0]->devnum,
                          pLCSDev->pDEVBLK[0]->typname,
                          "TT32SIOBUFF", pLCSPORT->szNetIfName, strerror( errno ) );
                }
            }
#endif

            // Indicate that the port is used.
            pLCSPORT->fUsed        = 1;
            pLCSPORT->fPortCreated = 1;

            // Set assist flags
            LCS_Assist( pLCSPORT );

            // Now create the port thread to read packets from tuntap
            // The thread name uses the read device address of the first device pair.
            MSGBUF( thread_name, "%s %4.4X Port %d Thread",
                                 pLCSBLK->pDevices->pDEVBLK[0]->typname,
                                 pLCSBLK->pDevices->pDEVBLK[0]->devnum,
                                 pLCSPORT->bPort);
            rc = create_thread( &pLCSPORT->tid, JOINABLE,
                                LCS_PortThread, pLCSPORT, thread_name );
            if (rc)
            {
                // "Error in function create_thread(): %s"
                WRMSG( HHC00102, "E", strerror( rc ));
            }

            // Identify thread ID with devices on which they're active
            pLCSDev->pDEVBLK[0]->tid = pLCSPORT->tid;
            if (pLCSDev->pDEVBLK[1])
                pLCSDev->pDEVBLK[1]->tid = pLCSPORT->tid;
        }

        // Add these devices to the ports device list.
        pLCSPORT->icDevices++;
        pLCSDev->pDEVBLK[0]->fd = pLCSPORT->fd;

        if (pLCSDev->pDEVBLK[1])
            pLCSDev->pDEVBLK[1]->fd = pLCSPORT->fd;

    }   // end of  for (pLCSDev = pLCSBLK->pDevices; pLCSDev; pLCSDev = pLCSDev->pNext)


    // If this LCS has one or more SNA devices we need an attention required thread to present Attention interrupts to the guest.
    for (pLCSDev = pLCSBLK->pDevices; pLCSDev; pLCSDev = pLCSDev->pNext)
    {
        if (pLCSDev->bMode == LCSDEV_MODE_SNA)
        {
            // Now create the attention required thread to present Attention interrupts to the guest(s).
            // The thread name uses the read device address of the first device pair.
            MSGBUF( thread_name, "%s %4.4X AttnThread",
                                 pLCSBLK->pDevices->pDEVBLK[0]->typname,
                                 pLCSBLK->pDevices->pDEVBLK[0]->devnum);
            rc = create_thread( &pLCSBLK->AttnTid, JOINABLE,
                                LCS_AttnThread, pLCSBLK, thread_name );
            if (rc)
            {
                // "Error in function create_thread(): %s"
                WRMSG( HHC00102, "E", strerror( rc ));
            }
            break;
        }
    }   // end of  for (pLCSDev = pLCSBLK->pDevices; pLCSDev; pLCSDev = pLCSDev->pNext)

    return 0;
}

// ====================================================================
//                          LCS_Assist
// ====================================================================
// Determine which IP assists we will be supporting, which depends on
// which assists the tuntap device itself supports, as well as which
// ones we can directly support ourselves if tuntap can't support it.
// The most important assist is perhaps the Checksum Offloading assist
// since we (or tuntap) can calculate checksums much more efficiently
// than our guest can using emulated instructions.
// --------------------------------------------------------------------

void LCS_Assist( PLCSPORT pLCSPORT )
{
#if defined( SIOCGIFHWADDR )
    MAC    mac  = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    ifreq  ifr  = {0};
#endif

    // We shall always support the following assists for the guest.

    pLCSPORT->sIPAssistsSupported |= LCS_MULTICAST_SUPPORT;
    pLCSPORT->sIPAssistsEnabled   |= LCS_MULTICAST_SUPPORT;

    pLCSPORT->sIPAssistsSupported |= LCS_INBOUND_CHECKSUM_SUPPORT;
    pLCSPORT->sIPAssistsEnabled   |= LCS_INBOUND_CHECKSUM_SUPPORT;

    pLCSPORT->sIPAssistsSupported |= LCS_OUTBOUND_CHECKSUM_SUPPORT;
    pLCSPORT->sIPAssistsEnabled   |= LCS_OUTBOUND_CHECKSUM_SUPPORT;

    // Check if tuntap can handle the multicast assist for us.

#if defined( SIOCGIFHWADDR )
    STRLCPY( ifr.ifr_name, pLCSPORT->szNetIfName );
    memcpy( ifr.ifr_hwaddr.sa_data, mac, sizeof( MAC ));

    if (TUNTAP_IOCtl( pLCSPORT->fd, SIOCADDMULTI, (char*) &ifr ) == 0)
    {
        TUNTAP_IOCtl( pLCSPORT->fd, SIOCDELMULTI, (char*) &ifr );
        pLCSPORT->fDoMCastAssist = 0;   // (tuntap does it for us)
    }
    else
#endif // !defined( SIOCGIFHWADDR )
        pLCSPORT->fDoMCastAssist = 1;   // (we must do it ourself)

    // "CTC: lcs device port %2.2X: %s Multicast assist enabled"
    WRMSG( HHC00921, "I", pLCSPORT->bPort,
        pLCSPORT->fDoMCastAssist ? "manual" : "tuntap" );

    // Check if tuntap can do outbound checksum offloading for us.

#if 0 /* Disable for now. TAP Checksum offload seems broken */
#if defined( TUNSETOFFLOAD ) && defined( TUN_F_CSUM )
    if (TUNTAP_IOCtl( pLCSPORT->fd, TUNSETOFFLOAD, (char*) TUN_F_CSUM ) == 0)
    {
        pLCSPORT->fDoCkSumOffload = 0;    // (tuntap does it for us)
    }
    else
#endif
#endif
        pLCSPORT->fDoCkSumOffload = 1;    // (we must do it ourself)

    // "CTC: lcs device port %2.2X: %s Checksum Offload enabled"
    WRMSG( HHC00935, "I", pLCSPORT->bPort,
        pLCSPORT->fDoCkSumOffload ? "manual" : "tuntap" );

    // Check if tuntap can also do segmentation offloading for us.

#if 0 /* Disable for now. TCP Segment Offload needs
         to be enabled by stack using LCS */
#if defined( TUNSETOFFLOAD ) && defined( TUN_F_TSO4 ) && defined( TUN_F_UFO )
    /* Only do offload if doing TAP checksum offload */
    if (!pLCSPORT->fDoCkSumOffload)
    {
        if (TUNTAP_IOCtl( pLCSPORT->fd, TUNSETOFFLOAD, (char*)(TUN_F_CSUM | TUN_F_TSO4 | TUN_F_UFO)) == 0)
        {
            VERIFY( TUNTAP_IOCtl( pLCSPORT->fd, TUNSETOFFLOAD, (char*) TUN_F_CSUM ) == 0);
            pLCSPORT->sIPAssistsSupported |= LCS_IP_FRAG_REASSEMBLY;
            /* Do Not ENABLE!! */
            // pLCSPORT->sIPAssistsEnabled   |= LCS_IP_FRAG_REASSEMBLY;
            // "CTC: lcs device port %2.2X: %s Large Send Offload supported"
            WRMSG( HHC00938, "I", pLCSPORT->bPort, "tuntap" );
        }
    }
#endif
#endif
}

// ====================================================================
//                        LCS_ExecuteCCW
// ====================================================================

void  LCS_ExecuteCCW( DEVBLK* pDEVBLK, BYTE  bCode,
                      BYTE    bFlags,  BYTE  bChained,
                      U32     sCount,  BYTE  bPrevCode,
                      int     iCCWSeq, BYTE* pIOBuf,
                      BYTE*   pMore,   BYTE* pUnitStat,
                      U32*    pResidual )
{
    int             iNum;               // Number of bytes to move
    BYTE            bOpCode;            // CCW opcode with modifier
                                        //   bits masked off

    UNREFERENCED( bFlags    );
    UNREFERENCED( bChained  );
    UNREFERENCED( bPrevCode );
    UNREFERENCED( iCCWSeq   );


    // Display various information, maybe
    if (((LCSDEV*)pDEVBLK->dev_data)->pLCSBLK->fDebug)
    {
        // HHC03992 "%1d:%04X %s: Code %02X: Flags %02X: Count %08X: Chained %02X: PrevCode %02X: CCWseq %d"
        WRMSG(HHC03992, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
            bCode, bFlags, sCount, bChained, bPrevCode, iCCWSeq );
    }

    // Intervention required if the device file is not open
    if (1
        && pDEVBLK->fd < 0
        && !IS_CCW_SENSE  ( bCode )
        && !IS_CCW_CONTROL( bCode )
    )
    {
        pDEVBLK->sense[0] = SENSE_IR;
        *pUnitStat = CSW_CE | CSW_DE | CSW_UC;
        // Display various information, maybe
        if (((LCSDEV*)pDEVBLK->dev_data)->pLCSBLK->fDebug)
        {
            // HHC03993 "%1d:%04X %s: Status %02X: Residual %08X: More %02X"
            WRMSG(HHC03993, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                *pUnitStat, *pResidual, *pMore );
        }
        return;
    }

    // Mask off the modifier bits in the CCW bOpCode
         if ((bCode & 0x07) == 0x07) bOpCode = 0x07;
    else if ((bCode & 0x03) == 0x02) bOpCode = 0x02;
    else if ((bCode & 0x0F) == 0x0C) bOpCode = 0x0C;
    else if ((bCode & 0x03) == 0x01) bOpCode = pDEVBLK->ctcxmode ? (bCode & 0x83) : 0x01;
    else if ((bCode & 0x1F) == 0x14) bOpCode = 0x14;
    else if ((bCode & 0x47) == 0x03) bOpCode = 0x03;
    else if ((bCode & 0xC7) == 0x43) bOpCode = 0x43;
    else
        bOpCode = bCode;


    // Process depending on CCW bOpCode
    switch (bOpCode)
    {
    case 0x01:  // 0MMMMM01  WRITE
        //------------------------------------------------------------
        // WRITE
        //------------------------------------------------------------

        // Return normal status if CCW count is zero
        if (sCount == 0)
        {
            *pUnitStat = CSW_CE | CSW_DE;
            break;
        }

        LCS_Write( pDEVBLK, sCount, pIOBuf, pUnitStat, pResidual );

        break;

    case 0x81:  // 1MMMMM01  WEOF
        //------------------------------------------------------------
        // WRITE EOF
        //------------------------------------------------------------

        // Return normal status
        *pUnitStat = CSW_CE | CSW_DE;
        break;

    case 0x02:  // MMMMMM10  READ
    case 0x0C:  // MMMM1100  RDBACK
        // -----------------------------------------------------------
        // READ & READ BACKWARDS
        // -----------------------------------------------------------

        // Read data and set unit status and residual byte count
        LCS_Read( pDEVBLK, sCount, pIOBuf, pUnitStat, pResidual, pMore );

        break;

    case 0x07:  // MMMMM111  CTL
        // -----------------------------------------------------------
        // CONTROL
        // -----------------------------------------------------------

        *pUnitStat = CSW_CE | CSW_DE;
        break;

    case 0x03:  // M0MMM011  NOP
        // -----------------------------------------------------------
        // CONTROL NO-OPERATON
        // -----------------------------------------------------------

        *pUnitStat = CSW_CE | CSW_DE;
        break;

    case 0x43:  // 00XXX011  SBM
        // -----------------------------------------------------------
        // SET BASIC MODE
        // -----------------------------------------------------------

        // Command reject if in basic mode
        if (pDEVBLK->ctcxmode == 0)
        {
            pDEVBLK->sense[0] = SENSE_CR;
            *pUnitStat        = CSW_CE | CSW_DE | CSW_UC;

            break;
        }

        // Reset extended mode and return normal status
        pDEVBLK->ctcxmode = 0;

        *pResidual = 0;
        *pUnitStat = CSW_CE | CSW_DE;

        break;

    case 0xC3:  // 11000011  SEM
        // -----------------------------------------------------------
        // SET EXTENDED MODE
        // -----------------------------------------------------------

        pDEVBLK->ctcxmode = 1;

        *pResidual = 0;
        *pUnitStat = CSW_CE | CSW_DE;

        break;

    case 0xE3:  // 11100011
        // -----------------------------------------------------------
        // PREPARE (PREP)
        // -----------------------------------------------------------

        *pUnitStat = CSW_CE | CSW_DE;

        break;

    case 0x14:  // XXX10100  SCB
        // -----------------------------------------------------------
        // SENSE COMMAND BYTE
        // -----------------------------------------------------------

        *pUnitStat = CSW_CE | CSW_DE;
        break;

    case 0x04:  // 00000100  SENSE
      // -----------------------------------------------------------
      // SENSE
      // -----------------------------------------------------------

        // Command reject if in basic mode
        if (pDEVBLK->ctcxmode == 0)
        {
            pDEVBLK->sense[0] = SENSE_CR;
            *pUnitStat        = CSW_CE | CSW_DE | CSW_UC;
            break;
        }

        // Calculate residual byte count
        iNum = ( sCount < pDEVBLK->numsense ) ?
            sCount : pDEVBLK->numsense;

        *pResidual = sCount - iNum;

        if (sCount < pDEVBLK->numsense)
            *pMore = 1;

        // Copy device sense bytes to channel I/O buffer
        memcpy( pIOBuf, pDEVBLK->sense, iNum );

        // Clear the device sense bytes
        memset( pDEVBLK->sense, 0, sizeof( pDEVBLK->sense ) );

        // Return unit status
        *pUnitStat = CSW_CE | CSW_DE;

        break;

    case 0xE4:  //  11100100  SID
        // -----------------------------------------------------------
        // SENSE ID
        // -----------------------------------------------------------

        // Calculate residual byte count
        iNum = ( sCount < pDEVBLK->numdevid ) ?
            sCount : pDEVBLK->numdevid;

        *pResidual = sCount - iNum;

        if (sCount < pDEVBLK->numdevid)
            *pMore = 1;

        // Copy device identifier bytes to channel I/O buffer
        memcpy( pIOBuf, pDEVBLK->devid, iNum );

        // Return unit status
        *pUnitStat = CSW_CE | CSW_DE;

        break;

    default:
        // ------------------------------------------------------------
        // INVALID OPERATION
        // ------------------------------------------------------------

        // Set command reject sense byte, and unit check status
        pDEVBLK->sense[0] = SENSE_CR;
        *pUnitStat        = CSW_CE | CSW_DE | CSW_UC;
    }

    // Display various information, maybe
    if (((LCSDEV*)pDEVBLK->dev_data)->pLCSBLK->fDebug)
    {
        // HHC03993 "%1d:%04X %s: Status %02X: Residual %08X: More %02X"
        WRMSG(HHC03993, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
            *pUnitStat, *pResidual, *pMore );
    }

    return;
}

// ====================================================================
//                           LCS_Close
// ====================================================================

int  LCS_Close( DEVBLK* pDEVBLK )
{
    PLCSDEV     pLCSDEV;
    PLCSBLK     pLCSBLK;
    PLCSPORT    pLCSPORT;

    if (!(pLCSDEV = (PLCSDEV)pDEVBLK->dev_data))
        return 0; // (was incomplete group)

    pLCSBLK  = pLCSDEV->pLCSBLK;
    pLCSPORT = &pLCSBLK->Port[pLCSDEV->bPort];

    pLCSPORT->icDevices--;

    PTT_DEBUG( "CLOSE: ENTRY      ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    // Is this the last device on the port?
    if (!pLCSPORT->icDevices)
    {
        PTT_DEBUG( "CLOSE: is last    ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

        // PROGRAMMING NOTE: there's currently no way to interrupt
        // the "LCS_PortThread"s TUNTAP_Read of the adapter. Thus
        // we must simply wait for LCS_PortThread to eventually
        // notice that we're doing a close (via our setting of the
        // fCloseInProgress flag). Its TUNTAP_Read will eventually
        // timeout after a few seconds (currently 5, which is dif-
        // ferent than the DEF_NET_READ_TIMEOUT_SECS timeout value
        // CTCI_Read function uses) and will then do the close of
        // the adapter for us (TUNTAP_Close) so we don't have to.
        // All we need to do is ask it to exit (via our setting of
        // the fCloseInProgress flag) and then wait for it to exit
        // (which, as stated, could take up to a max of 5 seconds).

        // All of this is simply because it's poor form to close a
        // device from one thread while another thread is reading
        // from it. Attempting to do so could trip a race condition
        // wherein the internal i/o buffers used to process the
        // read request could have been freed (by the close call)
        // by the time the read request eventually gets serviced.

        if (pLCSPORT->fd >= 0)
        {
            TID tid = pLCSPORT->tid;
            PTT_DEBUG( "CLOSE: closing... ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            PTT_DEBUG(        "GET  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            obtain_lock( &pLCSPORT->PortEventLock );
            PTT_DEBUG(        "GOT  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            {
                if (pDEVBLK->ccwtrace || pDEVBLK->ccwstep || pLCSBLK->fDebug)
                    // "%1d:%04X CTC: lcs triggering port %2.2X event"
                    WRMSG( HHC00966, "I", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum, pLCSPORT->bPort );

                PTT_DEBUG( "CLOSING started=NO", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                pLCSPORT->fPortStarted = 0;
                PTT_DEBUG( "SET  closeInProg  ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                pLCSPORT->fCloseInProgress = 1;
                PTT_DEBUG(             "SIG  PortEvent    ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                signal_condition( &pLCSPORT->PortEvent );
            }
            PTT_DEBUG(         "REL  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            release_lock( &pLCSPORT->PortEventLock );
            PTT_DEBUG( "join_thread       ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            join_thread( tid, NULL );
            PTT_DEBUG( "detach_thread     ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            detach_thread( tid );
        }

        if (pLCSDEV->pDEVBLK[0] && pLCSDEV->pDEVBLK[0]->fd >= 0)
            pLCSDEV->pDEVBLK[0]->fd = -1;
        if (pLCSDEV->pDEVBLK[1] && pLCSDEV->pDEVBLK[1]->fd >= 0)
            pLCSDEV->pDEVBLK[1]->fd = -1;

        PTT_DEBUG( "CLOSE: closed     ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    }
    else
        PTT_DEBUG( "CLOSE: not last   ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    PTT_DEBUG( "CLOSE: cleaning up", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    // Housekeeping
    if (pLCSDEV->pDEVBLK[0] == pDEVBLK)
        pLCSDEV->pDEVBLK[0] = NULL;
    if (pLCSDEV->pDEVBLK[1] == pDEVBLK)
        pLCSDEV->pDEVBLK[1] = NULL;

    if (!pLCSDEV->pDEVBLK[0] &&
        !pLCSDEV->pDEVBLK[1])
    {
        // Remove this LCS Device from the chain...

        PLCSDEV  pCurrLCSDev  = NULL;
        PLCSDEV* ppPrevLCSDev = &pLCSBLK->pDevices;

        for (pCurrLCSDev = pLCSBLK->pDevices; pCurrLCSDev; pCurrLCSDev = pCurrLCSDev->pNext)
        {
            if (pCurrLCSDev == pLCSDEV)
            {
                *ppPrevLCSDev = pCurrLCSDev->pNext;

                if (pCurrLCSDev->pszIPAddress)
                {
                    free( pCurrLCSDev->pszIPAddress );
                    pCurrLCSDev->pszIPAddress = NULL;
                }

                free( pLCSDEV );
                pLCSDEV = NULL;
                break;
            }

            ppPrevLCSDev = &pCurrLCSDev->pNext;
        }
    }

    if (!pLCSBLK->pDevices)
    {
        if (pLCSBLK->pszTUNDevice  ) { free( pLCSBLK->pszTUNDevice   ); pLCSBLK->pszTUNDevice   = NULL; }
        if (pLCSBLK->pszOATFilename) { free( pLCSBLK->pszOATFilename ); pLCSBLK->pszOATFilename = NULL; }
        if (pLCSBLK->pszIPAddress  ) { free( pLCSBLK->pszIPAddress   ); pLCSBLK->pszIPAddress   = NULL; }


        if ( pLCSBLK->AttnTid )
        {
            TID tid = pLCSBLK->AttnTid;
            PTT_DEBUG( "CLOSE: closing... ", 000, 000,000 );
            PTT_DEBUG( "GET  AttnEventLock", 000, 000, 000 );
            obtain_lock( &pLCSBLK->AttnEventLock );
            PTT_DEBUG( "GOT  AttnEventLock", 000, 000, 000 );
            {
                PTT_DEBUG( "SET  closeInProg  ", 000, 000, 000 );
                pLCSBLK->fCloseInProgress = 1;
                PTT_DEBUG( "SIG  AttnEvent", 000, 000, 000 );
                signal_condition( &pLCSBLK->AttnEvent );
            }
            PTT_DEBUG( "REL  AttnEventLock", 000, 000, 000 );
            release_lock( &pLCSBLK->AttnEventLock );
            PTT_DEBUG( "join_thread       ", 000, 000, 000 );
            join_thread( tid, NULL );
            PTT_DEBUG( "detach_thread     ", 000, 000, 000 );
            detach_thread( tid );
        }

        free( pLCSBLK );
        pLCSBLK = NULL;
    }

    pDEVBLK->dev_data = NULL;

    PTT_DEBUG( "CLOSE: EXIT       ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    return 0;
}

// ====================================================================
//                         LCS_Query
// ====================================================================

void  LCS_Query( DEVBLK* pDEVBLK, char** ppszClass,
                 int     iBufLen, char*  pBuffer )
{
    char filename[ PATH_MAX + 1 ];      /* full path or just name    */

    char *sType[] = { "", " Pri", " Sec" };

    LCSDEV*  pLCSDEV;

    BEGIN_DEVICE_CLASS_QUERY( "CTCA", pDEVBLK, ppszClass, iBufLen, pBuffer );

    pLCSDEV = (LCSDEV*) pDEVBLK->dev_data;

    if (!pLCSDEV)
    {
        strlcpy(pBuffer,"*Uninitialized",iBufLen);
        return;
    }

    snprintf( pBuffer, iBufLen, "LCS Port %2.2X %s%s (%s)%s IO[%"PRIu64"]",
              pLCSDEV->bPort,
              pLCSDEV->bMode == LCSDEV_MODE_IP ? "IP" : "SNA",
              sType[pLCSDEV->bType],
              pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort].szNetIfName,
              pLCSDEV->pLCSBLK->fDebug ? " -d" : "",
              pDEVBLK->excps );
}

// ====================================================================
//                   LCS Multi-Write Support
// ====================================================================

#if defined( OPTION_W32_CTCI )

static void  LCS_BegMWrite( DEVBLK* pDEVBLK )
{
    if (((LCSDEV*)pDEVBLK->dev_data)->pLCSBLK->fNoMultiWrite) return;
    PTT_TIMING( "b4 begmw", 0, 0, 0 );
    TUNTAP_BegMWrite( pDEVBLK->fd, CTC_DEF_FRAME_BUFFER_SIZE );
    PTT_TIMING( "af begmw", 0, 0, 0);
}

static void  LCS_EndMWrite( DEVBLK* pDEVBLK, int nEthBytes, int nEthFrames )
{
    if (((LCSDEV*)pDEVBLK->dev_data)->pLCSBLK->fNoMultiWrite) return;
    PTT_TIMING( "b4 endmw", 0, nEthBytes, nEthFrames );
    TUNTAP_EndMWrite( pDEVBLK->fd );
    PTT_TIMING( "af endmw", 0, nEthBytes, nEthFrames );
}

#else // !defined( OPTION_W32_CTCI )

  #define  LCS_BegMWrite( pDEVBLK )
  #define  LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames )

#endif // defined( OPTION_W32_CTCI )

// ====================================================================
//                         LCS_Write
// ====================================================================
// The guest o/s is issuing a Write CCW for our LCS device. All LCS
// Frames in its buffer which are NOT internal Command Frames will
// be immediately written to our host's adapter (via TunTap). Frames
// that are internal Command Frames however are processed internally
// and cause a "reply" frame to be enqueued to the LCS Device output
// buffer to be eventually returned back to the guest the next time
// it issues a Read CCW (satisfied by LCS_Read).
// --------------------------------------------------------------------

void  LCS_Write( DEVBLK* pDEVBLK,   U32   sCount,
                 BYTE*   pIOBuf,    BYTE* pUnitStat,
                 U32*    pResidual )
{
    PLCSDEV     pLCSDEV      = (PLCSDEV) pDEVBLK->dev_data;
    PLCSBLK     pLCSBLK      = pLCSDEV->pLCSBLK;
    PLCSPORT    pLCSPORT     = &pLCSBLK->Port[ pLCSDEV->bPort ];
    PLCSHDR     pLCSHDR      = NULL;
    PLCSCMDHDR  pCmdFrame    = NULL;
    PLCSETHFRM  pLCSEthFrame = NULL;
    PETHFRM     pEthFrame    = NULL;
    PLCSATTN    pLCSATTN     = NULL;
    U16         iOffset      = 0;
    U16         iPrevOffset  = 0;
    U16         iLength      = 0;
    U16         iEthLen      = 0;
    int         nEthFrames   = 0;
    int         nEthBytes    = 0;
    char        cPktType[8];
    char        buf[32];
    U16         hwEthernetType;
    U16         hwBaffleLen;
    BYTE        bBafflePres;
    BYTE*       pIOBufStart = NULL;

    // Display the data written by the guest, if debug is active.
    if (pLCSBLK->fDebug)
    {
        // "%1d:%04X %s: Accept data of size %d bytes from guest"
        WRMSG(HHC00981, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum,  pDEVBLK->typname, (int)sCount );
        net_data_trace( pDEVBLK, pIOBuf, (int)sCount, '<', 'D', "data", 0 );
    }

    // Process each frame in the buffer...

    PTT_DEBUG( "WRIT ENTRY        ", 000, pDEVBLK->devnum, -1 );
    PTT_TIMING( "beg write", 0, 0, 0 );
    LCS_BegMWrite( pDEVBLK ); // (performance)

    // ----------------------------------------------------------------
    //    LCS_Write for IP mode
    // ----------------------------------------------------------------
    //
    // IP mode handles Ethernet frames and LCS commands, both of which
    // are prefixed with a 4-byte LCSHDR.
    // The following illustrates an Ethernet frame containing an IPv4
    // packet with an ICMP Ping reply:-
    //   HHC00979D LCS: data: +0000< 00660100 02000042 22800200 00422281  .f......".....". ...../...../...a
    //   HHC00979D LCS: data: +0010< 08004500 00540038 40004001 C423C0A8  ..E..T.8@.@..#.. ........ . .D.{y
    //   HHC00979D LCS: data: +0020< FA7DC0A8 FA7E0000 CFF40002 00010A05  .}...~.......... .'{y.=...4......
    //   HHC00979D LCS: data: +0030< 63600000 0000FDCF 06000000 00001011  c`.............. .-..............
    //   HHC00979D LCS: data: +0040< 12131415 16171819 1A1B1C1D 1E1F2021  .............. ! ................
    //   HHC00979D LCS: data: +0050< 22232425 26272829 2A2B2C2D 2E2F3031  "#$%&'()*+,-./01 ................
    //   HHC00979D LCS: data: +0060< 32333435 36370000                    234567..         ........
    // The following illustrates an LCS Command frame containing a Start
    // LAN command:-
    //   HHC00979D LCS: data: +0000< 001D0000 01000000 00000100 00000000  ................ ................
    //   HHC00979D LCS: data: +0010< 00000000 00000000 00000000 000000    ...............  ...............
    //

    if (pLCSDEV->bMode == LCSDEV_MODE_IP)
    {

        while (1)

        {
            // Fix-up the LCS header pointer to the current frame
            pLCSHDR = (PLCSHDR)( pIOBuf + iOffset );

            // Save current offset so we can tell how big next frame is
            iPrevOffset = iOffset;

            // Get the next frame offset, exit loop if 0
            FETCH_HW( iOffset, pLCSHDR->hwOffset );

            if (iOffset == 0)   // ("EOF")
                break;

            // Calculate size of this LCS Frame
            iLength = iOffset - iPrevOffset;

            switch (pLCSHDR->bType)
            {
            case LCS_FRMTYP_ENET:   // Ethernet Passthru

                PTT_DEBUG( "WRIT: Eth frame   ", 000, pDEVBLK->devnum, -1 );

                pLCSEthFrame = (PLCSETHFRM) pLCSHDR;
                pEthFrame    = (PETHFRM) pLCSEthFrame->bData;
                iEthLen      = iLength - sizeof(LCSETHFRM);

                // Fill in LCS source MAC address if not specified by guest program
                if (memcmp( pEthFrame->bSrcMAC, zeromac, sizeof( MAC )) == 0)
                {
                    memcpy( pEthFrame->bSrcMAC, pLCSPORT->MAC_Address, sizeof( MAC ));
#if !defined( OPTION_TUNTAP_LCS_SAME_ADDR )
                    pEthFrame->bSrcMAC[5]++;    /* Get next MAC address */
#endif
                }

                // Perform outbound checksum offloading if necessary
                if (pLCSPORT->fDoCkSumOffload)
                {
                    PTT_TIMING( "beg csumoff", 0, iEthLen, 0 );
                    EtherIpv4CkSumOffload( (BYTE*) pEthFrame, iEthLen );
                    PTT_TIMING( "end csumoff", 0, iEthLen, 0 );
                }

                // Trace Ethernet frame before sending to TAP device
                if (pLCSBLK->fDebug)
                {
                    FETCH_HW( hwEthernetType, pEthFrame->hwEthernetType );
                    SET_CPKTTYPE( hwEthernetType, cPktType );

                    // "%1d:%04X %s: port %2.2X: Send frame of size %d bytes (with %s packet) to device %s"
                    WRMSG(HHC00983, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                                         pLCSHDR->bSlot, iEthLen, cPktType,
                                         pLCSPORT->szNetIfName );
                    net_data_trace( pDEVBLK, (BYTE*)pEthFrame, iEthLen, '<', 'D', "eth frame", 0 );
                }

                // Write the Ethernet frame to the TAP device
                nEthBytes += iEthLen;
                nEthFrames++;
                PTT_DEBUG( "WRIT: writing...  ", 000, pDEVBLK->devnum, -1 );
                PTT_TIMING( "b4 write", 0, iEthLen, 1 );
                if (TUNTAP_Write( pDEVBLK->fd, (BYTE*) pEthFrame, iEthLen ) != iEthLen)
                {
                    PTT_TIMING( "*WRITE ERR", 0, iEthLen, 1 );
                    // "%1d:%04X CTC: error writing to file %s: %s"
                    WRMSG( HHC00936, "E",
                            SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->filename,
                            strerror( errno ) );
                    pDEVBLK->sense[0] = SENSE_EC;
                    *pUnitStat = CSW_CE | CSW_DE | CSW_UC;
                    LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames );
                    PTT_DEBUG( "WRIT EXIT         ", 000, pDEVBLK->devnum, -1 );
                    return;
                }
                PTT_TIMING( "af write", 0, iEthLen, 1 );
                break;

            case LCS_FRMTYP_CMD:    // LCS Command Frame

                pCmdFrame = (PLCSCMDHDR)pLCSHDR;

                PTT_DEBUG( "WRIT: Cmd frame   ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );

                // Trace received command frame...
                if (pLCSBLK->fDebug)
                {
                    // "%1d:%04X CTC: lcs command packet received"
                    WRMSG( HHC00922, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );
                    net_data_trace( pDEVBLK, (BYTE*)pCmdFrame, iLength, '<', 'D', "command", 0 );
                }

                // Ignore packets that appear to be inbound and not outbound.
                //
                // The Linux kernel LCS driver has two values defined that
                // might be found in variable pCmdFrame->bInitiator: #define
                // LCS_INITIATOR_TCPIP (0x00), and #define LCS_INITIATOR_LGW
                // (0x01), where 'LGW' is an abbreviation of 'LAN Gateway'.
                //
                // Older kernel LCS drivers had lots of code related to LGW,
                // but most of it has been removed from modern kernels (4.3,
                // at the time of writing).
                //
                // I'm not sure, but I think that applications, for example
                // IBM's Operator Facility/2, could send commands to a 3172
                // from a host attached to the LAN, and those commands would
                // arrive with pCmdFrame->bInitiator == LCS_INITIATOR_LGW.
                //
                // The current Linux kernel LCS driver only checks for the
                // pCmdFrame->bInitiator == LCS_INITIATOR_LGW in inbound
                // packets arriving from the LCS device; outbound packets
                // sent to the LCS device always have pCmdFrame->bInitiator
                // set to LCS_INITIATOR_TCPIP.

                if (pCmdFrame->bInitiator == LCS_INITIATOR_LGW)
                {
                    PTT_DEBUG( "CMD initiator LGW", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        // "%1d:%04X CTC: lcs command packet IGNORED (bInitiator == LGW)"
                        WRMSG( HHC00977, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );
                    break;
                }

                switch (pCmdFrame->bCmdCode)
                {
                    //  HHC00933  =  "%1d:%04X CTC: executing command %s"

                case LCS_CMD_STARTUP:       // Start Host
                    PTT_DEBUG( "CMD=StartUp       ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "startup" );
                    LCS_Startup( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_SHUTDOWN:      // Shutdown Host
                    PTT_DEBUG( "CMD=Shutdown      ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "shutdown" );
                    LCS_Shutdown( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_STRTLAN:       // Start LAN
                    PTT_DEBUG( "CMD=Start LAN     ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "start lan" );
                    LCS_StartLan( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_STOPLAN:       // Stop LAN
                    PTT_DEBUG( "CMD=Stop LAN      ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "stop lan" );
                    LCS_StopLan( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_QIPASSIST:     // Query IP Assists
                    PTT_DEBUG( "CMD=Query IPAssist", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "query IP assist" );
                    LCS_QueryIPAssists( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_LANSTAT:       // LAN Stats
                    PTT_DEBUG( "CMD=LAN Statistics", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "lan statistics" );
                    LCS_LanStats( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_SETIPM:        // Set IP Multicast
                    PTT_DEBUG( "CMD=Set IP Multicast", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "set multicast" );
                    LCS_AddMulticast( pLCSDEV, pCmdFrame, iLength );
                    break;

                case LCS_CMD_DELIPM:        // Delete IP Multicast
                    PTT_DEBUG( "CMD=Delete IP Multicast", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "delete multicast" );
                    LCS_DelMulticast( pLCSDEV, pCmdFrame, iLength );
                    break;

//              case LCS_CMD_GENSTAT:       // General Stats
//              case LCS_CMD_LISTLAN:       // List LAN
//              case LCS_CMD_LISTLAN2:      // List LAN (another version)
//              case LCS_CMD_TIMING:        // Timing request
                default:
                    PTT_DEBUG( "*CMD=Unsupported! ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                    {
                        // "%1d:%04X CTC: executing command %s"
                        MSGBUF( buf, "other (0x%2.2X)", pCmdFrame->bCmdCode );
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, buf );
                    }
                    LCS_DefaultCmdProc( pLCSDEV, pCmdFrame, iLength );
                    break;

                } // end switch (LCS Command Frame cmd code)

                break; // end case LCS_FRMTYP_CMD

            default:
                PTT_DEBUG( "*WRIT Unsupp frame", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                // "%1d:%04X CTC: lcs write: unsupported frame type 0x%2.2X"
                WRMSG( HHC00937, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pLCSHDR->bType );
                ASSERT( FALSE );
                pDEVBLK->sense[0] = SENSE_EC;
                *pUnitStat = CSW_CE | CSW_DE | CSW_UC;
                LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames );
                PTT_TIMING( "end write",  0, 0, 0 );
                PTT_DEBUG( "WRIT EXIT         ", 000, pDEVBLK->devnum, -1 );
                return;

            } // end switch (LCS Frame type)

        } // end while (1) for IP

    }

    // ----------------------------------------------------------------
    //    LCS_Write for SNA mode
    // ----------------------------------------------------------------
    //
    // SNA mode is, inevitably, more complicated. When the XCA is activated
    // the first two things sent from VTAM are LCS commands frames, both
    // prefixed with a 4-byte LCSHDR, the first an SNA Start LAN command,
    // the second an SNA LAN Statistics command. The following illustrates
    // an SNA Start LAN command:-
    //   HHC00979D LCS: data: +0000< 00160000 41000000 00000000 00000000  ....A........... ................
    //   HHC00979D LCS: data: +0010< 00000000 00000000                    ........         ........
    //
    // After that uncertainty reigns! So far there have only been two more
    // things sent from VTAM, the first is this:-
    //   HHC00979D LCS: data: +0000< 00160000 00000000 00140400 000C0C99  ................ ...............r
    //   HHC00979D LCS: data: +0010< 0003C000 00000000 01000000 0000      ..............   ..{...........
    // and 30 seconds later is this:-
    //   HHC00979D LCS: data: +0000< 00160000 00000000 00140000 42000002  ............B... ................
    //   HHC00979D LCS: data: +0010< 00000000 00000000 00000000 0000      ..............   ..............
    //
    // Are the first 8-bytes a structure, with the first 2-bytes of the
    // containing the length of whatever follows the structure?
    // Are the bytes following the structure an LCSHDR with, in the first
    // instance, some kind of SNA data, and, in the second instance, an
    // SNA Stop LAN command?
    // Are the 0x0016 and 0x0014 just coincidence?
    // We need a trace of real hardware!
    //
    // When the 8-byte structure is in a reply to VTAM the 8-bytes are
    // copied to an XCNCB, and various bits in the third byte are tested.
    //


    else  //  (pLCSDEV->bMode == LCSDEV_MODE_SNA)
    {

        FETCH_HW( hwBaffleLen, pIOBuf );
        iLength = sCount - SIZEOF_BAFFLE;

        if ( hwBaffleLen == iLength &&                   // First two bytes contain length?, and
             pIOBuf[2] == 0x00 &&                        // third byte is nulls?, and
             memcmp( &pIOBuf[2], &pIOBuf[3], 5 ) == 0 )  // third to eighth byte are all nulls?
        {
            bBafflePres = TRUE;
            pIOBufStart = &pIOBuf[SIZEOF_BAFFLE];
        }
        else
        {
            hwBaffleLen = 0;
            bBafflePres = FALSE;
            pIOBufStart = pIOBuf;
        }

        while (1)
        {
            // Fix-up the LCS header pointer to the current frame
            pLCSHDR = (PLCSHDR)( pIOBufStart + iOffset );

            // Save current offset so we can tell how big next frame is
            iPrevOffset = iOffset;

            // Get the next frame offset, exit loop if 0
            FETCH_HW( iOffset, pLCSHDR->hwOffset );

            if (iOffset == 0)   // ("EOF")
                break;

            // Calculate size of this LCS Frame
            iLength = iOffset - iPrevOffset;

            switch (pLCSHDR->bType)
            {
            case LCS_FRMTYP_ENET:   // Ethernet Passthru

                PTT_DEBUG( "WRIT: Eth frame   ", 000, pDEVBLK->devnum, -1 );

                pLCSEthFrame = (PLCSETHFRM) pLCSHDR;
                pEthFrame    = (PETHFRM) pLCSEthFrame->bData;
                iEthLen      = iLength - sizeof(LCSETHFRM);

                // Fill in LCS source MAC address if not specified by guest program
                if (memcmp( pEthFrame->bSrcMAC, zeromac, sizeof( MAC )) == 0)
                {
                    memcpy( pEthFrame->bSrcMAC, pLCSPORT->MAC_Address, sizeof( MAC ));
#if !defined( OPTION_TUNTAP_LCS_SAME_ADDR )
                    pEthFrame->bSrcMAC[5]++;    /* Get next MAC address */
#endif
                }

                // Trace Ethernet frame before sending to TAP device
                if (pLCSBLK->fDebug)
                {
                    FETCH_HW( hwEthernetType, pEthFrame->hwEthernetType );
                    SET_CPKTTYPE( hwEthernetType, cPktType );

                    // "%1d:%04X %s: port %2.2X: Send frame of size %d bytes (with %s packet) to device %s"
                    WRMSG(HHC00983, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                                         pLCSHDR->bSlot, iEthLen, cPktType,
                                         pLCSPORT->szNetIfName );
                    net_data_trace( pDEVBLK, (BYTE*)pEthFrame, iEthLen, '<', 'D', "eth frame", 0 );
                }

                // Write the Ethernet frame to the TAP device
                nEthBytes += iEthLen;
                nEthFrames++;
                PTT_DEBUG( "WRIT: writing...  ", 000, pDEVBLK->devnum, -1 );
                PTT_TIMING( "b4 write", 0, iEthLen, 1 );
                if (TUNTAP_Write( pDEVBLK->fd, (BYTE*) pEthFrame, iEthLen ) != iEthLen)
                {
                    PTT_TIMING( "*WRITE ERR", 0, iEthLen, 1 );
                    // "%1d:%04X CTC: error writing to file %s: %s"
                    WRMSG( HHC00936, "E",
                            SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->filename,
                            strerror( errno ) );
                    pDEVBLK->sense[0] = SENSE_EC;
                    *pUnitStat = CSW_CE | CSW_DE | CSW_UC;
                    LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames );
                    PTT_DEBUG( "WRIT EXIT         ", 000, pDEVBLK->devnum, -1 );
                    return;
                }
                PTT_TIMING( "af write", 0, iEthLen, 1 );
                break;

            case 0x04:              // LCS Baffle

                pCmdFrame = (PLCSCMDHDR)pLCSHDR;           /* FixMe! Need a structure! */

                PTT_DEBUG( "WRIT: Baffle      ", -1, pDEVBLK->devnum, -1 );

                // Trace received command frame...
                if (pLCSBLK->fDebug)
                {
//                  // "%1d:%04X CTC: lcs command packet received"
// FixMe!           WRMSG( HHC00922, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );  /* FixMe! Need a message! */
      {                                                                          /* FixMe! Remove! */
          char    tmp[256];
          snprintf( (char*)tmp, 256, "lcs baffle sna thingy received" );
          // HHC03983 "%1d:%04X %s: %s"
          WRMSG( HHC03983, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum, "LCS", tmp );
      }                                                                          /* FixMe! Remove! */
                    net_data_trace( pDEVBLK, (BYTE*)pCmdFrame, iLength, '<', 'D', "baffle", 0 );
                }

                PTT_DEBUG( "Baffle SNA        ", -1, pDEVBLK->devnum, -1 );
                if (pLCSBLK->fDebug)
                {
// FixMe!           WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "baffle sna" );
      {                                                                          /* FixMe! Remove! */
          char    tmp[256];
          snprintf( (char*)tmp, 256, "lcs processing baffle sna thingy" );
          // HHC03983 "%1d:%04X %s: %s"
          WRMSG( HHC03983, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum, "LCS", tmp );
      }                                                                          /* FixMe! Remove! */
                }

                LCS_Baffle_SNA( pLCSDEV, pCmdFrame, iLength, bBafflePres );

                break;

            case LCS_FRMTYP_CMD:    // LCS Command Frame

                pCmdFrame = (PLCSCMDHDR)pLCSHDR;

                PTT_DEBUG( "WRIT: Cmd frame   ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );

                // Trace received command frame...
                if (pLCSBLK->fDebug)
                {
                    // "%1d:%04X CTC: lcs command packet received"
                    WRMSG( HHC00922, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );
                    net_data_trace( pDEVBLK, (BYTE*)pCmdFrame, iLength, '<', 'D', "command", 0 );
                }

                switch (pCmdFrame->bCmdCode)
                {
                    //  HHC00933  =  "%1d:%04X CTC: executing command %s"

                case LCS_CMD_STRTLAN_SNA:   // Start LAN SNA
                    PTT_DEBUG( "CMD=Start LAN SNA ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "start lan sna" );
                    LCS_StartLan_SNA( pLCSDEV, pCmdFrame, iLength, bBafflePres );
                    break;

                case LCS_CMD_STOPLAN_SNA:   // Stop LAN SNA
                    PTT_DEBUG( "CMD=Stop LAN SNA  ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "stop lan sna" );
                    LCS_StopLan_SNA( pLCSDEV, pCmdFrame, iLength, bBafflePres );
                    break;

                case LCS_CMD_LANSTAT_SNA:   // LAN Stats SNA
                    PTT_DEBUG( "CMD=LAN Stats SNA ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, "lan statistics sna" );
                    LCS_LanStats_SNA( pLCSDEV, pCmdFrame, iLength, bBafflePres );
                    break;

                default:
                    PTT_DEBUG( "*CMD=Unsupported! ", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                    if (pLCSBLK->fDebug)
                    {
                        // "%1d:%04X CTC: executing command %s"
                        MSGBUF( buf, "other (0x%2.2X)", pCmdFrame->bCmdCode );
                        WRMSG( HHC00933, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, buf );
                    }
                    LCS_DefaultCmd_SNA( pLCSDEV, pCmdFrame, iLength, bBafflePres );
                    break;

                } // end switch (LCS Command Frame cmd code)

                // The command reply has been enqueued in the LCSDEV->bFrameBuffer.
                // We must generate an Attention interrupt, to trigger the guest into
                // issuing a Read. Prompt LCS_AttnThread to generate the Attention.

                /* Create an LCSATTN block */
                pLCSATTN = malloc( sizeof( LCSATTN ) );
                if (!pLCSATTN) break;  /* FixMe! Produce a message? */
                pLCSATTN->pNext = NULL;
                pLCSATTN->pDevice = pLCSDEV;

//              if (pLCSBLK->fDebug)                                                                         /* FixMe! Remove! */
//                net_data_trace( pDEVBLK, (BYTE*)pLCSATTN, sizeof( LCSATTN ), ' ', 'D', "LCSATTN in", 0 );  /* FixMe! Remove! */

                /* Add LCSATTN block to start of chain */
                PTT_DEBUG( "GET  AttnLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                obtain_lock( &pLCSBLK->AttnLock );
                PTT_DEBUG( "GOT  AttnLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                {
                    PTT_DEBUG( "ADD  Attn", pLCSATTN, pDEVBLK->devnum, pLCSPORT->bPort );
                    pLCSATTN->pNext = pLCSBLK->pAttns;
                    pLCSBLK->pAttns = pLCSATTN;
                }
                PTT_DEBUG( "REL  AttnLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                release_lock( &pLCSBLK->AttnLock );

                /* Signal the LCS_AttnThread to process the LCSATTN block(s) on the chain */
                PTT_DEBUG( "GET  AttnEventLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                obtain_lock( &pLCSBLK->AttnEventLock );
                PTT_DEBUG( "GOT  AttnEventLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                {
                    PTT_DEBUG( "SIG  AttnEvent", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                    signal_condition( &pLCSBLK->AttnEvent );
                }
                PTT_DEBUG( "REL  AttnEventLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                release_lock( &pLCSBLK->AttnEventLock );

                break; // end case LCS_FRMTYP_CMD

            default:
                PTT_DEBUG( "*WRIT Unsupp frame", pCmdFrame->bCmdCode, pDEVBLK->devnum, -1 );
                // "%1d:%04X CTC: lcs write: unsupported frame type 0x%2.2X"
                WRMSG( HHC00937, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pLCSHDR->bType );
                ASSERT( FALSE );
                pDEVBLK->sense[0] = SENSE_EC;
                *pUnitStat = CSW_CE | CSW_DE | CSW_UC;
                LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames );
                PTT_TIMING( "end write",  0, 0, 0 );
                PTT_DEBUG( "WRIT EXIT         ", 000, pDEVBLK->devnum, -1 );
                return;

            } // end switch (LCS Frame type)

        } // end while (1) for SNA

    }

    // ----------------------------------------------------------------
    //    End of LCS_Write for IP or SNA mode
    // ----------------------------------------------------------------

    LCS_EndMWrite( pDEVBLK, nEthBytes, nEthFrames ); // (performance)

    *pResidual = 0;
    *pUnitStat = CSW_CE | CSW_DE;

    PTT_TIMING( "end write",  0, 0, 0 );
    PTT_DEBUG( "WRIT EXIT         ", 000, pDEVBLK->devnum, -1 );
}   // End of LCS_Write

// ====================================================================
//                         LCS_Startup
// ====================================================================

static void  LCS_Startup( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCSSTRTFRM  Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTRTFRM pLCSSTRTFRM = (PLCSSTRTFRM)&Reply;
    PLCSPORT    pLCSPORT;
    U16         iOrigMaxFrameBufferSize;

    INIT_REPLY_FRAME( pLCSSTRTFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTRTFRM->bLCSCmdHdr.bLanType      = LCS_FRMTYP_ENET;
    pLCSSTRTFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;

    // Save the max buffer size parameter
    iOrigMaxFrameBufferSize = pLCSDEV->iMaxFrameBufferSize;
    // If original is 0 set to compiled in buffer size
    if(!iOrigMaxFrameBufferSize) iOrigMaxFrameBufferSize=(U16)sizeof(pLCSDEV->bFrameBuffer);

    FETCH_HW( pLCSDEV->iMaxFrameBufferSize, ((PLCSSTRTFRM)pCmdFrame)->hwBufferSize );

    // Make sure it doesn't exceed our compiled maximum
    if (pLCSDEV->iMaxFrameBufferSize > sizeof(pLCSDEV->bFrameBuffer))
    {
        // "%1d:%04X CTC: lcs startup: frame buffer size 0x%4.4X '%s' compiled size 0x%4.4X: ignored"
        WRMSG(HHC00939, "W", SSID_TO_LCSS(pLCSDEV->pDEVBLK[1]->ssid),
            pLCSDEV->pDEVBLK[1]->devnum,
            pLCSDEV->iMaxFrameBufferSize,
            "LCS",
            (int)sizeof( pLCSDEV->bFrameBuffer ) );
        pLCSDEV->iMaxFrameBufferSize = iOrigMaxFrameBufferSize;
    }
    else
    {
        // Make sure it's not smaller than the compiled minimum size
        if (pLCSDEV->iMaxFrameBufferSize < CTC_MIN_FRAME_BUFFER_SIZE)
        {
            // "%1d:%04X CTC: lcs startup: frame buffer size 0x%4.4X '%s' compiled size 0x%4.4X: ignored"
            WRMSG(HHC00939, "W", SSID_TO_LCSS(pLCSDEV->pDEVBLK[1]->ssid),
                pLCSDEV->pDEVBLK[1]->devnum,
                pLCSDEV->iMaxFrameBufferSize,
                "LCS",
                CTC_MIN_FRAME_BUFFER_SIZE );
            pLCSDEV->iMaxFrameBufferSize = iOrigMaxFrameBufferSize;
        }
    }

    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];

    if (!pLCSPORT->fPreconfigured)
    {
        VERIFY( TUNTAP_SetIPAddr( pLCSPORT->szNetIfName, "0.0.0.0" ) == 0 );
        VERIFY( TUNTAP_SetMTU   ( pLCSPORT->szNetIfName,  "1500"   ) == 0 );
#ifdef OPTION_TUNTAP_SETMACADDR
        if (pLCSPORT->fLocalMAC)
        {
            VERIFY( TUNTAP_SetMACAddr( pLCSPORT->szNetIfName,
                                       pLCSPORT->szMACAddress ) == 0 );
        }
#endif // OPTION_TUNTAP_SETMACADDR
    }

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTRTFRM, iReplyLen, 0 );

    pLCSDEV->fDevStarted = 1;
}

// ====================================================================
//                         LCS_Shutdown
// ====================================================================

static void  LCS_Shutdown( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
//    char        Reply[128];
    LCSSTDFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTDFRM  pLCSSTDFRM = (PLCSSTDFRM)&Reply;

    INIT_REPLY_FRAME( pLCSSTDFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTDFRM->bLCSCmdHdr.bLanType      = LCS_FRMTYP_ENET;
    pLCSSTDFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTDFRM, iReplyLen, 0 );

    pLCSDEV->fDevStarted = 0;
}

// ====================================================================
//                       UpdatePortStarted
// ====================================================================

static void  UpdatePortStarted( int bStarted, DEVBLK* pDEVBLK, PLCSPORT pLCSPORT )
{
    PTT_DEBUG(        "GET  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortDataLock );
    PTT_DEBUG(        "GOT  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // The following will either caise the LCS_PortThread to start
        // reading packets or stop reading packets (fPortStarted = 1/0)

        PTT_DEBUG( "UPDTPORTSTARTED   ", bStarted, pDEVBLK->devnum, pLCSPORT->bPort );
        pLCSPORT->fPortStarted = bStarted;
    }
    PTT_DEBUG(         "REL  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortDataLock );

    if (pDEVBLK->ccwtrace || pDEVBLK->ccwstep || pLCSPORT->pLCSBLK->fDebug)
        // "%1d:%04X CTC: lcs triggering port %2.2X event"
        WRMSG( HHC00966, "I", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum, pLCSPORT->bPort );

    PTT_DEBUG(        "GET  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortEventLock );
    PTT_DEBUG(        "GOT  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // Wake up the LCS_PortThread...

        PTT_DEBUG(             "SIG  PortEvent    ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
        signal_condition( &pLCSPORT->PortEvent );
    }
    PTT_DEBUG(         "REL  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortEventLock );

    PTT_DEBUG( "UPDTPORT pause 150", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    usleep( 150*1000 );
}

// ====================================================================
//                         LCS_StartLan
// ====================================================================

static void  LCS_StartLan( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCSSTRTFRM  Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTRTFRM pLCSSTRTFRM = (PLCSSTRTFRM)&Reply;
    PLCSPORT    pLCSPORT;
#ifdef OPTION_TUNTAP_DELADD_ROUTES
    PLCSRTE     pLCSRTE;
#endif // OPTION_TUNTAP_DELADD_ROUTES
    DEVBLK*     pDEVBLK;
    int         nIFFlags;
    U8          fStartPending = 0;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];
    pDEVBLK  = pLCSDEV->pDEVBLK[ LCSDEV_WRITE_SUBCHANN ];
    if (!pDEVBLK) pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];  /* SNA has only one device */

    INIT_REPLY_FRAME( pLCSSTRTFRM, iReplyLen, pCmdFrame, iCmdLen );

    // Serialize access to eliminate ioctl errors
    PTT_DEBUG(        "GET  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortDataLock );
    PTT_DEBUG(        "GOT  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // Configure the TAP interface if used
        PTT_DEBUG( "STRTLAN if started", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );
        if (pLCSPORT->fUsed && pLCSPORT->fPortCreated && !pLCSPORT->fPortStarted)
        {
            PTT_DEBUG( "STRTLAN started=NO", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            nIFFlags =              // Interface flags
                0
                | IFF_UP            // (interface is being enabled)
                | IFF_BROADCAST     // (interface broadcast addr is valid)
                ;

#if defined( TUNTAP_IFF_RUNNING_NEEDED )

            nIFFlags |=             // ADDITIONAL Interface flags
                0
                | IFF_RUNNING       // (interface is ALSO operational)
                ;

#endif /* defined( TUNTAP_IFF_RUNNING_NEEDED ) */

            // Enable the interface by turning on the IFF_UP flag...
            // This lets the packets start flowing...

            if (!pLCSPORT->fPreconfigured)
                VERIFY( TUNTAP_SetFlags( pLCSPORT->szNetIfName, nIFFlags ) == 0 );

            fStartPending = 1;

#ifdef OPTION_TUNTAP_DELADD_ROUTES

            // Add any needed extra routing entries the
            // user may have specified in their OAT file
            // to the host's routing table...

            if (!pLCSPORT->fPreconfigured)
            {
                for (pLCSRTE = pLCSPORT->pRoutes; pLCSRTE; pLCSRTE = pLCSRTE->pNext)
                {
                    VERIFY( TUNTAP_AddRoute( pLCSPORT->szNetIfName,
                                     pLCSRTE->pszNetAddr,
                                     pLCSRTE->pszNetMask,
                                     NULL,
                                     RTF_UP ) == 0 );
                }
            }
#endif // OPTION_TUNTAP_DELADD_ROUTES
        }
    }
    PTT_DEBUG(         "REL  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortDataLock );

#ifdef OPTION_TUNTAP_DELADD_ROUTES

    // Add a Point-To-Point routing entry to the
    // host's routing table for our interface...

    if (!pLCSPORT->fPreconfigured)
    {
        if (pLCSDEV->pszIPAddress)
        {
            VERIFY( TUNTAP_AddRoute( pLCSPORT->szNetIfName,
                             pLCSDEV->pszIPAddress,
                             "255.255.255.255",
                             NULL,
                             RTF_UP | RTF_HOST ) == 0 );
        }
    }
#endif // OPTION_TUNTAP_DELADD_ROUTES

    // PROGRAMMING NOTE: it's important to enqueue the reply frame BEFORE
    // we trigger the LCS_PortThread to start reading the adapter and
    // begin enqueuing Ethernet frames. This is so the guest receives
    // the reply to its cmd BEFORE it sees any Ethernet packets that might
    // result from its StartLAN cmd.

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTRTFRM, iReplyLen, 0 );

    if (fStartPending)
        UpdatePortStarted( TRUE, pDEVBLK, pLCSPORT );
}

// ====================================================================
//                         LCS_StopLan
// ====================================================================

static void  LCS_StopLan( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCSSTDFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTDFRM  pLCSSTDFRM = (PLCSSTDFRM)&Reply;
    PLCSPORT    pLCSPORT;
#ifdef OPTION_TUNTAP_DELADD_ROUTES
    PLCSRTE     pLCSRTE;
#endif // OPTION_TUNTAP_DELADD_ROUTES
    DEVBLK*     pDEVBLK;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[ pLCSDEV->bPort ];
    pDEVBLK  =  pLCSDEV->pDEVBLK[ LCSDEV_WRITE_SUBCHANN ];
    if (!pDEVBLK) pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];  /* SNA has only one device */

    INIT_REPLY_FRAME( pLCSSTDFRM, iReplyLen, pCmdFrame, iCmdLen );

    // Serialize access to eliminate ioctl errors
    PTT_DEBUG(        "GET  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortDataLock );
    PTT_DEBUG(        "GOT  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // Disable the interface by turning off the IFF_UP flag...
        if (!pLCSPORT->fPreconfigured)
            VERIFY( TUNTAP_SetFlags( pLCSPORT->szNetIfName, 0 ) == 0 );

#ifdef OPTION_TUNTAP_DELADD_ROUTES

        // Remove routing entries from host's routing table...

        // First, remove the Point-To-Point routing entry
        // we added when we brought the interface IFF_UP...

        if (!pLCSPORT->fPreconfigured)
        {
            if (pLCSDEV->pszIPAddress)
            {
                VERIFY( TUNTAP_DelRoute( pLCSPORT->szNetIfName,
                                 pLCSDEV->pszIPAddress,
                                 "255.255.255.255",
                                 NULL,
                                 RTF_HOST ) == 0 );
            }
        }

        // Next, remove any extra routing entries
        // (specified by the user in their OAT file)
        // that we may have also added...

        if (!pLCSPORT->fPreconfigured)
        {
            for (pLCSRTE = pLCSPORT->pRoutes; pLCSRTE; pLCSRTE = pLCSRTE->pNext)
            {
                VERIFY( TUNTAP_DelRoute( pLCSPORT->szNetIfName,
                                 pLCSRTE->pszNetAddr,
                                 pLCSRTE->pszNetMask,
                                 NULL,
                                 RTF_UP ) == 0 );
            }
        }
#endif // OPTION_TUNTAP_DELADD_ROUTES
    }
    PTT_DEBUG(         "REL  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortDataLock );

    // Now that the tuntap device has been stopped and the packets
    // are no longer flowing, tell the LCS_PortThread to stop trying
    // to read from the tuntap device (adapter).

    UpdatePortStarted( FALSE, pDEVBLK, pLCSPORT );

    // Now that we've stopped new packets from being added to our
    // frame buffer we can now finally enqueue our reply frame
    // to our frame buffer (so LCS_Read can return it to the guest).

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTDFRM, iReplyLen, 0 );
}

// ====================================================================
//                      LCS_QueryIPAssists
// ====================================================================

static void  LCS_QueryIPAssists( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCSQIPFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSQIPFRM  pLCSQIPFRM = (PLCSQIPFRM)&Reply;
    PLCSPORT    pLCSPORT;

    INIT_REPLY_FRAME( pLCSQIPFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];

    STORE_HW( pLCSQIPFRM->hwNumIPPairs,         _countof( pLCSPORT->MCastTab ));
    STORE_HW( pLCSQIPFRM->hwIPAssistsSupported, pLCSPORT->sIPAssistsSupported );
    STORE_HW( pLCSQIPFRM->hwIPAssistsEnabled,   pLCSPORT->sIPAssistsEnabled   );
    STORE_HW( pLCSQIPFRM->hwIPVersion,          0x0004 ); // (IPv4 only)

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSQIPFRM, iReplyLen, 0 );
}

// ====================================================================
//                         LCS_LanStats
// ====================================================================

static void  LCS_LanStats( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{

    LCSLSTFRM  Reply;
    int        iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSLSTFRM pLCSLSTFRM = (PLCSLSTFRM)&Reply;
    PLCSPORT   pLCSPORT;
    BYTE*      pPortMAC;
    BYTE*      pIFaceMAC;
    int        fd, rc, success;
    ifreq      ifr;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];
    pPortMAC = (BYTE*) &pLCSPORT->MAC_Address;
    pIFaceMAC = pPortMAC;

    /* Not all systems can return the hardware address of an interface. */
#if defined(SIOCGIFHWADDR)

    while (1)
    {
        fd = socket( AF_INET, SOCK_STREAM, IPPROTO_IP );

        if (fd == -1)
        {
            // "CTC: error in function %s: %s"
            rc = HSO_errno;
            WRMSG( HHC00940, "E", "socket()", strerror( rc ) );
            success = FALSE;
            break;
        }

        memset( &ifr, 0, sizeof( ifr ) );
        STRLCPY( ifr.ifr_name, pLCSPORT->szNetIfName );

        rc = TUNTAP_IOCtl( fd, SIOCGIFHWADDR, (char*)&ifr );

        close( fd );

        if (rc != 0)
        {
            // "CTC: ioctl %s failed for device %s: %s"
            rc = HSO_errno;
            WRMSG( HHC00941, "E", "SIOCGIFHWADDR", pLCSPORT->szNetIfName, strerror( rc ) );
            success = FALSE;
            break;
        }

        pIFaceMAC  = (BYTE*) ifr.ifr_hwaddr.sa_data;
        rc = 0;
        success = TRUE;
        break;
    }

#else // !defined(SIOCGIFHWADDR)

    rc = 0;
    success = TRUE;

#endif // defined(SIOCGIFHWADDR)

    if (success)
    {
        /* Report what MAC address we will really be using */
        // "CTC: lcs device '%s' using mac %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
        WRMSG( HHC00942, "I", pLCSPORT->szNetIfName, *(pIFaceMAC+0),*(pIFaceMAC+1),
                                        *(pIFaceMAC+2),*(pIFaceMAC+3),
                                        *(pIFaceMAC+4),*(pIFaceMAC+5));

        /* Issue warning if different from specified value */
        if (memcmp( pPortMAC, pIFaceMAC, IFHWADDRLEN ) != 0)
        {
            if (pLCSPORT->fLocalMAC)
            {
                // "CTC: lcs device %s not using mac %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
                WRMSG( HHC00943, "W", pLCSPORT->szNetIfName, *(pPortMAC+0),*(pPortMAC+1),
                                                *(pPortMAC+2),*(pPortMAC+3),
                                                *(pPortMAC+4),*(pPortMAC+5));
            }

            memcpy( pPortMAC, pIFaceMAC, IFHWADDRLEN );

            snprintf(pLCSPORT->szMACAddress, sizeof(pLCSPORT->szMACAddress),
                "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X", *(pPortMAC+0), *(pPortMAC+1),
                *(pPortMAC+2), *(pPortMAC+3), *(pPortMAC+4), *(pPortMAC+5));
        }
    }

    INIT_REPLY_FRAME( pLCSLSTFRM, iReplyLen, pCmdFrame, iCmdLen );

    /* Respond with a different MAC address for the LCS side */
    /* unless the TAP mechanism is designed as such          */
    /* cf : hostopts.h for an explanation                    */
    iReplyLen = sizeof(Reply);
    STORE_HW( pLCSLSTFRM->bLCSCmdHdr.hwReturnCode, (S16) rc );
    memcpy( pLCSLSTFRM->MAC_Address, pIFaceMAC, IFHWADDRLEN );
#if !defined( OPTION_TUNTAP_LCS_SAME_ADDR )
    pLCSLSTFRM->MAC_Address[5]++;
#endif
    // FIXME: Really should read /proc/net/dev to retrieve actual stats

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSLSTFRM, iReplyLen, 0 );
}

// ====================================================================
//                       LCS_DoMulticast
// ====================================================================

static  void  LCS_DoMulticast( int ioctlcode, PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{

    LCSIPMFRM         Reply;
    int               iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSIPMFRM        pLCSIPMFRM = (PLCSIPMFRM)&Reply;
    const LCSIPMFRM*  pIPMFrame;
    LCSPORT*          pLCSPORT;
    const MAC*        pMAC;
    ifreq             ifr = {0};
    int               rc, badrc = 0, errnum = 0;
    U16               i, numpairs;
    char*             pszMAC;
    const char*       what;

    // Initialize reply frame

    INIT_REPLY_FRAME( pLCSIPMFRM, iReplyLen, pCmdFrame, iCmdLen );
    pIPMFrame = (const LCSIPMFRM*) pCmdFrame;
    pLCSPORT  = &pLCSDEV->pLCSBLK->Port[ pLCSDEV->bPort ];

    // Retrieve number of MAC addresses in their request

    FETCH_HW( numpairs, pIPMFrame->hwNumIPPairs );
    if (numpairs > MAX_IP_MAC_PAIRS)
        numpairs = MAX_IP_MAC_PAIRS;

    // If tuntap multicast assist is available, use it.
    // Otherwise keep track of guest's request manually.

    if (pLCSPORT->fDoMCastAssist)  // (manual multicast assist?)
    {
        what = (U32) SIOCADDMULTI == (U32) ioctlcode ? "MACTabAdd"
             : (U32) SIOCDELMULTI == (U32) ioctlcode ? "MACTabRem" : "???";

        for (i=0, badrc=0; i < numpairs; i++)
        {
            pMAC = &pIPMFrame->IP_MAC_Pair[i].MAC_Address;

            // Remember (or forget) this MAC for later

            if ((U32) SIOCADDMULTI == (U32) ioctlcode)
            {
                if ((rc = MACTabAdd( pLCSPORT->MCastTab, (BYTE*) pMAC, 0 )) == 0)
                {
                    pLCSPORT->nMCastCount++;

                    if (pLCSDEV->pLCSBLK->fDebug)
                    {
                        VERIFY( FormatMAC( &pszMAC, (BYTE*) pMAC ) == 0);
                        // "CTC: lcs device '%s' port %2.2X: %s %s: ok"
                        WRMSG( HHC00964, "D", pLCSPORT->szNetIfName, pLCSPORT->bPort,
                            what, pszMAC );
                        free( pszMAC );
                    }
                }
                else
                    badrc = -rc;    // (convert to errno)
            }
            else // ((U32) SIOCDELMULTI == (U32) ioctlcode)
            {
                if ((rc = MACTabRem( pLCSPORT->MCastTab, (BYTE*) pMAC )) == 0)
                {
                    pLCSPORT->nMCastCount--;

                    if (pLCSDEV->pLCSBLK->fDebug)
                    {
                        VERIFY( FormatMAC( &pszMAC, (BYTE*) pMAC ) == 0);
                        // "CTC: lcs device '%s' port %2.2X: %s %s: ok"
                        WRMSG( HHC00964, "D", pLCSPORT->szNetIfName, pLCSPORT->bPort,
                            what, pszMAC );
                        free( pszMAC );
                    }
                }
                else
                    badrc = -rc;    // (convert to errno)
            }
        }

        // Set return code and issue message if any requests failed.

        if (badrc)
        {
            errnum = badrc;  // (get errno)
            // "CTC: error in function %s: %s"
            WRMSG( HHC00940, "E", what, strerror( errnum ));
            STORE_HW( pLCSIPMFRM->bLCSCmdHdr.hwReturnCode, 0xFFFF );
        }
    }
    else // (!pLCSPORT->fDoMCastAssist): let tuntap do it for us
    {
        // Issue ioctl for each MAC address in their request

        what = (U32) SIOCADDMULTI == (U32) ioctlcode ? "SIOCADDMULTI"
             : (U32) SIOCDELMULTI == (U32) ioctlcode ? "SIOCDELMULTI" : "???";

        STRLCPY( ifr.ifr_name, pLCSPORT->szNetIfName );

#if defined( SIOCGIFHWADDR )
        for (i=0, badrc=0; i < numpairs; i++)
        {
            pMAC = &pIPMFrame->IP_MAC_Pair[i].MAC_Address;
            memcpy( ifr.ifr_hwaddr.sa_data, pMAC, sizeof( MAC ));

            if ((rc = TUNTAP_IOCtl( 0, ioctlcode, (char*) &ifr )) == 0)
            {
                if (pLCSDEV->pLCSBLK->fDebug)
                {
                    VERIFY( FormatMAC( &pszMAC, (BYTE*) pMAC ) == 0);
                    // "CTC: lcs device '%s' port %2.2X: %s %s: ok"
                    WRMSG( HHC00964, "D", pLCSPORT->szNetIfName, pLCSPORT->bPort,
                        what, pszMAC );
                    free( pszMAC );
                }
            }
            else
            {
                badrc = rc;
                errnum = HSO_errno;
            }
        }

        // Issue error message if any of the requests failed.

        if (badrc)
        {
            // "CTC: ioctl %s failed for device %s: %s"
            WRMSG( HHC00941, "E", what, pLCSPORT->szNetIfName, strerror( errnum ));
            STORE_HW( pLCSIPMFRM->bLCSCmdHdr.hwReturnCode, 0xFFFF );
        }
#endif // defined( SIOCGIFHWADDR )
    }

    // Queue response back to caller

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSIPMFRM, iReplyLen, 0 );
}

// ====================================================================
//                       LCS_AddMulticast
// ====================================================================

static  void  LCS_AddMulticast( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCS_DoMulticast( SIOCADDMULTI, pLCSDEV, pCmdFrame, iCmdLen );
}

// ====================================================================
//                       LCS_DelMulticast
// ====================================================================

static  void  LCS_DelMulticast( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCS_DoMulticast( SIOCDELMULTI, pLCSDEV, pCmdFrame, iCmdLen );
}

// ====================================================================
//                       LCS_DefaultCmdProc
// ====================================================================

static void  LCS_DefaultCmdProc( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen )
{
    LCSSTDFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTDFRM  pLCSSTDFRM = (PLCSSTDFRM)&Reply;

    INIT_REPLY_FRAME( pLCSSTDFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTDFRM->bLCSCmdHdr.bLanType      = LCS_FRMTYP_ENET;
    pLCSSTDFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTDFRM, iReplyLen, 0 );
}

// ====================================================================
//                         LCS_StartLan_SNA
// ====================================================================

static void  LCS_StartLan_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres )
{
    LCSSTRTFRM  Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTRTFRM pLCSSTRTFRM = (PLCSSTRTFRM)&Reply;
    PLCSPORT    pLCSPORT;
    DEVBLK*     pDEVBLK;
    int         nIFFlags;
    U8          fStartPending = 0;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];
    pDEVBLK  = pLCSDEV->pDEVBLK[ LCSDEV_WRITE_SUBCHANN ];
    if (!pDEVBLK) pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];  /* SNA has only one device */

    INIT_REPLY_FRAME( pLCSSTRTFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTRTFRM->bLCSCmdHdr.bLCSHdr.bSlot = pLCSDEV->bPort;
    pLCSSTRTFRM->bLCSCmdHdr.bInitiator    = LCS_INITIATOR_SNA;
    pLCSSTRTFRM->bLCSCmdHdr.bLanType      = LCS_FRMTYP_ENET;
    pLCSSTRTFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;
    STORE_HW( pLCSSTRTFRM->hwBufferSize, pLCSDEV->iMaxFrameBufferSize );
    STORE_FW( pLCSSTRTFRM->fwUnknown, 0x00000800 );  /* 0x0800 to 0xFFFF */
    iReplyLen = sizeof(Reply);

    // Serialize access to eliminate ioctl errors
    PTT_DEBUG(        "GET  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortDataLock );
    PTT_DEBUG(        "GOT  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // Configure the TAP interface if used
        PTT_DEBUG( "STRTLAN if started", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );
        if (pLCSPORT->fUsed && pLCSPORT->fPortCreated && !pLCSPORT->fPortStarted)
        {
            PTT_DEBUG( "STRTLAN started=NO", 000, pDEVBLK->devnum, pLCSPORT->bPort );
            nIFFlags =              // Interface flags
                0
                | IFF_UP            // (interface is being enabled)
                | IFF_BROADCAST     // (interface broadcast addr is valid)
                ;

#if defined( TUNTAP_IFF_RUNNING_NEEDED )

            nIFFlags |=             // ADDITIONAL Interface flags
                0
                | IFF_RUNNING       // (interface is ALSO operational)
                ;

#endif /* defined( TUNTAP_IFF_RUNNING_NEEDED ) */

            // Enable the interface by turning on the IFF_UP flag...
            // This lets the packets start flowing...

            if (!pLCSPORT->fPreconfigured)
            {
                VERIFY( TUNTAP_SetFlags( pLCSPORT->szNetIfName, nIFFlags ) == 0 );
                VERIFY( TUNTAP_SetMTU( pLCSPORT->szNetIfName,  "1500"   ) == 0 );
#ifdef OPTION_TUNTAP_SETMACADDR
                if (pLCSPORT->fLocalMAC)
                {
                    VERIFY( TUNTAP_SetMACAddr( pLCSPORT->szNetIfName,
                                               pLCSPORT->szMACAddress ) == 0 );
                }
#endif // OPTION_TUNTAP_SETMACADDR
            }

            fStartPending = 1;
        }
    }
    PTT_DEBUG(         "REL  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortDataLock );

    // PROGRAMMING NOTE: it's important to enqueue the reply frame BEFORE
    // we trigger the LCS_PortThread to start reading the adapter and
    // begin enqueuing Ethernet frames. This is so the guest receives
    // the reply to its cmd BEFORE it sees any Ethernet packets that might
    // result from its StartLAN cmd.

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTRTFRM, iReplyLen, bBafflePres );

    if (fStartPending)
        UpdatePortStarted( TRUE, pDEVBLK, pLCSPORT );

    pLCSDEV->fDevStarted = 1;

}

// ====================================================================
//                         LCS_StopLan_SNA
// ====================================================================

static void  LCS_StopLan_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres )
{
    LCSSTDFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTDFRM  pLCSSTDFRM = (PLCSSTDFRM)&Reply;
    PLCSPORT    pLCSPORT;
    DEVBLK*     pDEVBLK;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[ pLCSDEV->bPort ];
    pDEVBLK  =  pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];  /* SNA has only one device */

    INIT_REPLY_FRAME( pLCSSTDFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTDFRM->bLCSCmdHdr.bLCSHdr.bSlot = pLCSDEV->bPort;
    pLCSSTDFRM->bLCSCmdHdr.bInitiator = LCS_INITIATOR_SNA;
//  pLCSSTDFRM->bLCSCmdHdr.bLanType = LCS_FRMTYP_SNA;
    pLCSSTDFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;

    // Serialize access to eliminate ioctl errors
    PTT_DEBUG(        "GET  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    obtain_lock( &pLCSPORT->PortDataLock );
    PTT_DEBUG(        "GOT  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    {
        // Disable the interface by turning off the IFF_UP flag...
        if (!pLCSPORT->fPreconfigured)
            VERIFY( TUNTAP_SetFlags( pLCSPORT->szNetIfName, 0 ) == 0 );

    }
    PTT_DEBUG(         "REL  PortDataLock ", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    release_lock( &pLCSPORT->PortDataLock );

    // Now that the tuntap device has been stopped and the packets
    // are no longer flowing, tell the LCS_PortThread to stop trying
    // to read from the tuntap device (adapter).

    UpdatePortStarted( FALSE, pDEVBLK, pLCSPORT );

    // Now that we've stopped new packets from being added to our
    // frame buffer we can now finally enqueue our reply frame
    // to our frame buffer (so LCS_Read can return it to the guest).

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTDFRM, iReplyLen, bBafflePres );

    pLCSDEV->fDevStarted = 0;
}

// ====================================================================
//                         LCS_LanStats_SNA
// ====================================================================

static void  LCS_LanStats_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres )
{

    LCSLSSFRM  Reply;
    int        iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSLSSFRM pLCSLSSFRM = (PLCSLSSFRM)&Reply;
    PLCSPORT   pLCSPORT;
    BYTE*      pPortMAC;
    BYTE*      pIFaceMAC;
    int        fd, rc, success;
    ifreq      ifr;


    pLCSPORT = &pLCSDEV->pLCSBLK->Port[pLCSDEV->bPort];
    pPortMAC = (BYTE*) &pLCSPORT->MAC_Address;
    pIFaceMAC = pPortMAC;

    /* Not all systems can return the hardware address of an interface. */
#if defined(SIOCGIFHWADDR)

    while (1)
    {
        fd = socket( AF_INET, SOCK_STREAM, IPPROTO_IP );

        if (fd == -1)
        {
            // "CTC: error in function %s: %s"
            rc = HSO_errno;
            WRMSG( HHC00940, "E", "socket()", strerror( rc ) );
            success = FALSE;
            break;
        }

        memset( &ifr, 0, sizeof( ifr ) );
        STRLCPY( ifr.ifr_name, pLCSPORT->szNetIfName );

        rc = TUNTAP_IOCtl( fd, SIOCGIFHWADDR, (char*)&ifr );

        close( fd );

        if (rc != 0)
        {
            // "CTC: ioctl %s failed for device %s: %s"
            rc = HSO_errno;
            WRMSG( HHC00941, "E", "SIOCGIFHWADDR", pLCSPORT->szNetIfName, strerror( rc ) );
            success = FALSE;
            break;
        }

        pIFaceMAC  = (BYTE*) ifr.ifr_hwaddr.sa_data;
        rc = 0;
        success = TRUE;
        break;
    }

#else // !defined(SIOCGIFHWADDR)

    rc = 0;
    success = TRUE;

#endif // defined(SIOCGIFHWADDR)

    if (success)
    {
        /* Report what MAC address we will really be using */
        // "CTC: lcs device '%s' using mac %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
        WRMSG( HHC00942, "I", pLCSPORT->szNetIfName, *(pIFaceMAC+0),*(pIFaceMAC+1),
                                        *(pIFaceMAC+2),*(pIFaceMAC+3),
                                        *(pIFaceMAC+4),*(pIFaceMAC+5));

        /* Issue warning if different from specified value */
        if (memcmp( pPortMAC, pIFaceMAC, IFHWADDRLEN ) != 0)
        {
            if (pLCSPORT->fLocalMAC)
            {
                // "CTC: lcs device %s not using mac %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
                WRMSG( HHC00943, "W", pLCSPORT->szNetIfName, *(pPortMAC+0),*(pPortMAC+1),
                                                *(pPortMAC+2),*(pPortMAC+3),
                                                *(pPortMAC+4),*(pPortMAC+5));
            }

            memcpy( pPortMAC, pIFaceMAC, IFHWADDRLEN );

            snprintf(pLCSPORT->szMACAddress, sizeof(pLCSPORT->szMACAddress),
                "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X", *(pPortMAC+0), *(pPortMAC+1),
                *(pPortMAC+2), *(pPortMAC+3), *(pPortMAC+4), *(pPortMAC+5));
        }
    }

    INIT_REPLY_FRAME( pLCSLSSFRM, iReplyLen, pCmdFrame, iCmdLen );

    /* Respond with a different MAC address for the LCS side */
    /* unless the TAP mechanism is designed as such          */
    /* cf : hostopts.h for an explanation                    */
    iReplyLen = sizeof(Reply);
    pLCSLSSFRM->bLCSCmdHdr.bLCSHdr.bSlot = pLCSDEV->bPort;
    pLCSLSSFRM->bLCSCmdHdr.bInitiator = LCS_INITIATOR_SNA;
    STORE_HW( pLCSLSSFRM->bLCSCmdHdr.hwReturnCode, (S16) rc );
//  pLCSLSSFRM->bLCSCmdHdr.bLanType = LCS_FRMTYP_SNA;
    pLCSLSSFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;
    pLCSLSSFRM->bUnknown1 = 0x01;  /* Count? */
    pLCSLSSFRM->bUnknown2 = 0x04;  /* This byte is kept by VTAM. SAP? Probably not. 0x04 works, 0x08 doesn't. */
    pLCSLSSFRM->bUnknown3 = 0x00;  /* This byte is kept by VTAM. */
    pLCSLSSFRM->bUnknown7 = 0x06;  /* MAC length? */
    memcpy( pLCSLSSFRM->MAC_Address, pIFaceMAC, IFHWADDRLEN );
#if !defined( OPTION_TUNTAP_LCS_SAME_ADDR )
    pLCSLSSFRM->MAC_Address[5]++;
#endif

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSLSSFRM, iReplyLen, bBafflePres );
}

// ====================================================================
//                       LCS_DefaultCmd_SNA
// ====================================================================

static void  LCS_DefaultCmd_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres )
{
    LCSSTDFRM   Reply;
    int         iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    PLCSSTDFRM  pLCSSTDFRM = (PLCSSTDFRM)&Reply;

    INIT_REPLY_FRAME( pLCSSTDFRM, iReplyLen, pCmdFrame, iCmdLen );

    pLCSSTDFRM->bLCSCmdHdr.bLCSHdr.bSlot = pLCSDEV->bPort;
    pLCSSTDFRM->bLCSCmdHdr.bInitiator    = LCS_INITIATOR_SNA;
//  pLCSSTDFRM->bLCSCmdHdr.bLanType      = LCS_FRMTYP_SNA;
    pLCSSTDFRM->bLCSCmdHdr.bRelAdapterNo = pLCSDEV->bPort;

    ENQUEUE_REPLY_FRAME( pLCSDEV, pLCSSTDFRM, iReplyLen, bBafflePres );
}

// ====================================================================
//                         LCS_Baffle_SNA
// ====================================================================

// HHC00979D LCS: data: +0000< 00160000 00000000 00140400 000C0C99  ................ ...............r
// HHC00979D LCS: data: +0010< 0003C000 00000000 01000000 0000      ..............   ..{...........

static void  LCS_Baffle_SNA( PLCSDEV pLCSDEV, PLCSCMDHDR pCmdFrame, int iCmdLen, BYTE bBafflePres )
{

    char       Reply[128];
    int        iReplyLen = sizeof(Reply);  /* Used and changed by INIT_REPLY_FRAME */
    char*      pReply = (char*)&Reply;


    memset( pReply, 0, iReplyLen );
    memcpy( pReply, pCmdFrame, iCmdLen );
    iReplyLen = iCmdLen;

    LCS_EnqueueReplyFrame( pLCSDEV, (PLCSCMDHDR)pReply, iReplyLen, bBafflePres );
//      LCS_EnqueueBaffleFrame( pLCSDEV, (PLCSCMDHDR)pReply, iReplyLen, bBafflePres );
}

// ====================================================================
//                       LCS_EnqueueReplyFrame
// ====================================================================
//
// Copy a pre-built LCS Command Frame reply frame into the
// next available frame slot. Keep trying if buffer is full.
// The LCS device data lock must NOT be held when called!
//
// --------------------------------------------------------------------

static void LCS_EnqueueReplyFrame( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres )
{
    PLCSPORT  pLCSPORT;
    DEVBLK*   pDEVBLK;

    BYTE      bPort;
    time_t    t1, t2;


    bPort = pLCSDEV->bPort;
    pLCSPORT = &pLCSDEV->pLCSBLK->Port[ bPort ];
    pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

    // Trace command reply frame about to be enqueued...
    if (pLCSDEV->pLCSBLK->fDebug)
    {
        // HHC00923 "%1d:%04X CTC: lcs command reply enqueue"
        WRMSG( HHC00923, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );
        net_data_trace( pDEVBLK, (BYTE*)pReply, iSize, '>', 'D', "reply", 0 );
    }

    PTT_DEBUG( "ENQ RepFrame ENTRY", pReply->bCmdCode, pDEVBLK->devnum, bPort );

    time( &t1 );

    PTT_TIMING( "b4 repNQ", 0, iSize, 0 );

    // While port open, not close in progress, and frame buffer full...

    while (1
        &&  pLCSPORT->fd != -1
        && !pLCSPORT->fCloseInProgress
        && LCS_DoEnqueueReplyFrame( pLCSDEV, pReply, iSize, bBafflePres ) < 0
    )
    {
        if (pLCSDEV->pLCSBLK->fDebug)
        {
            // Limit message rate to only once every few seconds...

            time( &t2 );

            if ((t2 - t1) >= 3)     // (only once every 3 seconds)
            {
                union converter { struct { unsigned char a, b, c, d; } b; U32 i; } c;
                char  str[40];

                t1 = t2;

                c.i = ntohl( pLCSDEV->lIPAddress );
                MSGBUF( str, "%8.08X %d.%d.%d.%d", c.i, c.b.d, c.b.c, c.b.b, c.b.a );

                // "CTC: lcs device port %2.2X: STILL trying to enqueue REPLY frame to device %4.4X %s"
                WRMSG( HHC00978, "D", bPort, pLCSDEV->sAddr, str );
            }
        }
        PTT_TIMING( "*repNQ wait", 0, iSize, 0 );

        // Wait for LCS_Read to empty the buffer...

        ASSERT( ENOBUFS == errno );
        usleep( CTC_DELAY_USECS );
    }
    PTT_TIMING( "af repNQ", 0, iSize, 0 );
    PTT_DEBUG( "ENQ RepFrame EXIT ", pReply->bCmdCode, pDEVBLK->devnum, bPort );
}

// ====================================================================
//                       LCS_DoEnqueueReplyFrame
// ====================================================================
//
// Copy a pre-built LCS Command Frame reply frame of iSize bytes
// to the next available frame slot. Returns 0 on success, -1 and
// errno set to ENOBUFS on failure (no room (yet) in o/p buffer).
// The LCS device data lock must NOT be held when called!
//
// --------------------------------------------------------------------

static int  LCS_DoEnqueueReplyFrame( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres )
{
    PLCSCMDHDR  pReplyCmdFrame;
    DEVBLK*     pDEVBLK;
    BYTE*       pBaffle;
    BYTE        bPort = pLCSDEV->bPort;

    pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

    PTT_DEBUG(       "GET  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    obtain_lock( &pLCSDEV->DevDataLock );
    PTT_DEBUG(       "GOT  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    {
        // Ensure we dont overflow the buffer
 /* FixMe! Need to account for bBafflePres here! */
        if ((pLCSDEV->iFrameOffset +            // Current buffer Offset
              iSize +                           // Size of reply frame
              sizeof(pReply->bLCSHdr.hwOffset)) // Size of Frame terminator
            > pLCSDEV->iMaxFrameBufferSize)     // Size of Frame buffer
        {
            PTT_DEBUG( "*DoENQRep ENOBUFS ", 000, pDEVBLK->devnum, bPort );
            PTT_DEBUG( "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
            release_lock( &pLCSDEV->DevDataLock );
            errno = ENOBUFS;                    // No buffer space available
            return -1;                          // (-1==failure)
        }

        if ( !pLCSDEV->iFrameOffset && bBafflePres )
        {
            pBaffle = pLCSDEV->bFrameBuffer;
            memset( pBaffle, 0, SIZEOF_BAFFLE );
            pLCSDEV->iFrameOffset += SIZEOF_BAFFLE;
            pLCSDEV->fPendingBaffle = 1;
        }

        // Point to next available LCS Frame slot in our buffer...
        pReplyCmdFrame = (PLCSCMDHDR)( pLCSDEV->bFrameBuffer +
                                       pLCSDEV->iFrameOffset );

        // Copy the reply frame into the frame buffer slot...
        memcpy( pReplyCmdFrame, pReply, iSize );

        // Increment buffer offset to NEXT next-available-slot...
        pLCSDEV->iFrameOffset += (U16) iSize;

        // Store offset of next frame
        if ( pLCSDEV->fPendingBaffle )
            STORE_HW( pReplyCmdFrame->bLCSHdr.hwOffset, ( pLCSDEV->iFrameOffset - SIZEOF_BAFFLE) );
        else
            STORE_HW( pReplyCmdFrame->bLCSHdr.hwOffset, pLCSDEV->iFrameOffset );

        // Mark reply pending
        PTT_DEBUG( "SET  ReplyPending ", 1, pDEVBLK->devnum, bPort );
        pLCSDEV->fReplyPending = 1;
    }
    PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    release_lock( &pLCSDEV->DevDataLock );

    // (wake up "LCS_Read" function)
    PTT_DEBUG(       "GET  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    obtain_lock( &pLCSDEV->DevEventLock );
    PTT_DEBUG(       "GOT  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    {
        PTT_DEBUG(            "SIG  DevEvent     ", 000, pDEVBLK->devnum, bPort );
        signal_condition( &pLCSDEV->DevEvent );
    }
    PTT_DEBUG(        "REL  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    release_lock( &pLCSDEV->DevEventLock );

    return 0;   // success
}

//  // ====================================================================
//  //                       LCS_EnqueueBaffleFrame
//  // ====================================================================
//  //
//  // --------------------------------------------------------------------
//
//  static void LCS_EnqueueBaffleFrame( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres )
//  {
//      PLCSPORT  pLCSPORT;
//      DEVBLK*   pDEVBLK;
//
//      BYTE      bPort;
//      time_t    t1, t2;
//
//
//      bPort = pLCSDEV->bPort;
//      pLCSPORT = &pLCSDEV->pLCSBLK->Port[ bPort ];
//      pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];
//
//      // Trace command reply frame about to be enqueued...
//      if (pLCSDEV->pLCSBLK->fDebug)
//      {
//          // HHC00923 "%1d:%04X CTC: lcs command reply enqueue"
//          WRMSG( HHC00923, "D", SSID_TO_LCSS( pDEVBLK->ssid ), pDEVBLK->devnum );
//          net_data_trace( pDEVBLK, (BYTE*)pReply, iSize, '>', 'D', "reply", 0 );
//      }
//
//      PTT_DEBUG( "ENQ BafFrame ENTRY", 0, pDEVBLK->devnum, 0 );
//
//      time( &t1 );
//
//      PTT_TIMING( "b4 BafNQ", 0, iSize, 0 );
//
//      // While port open, not close in progress, and frame buffer full...
//
//      while (1
//          &&  pLCSPORT->fd != -1
//          && !pLCSPORT->fCloseInProgress
//          && LCS_DoEnqueueBaffleFrame( pLCSDEV, pReply, iSize, bBafflePres ) < 0
//      )
//      {
//          if (pLCSDEV->pLCSBLK->fDebug)
//          {
//              // Limit message rate to only once every few seconds...
//
//              time( &t2 );
//
//              if ((t2 - t1) >= 3)     // (only once every 3 seconds)
//              {
//
//                  t1 = t2;
//
//                  // "CTC: lcs device port %2.2X: STILL trying to enqueue REPLY frame to device %4.4X SNA"
//                  WRMSG( HHC00978, "D", bPort, pLCSDEV->sAddr, "SNA" );
//              }
//          }
//          PTT_TIMING( "*BafNQ wait", 0, iSize, 0 );
//
//          // Wait for LCS_Read to empty the buffer...
//
//          ASSERT( ENOBUFS == errno );
//          usleep( CTC_DELAY_USECS );
//      }
//      PTT_TIMING( "af BafNQ", 0, iSize, 0 );
//      PTT_DEBUG( "ENQ BafFrame EXIT ", 0, pDEVBLK->devnum, 0 );
//  }
//
//  // ====================================================================
//  //                       LCS_DoEnqueueBaffleFrame
//  // ====================================================================
//  //
//  // --------------------------------------------------------------------
//
//  static int  LCS_DoEnqueueBaffleFrame( PLCSDEV pLCSDEV, PLCSCMDHDR pReply, size_t iSize, BYTE bBafflePres )
//  {
//      PLCSCMDHDR  pBaffleFrame;
//      DEVBLK*     pDEVBLK;
//      BYTE*       pBaffle;
//      BYTE        bPort = pLCSDEV->bPort;
//
//      pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];
//
//      PTT_DEBUG(       "GET  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
//      obtain_lock( &pLCSDEV->DevDataLock );
//      PTT_DEBUG(       "GOT  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
//      {
//          // Ensure we dont overflow the buffer
//   /* FixMe! Need to account for bBafflePres here! */
//          if ((pLCSDEV->iFrameOffset +            // Current buffer Offset
//                iSize +                           // Size of reply frame
//                sizeof(pReply->bLCSHdr.hwOffset)) // Size of Frame terminator
//              > pLCSDEV->iMaxFrameBufferSize)     // Size of Frame buffer
//          {
//              PTT_DEBUG( "*DoENQBaf ENOBUFS ", 000, pDEVBLK->devnum, bPort );
//              PTT_DEBUG( "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
//              release_lock( &pLCSDEV->DevDataLock );
//              errno = ENOBUFS;                    // No buffer space available
//              return -1;                          // (-1==failure)
//          }
//
//          if ( !pLCSDEV->iFrameOffset && bBafflePres )
//          {
//              pBaffle = pLCSDEV->bFrameBuffer;
//              memset( pBaffle, 0, SIZEOF_BAFFLE );
//              pLCSDEV->iFrameOffset += SIZEOF_BAFFLE;
//              pLCSDEV->fPendingBaffle = 1;
//          }
//
//          // Point to next available LCS Frame slot in our buffer...
//          pBaffleFrame = (PLCSCMDHDR)( pLCSDEV->bFrameBuffer +
//                                        pLCSDEV->iFrameOffset );
//
//          // Copy the reply frame into the frame buffer slot...
//          memcpy( pBaffleFrame, pReply, iSize );
//
//          // Increment buffer offset to NEXT next-available-slot...
//          pLCSDEV->iFrameOffset += (U16) iSize;
//
//          // Store offset of next frame
//          if ( pLCSDEV->fPendingBaffle )
//              STORE_HW( pBaffleFrame->bLCSHdr.hwOffset, ( pLCSDEV->iFrameOffset - SIZEOF_BAFFLE) );
//          else
//              STORE_HW( pBaffleFrame->bLCSHdr.hwOffset, pLCSDEV->iFrameOffset );
//
//          // Mark data pending
//          PTT_DEBUG( "SET  DataPending!!", 1, pDEVBLK->devnum, bPort );
//          pLCSDEV->fDataPending = 1;
//      }
//      PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
//      release_lock( &pLCSDEV->DevDataLock );
//
//      // (wake up "LCS_Read" function)
//      PTT_DEBUG(       "GET  DevEventLock ", 000, pDEVBLK->devnum, bPort );
//      obtain_lock( &pLCSDEV->DevEventLock );
//      PTT_DEBUG(       "GOT  DevEventLock ", 000, pDEVBLK->devnum, bPort );
//      {
//          PTT_DEBUG(            "SIG  DevEvent     ", 000, pDEVBLK->devnum, bPort );
//          signal_condition( &pLCSDEV->DevEvent );
//      }
//      PTT_DEBUG(        "REL  DevEventLock ", 000, pDEVBLK->devnum, bPort );
//      release_lock( &pLCSDEV->DevEventLock );
//
//      return 0;   // success
//  }

// ====================================================================
//                       LCS_PortThread
// ====================================================================
// This is the thread that does the actual read from the tap device.
// It waits for packets to arrive on the device and then enqueues them
// to the device input queue to be read by the LCS_Read() function the
// next time the guest issues a read CCW.
// --------------------------------------------------------------------

static void*  LCS_PortThread( void* arg)
{
    DEVBLK*     pDEVBLK;
    PLCSPORT    pLCSPORT = (PLCSPORT) arg;
    PLCSDEV     pLCSDev;
    PLCSDEV     pPrimaryLCSDEV;
    PLCSDEV     pSecondaryLCSDEV;
    PLCSDEV     pMatchingLCSDEV;
    PLCSRTE     pLCSRTE;
    PETHFRM     pEthFrame;
    PIP4FRM     pIPFrame   = NULL;
    PARPFRM     pARPFrame  = NULL;
    int         iLength;
    U16         hwEthernetType;
    U32         lIPAddress;             // (network byte order)
    BYTE*       pMAC;
    BYTE        szBuff[2048];
    char        bReported = 0;
    char        bStartReported = 0;
    char        cPktType[8];

    pDEVBLK = pLCSPORT->pLCSBLK->pDevices->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

    pLCSPORT->pid = getpid();

    PTT_DEBUG(            "PORTHRD: ENTRY    ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    for (;;)
    {
        PTT_DEBUG(        "GET  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
        obtain_lock( &pLCSPORT->PortEventLock );
        PTT_DEBUG(        "GOT  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
        {
            // Don't read unless/until port is enabled...

            if (!pLCSPORT->fPortStarted)
            {
                if (pLCSPORT->pLCSBLK->fDebug)
                {
                    if (bStartReported)
                        // "CTC: lcs device port %2.2X: read thread: port stopped"
                        WRMSG( HHC00969, "D", pLCSPORT->bPort );

                    // "CTC: lcs device port %2.2X: read thread: waiting for start event"
                    WRMSG( HHC00967, "D", pLCSPORT->bPort );
                }
                bStartReported = 0;
            }

            while (1)
            {
                PTT_DEBUG( "PORTHRD if started", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );
                if (0
                    || (pLCSPORT->fd < 0)
                    || pLCSPORT->fCloseInProgress
                    || pLCSPORT->fPortStarted
                )
                {
                    if ((pLCSPORT->fd < 0) || pLCSPORT->fCloseInProgress)
                        PTT_DEBUG( "PORTHRD is closing", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );
                    else
                        PTT_DEBUG( "PORTHRD is started", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );
                    break;
                }

                PTT_DEBUG( "WAIT PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
                timed_wait_condition_relative_usecs
                (
                    &pLCSPORT->PortEvent,       // ptr to condition to wait on
                    &pLCSPORT->PortEventLock,   // ptr to controlling lock (must be held!)
                    250*1000,                   // max #of microseconds to wait
                    NULL                        // [OPTIONAL] ptr to tod value (may be NULL)
                );
                PTT_DEBUG( "WOKE PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );

            } // end while (1)

            if (!bStartReported)
            {
                bStartReported = 1;
                if (pLCSPORT->pLCSBLK->fDebug)
                    // "CTC: lcs device port %2.2X: read thread: port started"
                    WRMSG( HHC00968, "D", pLCSPORT->bPort );
            }
        }
        PTT_DEBUG(         "REL  PortEventLock", 000, pDEVBLK->devnum, pLCSPORT->bPort );
        release_lock( &pLCSPORT->PortEventLock );

        // Exit when told...

        if ( pLCSPORT->fd < 0 || pLCSPORT->fCloseInProgress )
            break;

        // Read an IP packet from the TAP device
        PTT_TIMING( "b4 tt read", 0, 0, 0 );
        iLength = read_tuntap( pLCSPORT->fd, szBuff, sizeof( szBuff ), DEF_NET_READ_TIMEOUT_SECS );
        PTT_TIMING( "af tt read", 0, 0, iLength );

        if (iLength == 0)      // (probably EINTR; ignore)
            continue;

        // Check for other error condition
        if (iLength < 0)
        {
            if (pLCSPORT->fd < 0 || pLCSPORT->fCloseInProgress)
                break;
            // "CTC: lcs device read error from port %2.2X: %s"
            WRMSG( HHC00944, "E", pLCSPORT->bPort, strerror( errno ) );
            break;
        }

        // Point to ethernet frame and determine frame type
        pEthFrame = (PETHFRM)szBuff;
        FETCH_HW( hwEthernetType, pEthFrame->hwEthernetType );

        if (pLCSPORT->pLCSBLK->fDebug)
        {
            SET_CPKTTYPE( hwEthernetType, cPktType );

            // "%1d:%04X %s: port %2.2X: Receive frame of size %d bytes (with %s packet) from device %s"
            WRMSG( HHC00984, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                                  pLCSPORT->bPort, iLength, cPktType, pLCSPORT->szNetIfName );
            net_data_trace( pDEVBLK, szBuff, iLength, '>', 'D', "eth frame", 0 );
            bReported = 0;
        }

        // Perform multicast assist if necessary: discard any multicast
        // packets the guest didn't specifically register. We only need
        // to do this if tuntap said that it was unable to do so for us.

        if (1
            && pLCSPORT->fDoMCastAssist                                     // do mcast filtering ourself?
            && pLCSPORT->nMCastCount                                        // we have MACs in our table?
            && memcmp( pEthFrame->bDestMAC, mcast3, sizeof( mcast3 )) == 0  // this is a multicast frame?
            && IsMACTab( pLCSPORT->MCastTab, pEthFrame->bDestMAC ) < 0      // its MAC not in our table?
        )
        {
            if (pLCSPORT->pLCSBLK->fDebug)
                // "CTC: lcs device port %2.2X: MCAST not in table, discarding frame"
                WRMSG( HHC00945, "D", pLCSPORT->bPort );
            continue;
        }

        // Housekeeping
        pPrimaryLCSDEV   = NULL;
        pSecondaryLCSDEV = NULL;
        pMatchingLCSDEV  = NULL;

        // Attempt to find the device that this frame belongs to
        for (pLCSDev = pLCSPORT->pLCSBLK->pDevices; pLCSDev; pLCSDev = pLCSDev->pNext)
        {
            // Only process devices that are on this port
            if (pLCSDev->bPort == pLCSPORT->bPort)
            {
                // To quote Wikipedia "The EtherType field is two octets long
                // and it can be used for two different purposes. Values of 1500
                // and below mean that it is used to indicate the size of the
                // payload in octets, while values of 1536 and above indicate
                // that it is used as an EtherType, to indicate which protocol
                // is encapsulated in the payload of the frame."
                if (hwEthernetType >= ETH_TYPE)  // i.e. >= 1536
                {
                    // EtherType indicates which protocol is encapsulated in the payload.
                    if (hwEthernetType == ETH_TYPE_IP)
                    {
                        pIPFrame   = (PIP4FRM)pEthFrame->bData;
                        lIPAddress = pIPFrame->lDstIP;  // (network byte order)

                        if (pLCSPORT->pLCSBLK->fDebug && !bReported)
                        {
                            union converter { struct { unsigned char a, b, c, d; } b; U32 i; } c;
                            char  str[40];

                            c.i = ntohl(lIPAddress);
                            MSGBUF( str, "%8.08X %d.%d.%d.%d", c.i, c.b.d, c.b.c, c.b.b, c.b.a );

                            // "CTC: lcs device port %2.2X: IPv4 frame received for %s"
                            WRMSG( HHC00946, "D", pLCSPORT->bPort, str );
                            bReported = 1;
                        }

                        // If this is an exact match use it
                        // otherwise look for primary and secondary
                        // default devices
                        if (pLCSDev->lIPAddress == lIPAddress)
                        {
                            pMatchingLCSDEV = pLCSDev;
                            break;
                        }
                        else if (pLCSDev->bType == LCSDEV_TYPE_PRIMARY)
                            pPrimaryLCSDEV = pLCSDev;
                        else if (pLCSDev->bType == LCSDEV_TYPE_SECONDARY)
                            pSecondaryLCSDEV = pLCSDev;
                    }
                    else if (hwEthernetType == ETH_TYPE_ARP)
                    {
                        pARPFrame  = (PARPFRM)pEthFrame->bData;
                        lIPAddress = pARPFrame->lTargIPAddr; // (network byte order)

                        if (pLCSPORT->pLCSBLK->fDebug && !bReported)
                        {
                            union converter { struct { unsigned char a, b, c, d; } b; U32 i; } c;
                            char  str[40];

                            c.i = ntohl(lIPAddress);
                            MSGBUF( str, "%8.08X %d.%d.%d.%d", c.i, c.b.d, c.b.c, c.b.b, c.b.a );

                            // "CTC: lcs device port %2.2X: ARP frame received for %s"
                            WRMSG( HHC00947, "D", pLCSPORT->bPort, str );
                            bReported = 1;
                        }

                        // If this is an exact match use it
                        // otherwise look for primary and secondary
                        // default devices
                        if (pLCSDev->lIPAddress == lIPAddress)
                        {
                            pMatchingLCSDEV = pLCSDev;
                            break;
                        }
                        else if (pLCSDev->bType == LCSDEV_TYPE_PRIMARY)
                            pPrimaryLCSDEV = pLCSDev;
                        else if (pLCSDev->bType == LCSDEV_TYPE_SECONDARY)
                            pSecondaryLCSDEV = pLCSDev;
                    }
                    else if (hwEthernetType == ETH_TYPE_RARP)
                    {
                        pARPFrame  = (PARPFRM)pEthFrame->bData;
                        pMAC = pARPFrame->bTargEthAddr;

                        if (pLCSPORT->pLCSBLK->fDebug && !bReported)
                        {
                            // "CTC: lcs device port %2.2X: RARP frame received for %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
                            WRMSG( HHC00948, "D" ,pLCSPORT->bPort ,*(pMAC+0) ,*(pMAC+1) ,*(pMAC+2) ,*(pMAC+3) ,*(pMAC+4) ,*(pMAC+5) );
                            bReported = 1;
                        }

                        // If this is an exact match use it
                        // otherwise look for primary and secondary
                        // default devices
                        if (memcmp( pMAC, pLCSPORT->MAC_Address, IFHWADDRLEN ) == 0)
                        {
                            pMatchingLCSDEV = pLCSDev;
                            break;
                        }
                        else if (pLCSDev->bType == LCSDEV_TYPE_PRIMARY)
                            pPrimaryLCSDEV = pLCSDev;
                        else if (pLCSDev->bType == LCSDEV_TYPE_SECONDARY)
                            pSecondaryLCSDEV = pLCSDev;
                    }
                    else if (hwEthernetType == ETH_TYPE_SNA)
                    {
                        pMAC = pEthFrame->bDestMAC;

                        if (pLCSPORT->pLCSBLK->fDebug && !bReported)
                        {
                            // "CTC: lcs device port %2.2X: SNA frame received for %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
                            WRMSG( HHC00949, "D" ,pLCSPORT->bPort ,*(pMAC+0) ,*(pMAC+1) ,*(pMAC+2) ,*(pMAC+3) ,*(pMAC+4) ,*(pMAC+5) );
                            bReported = 1;
                        }

                        if (pLCSDev->bMode == LCSDEV_MODE_SNA)
                        {
                            pMatchingLCSDEV = pLCSDev;
                            break;
                        }
                        else if (pLCSDev->bType == LCSDEV_TYPE_PRIMARY)
                            pPrimaryLCSDEV = pLCSDev;
                        else if (pLCSDev->bType == LCSDEV_TYPE_SECONDARY)
                            pSecondaryLCSDEV = pLCSDev;
                    }
                }
                else  //  hwEthernetType < ETH_TYPE  i.e. < 1536
                {
                    // EtherType indicates the size of the payload, which should have
                    // a value of 46 to 1500 inclusive. However, frames have been seen
                    // with EtherType equal to three (0x0003), which was presumably the
                    // size of the following the 802.2 LLC fields, rather than the size
                    // of the entire payload.
                    // The LLC fields are a 1-byte Destination Service Access Point
                    // (DSAP), a 1-byte Source Service Access Point (SSAP), and a 1-
                    // or 2-byte Control Field.
                    // We will assume this is an 802.3 frame containing SNA traffic.
                    pMAC = pEthFrame->bDestMAC;

                    if (pLCSPORT->pLCSBLK->fDebug && !bReported)
                    {
                        // "CTC: lcs device port %2.2X: SNA frame received for %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X"
                        WRMSG( HHC00949, "D" ,pLCSPORT->bPort ,*(pMAC+0) ,*(pMAC+1) ,*(pMAC+2) ,*(pMAC+3) ,*(pMAC+4) ,*(pMAC+5) );
                        bReported = 1;
                    }

                    if (pLCSDev->bMode == LCSDEV_MODE_SNA)
                    {
                        pMatchingLCSDEV = pLCSDev;
                        break;
                    }
                    else if (pLCSDev->bType == LCSDEV_TYPE_PRIMARY)
                        pPrimaryLCSDEV = pLCSDev;
                    else if (pLCSDev->bType == LCSDEV_TYPE_SECONDARY)
                        pSecondaryLCSDEV = pLCSDev;
                }
            }
        }

        // If the matching device is not started
        // nullify the pointer and pass frame to one
        // of the defaults if present
        if (pMatchingLCSDEV && !pMatchingLCSDEV->fDevStarted)
            pMatchingLCSDEV = NULL;

        // Match not found, check for default devices
        // If one is defined and started, use it
        if (!pMatchingLCSDEV)
        {
            if (pPrimaryLCSDEV && pPrimaryLCSDEV->fDevStarted)
            {
                pMatchingLCSDEV = pPrimaryLCSDEV;

#ifndef LCS_NO_950_952 // (HHC00950 and HHC00952 are rarely interesting)
                if (pLCSPORT->pLCSBLK->fDebug)
                    // "CTC: lcs device port %2.2X: no match found, selecting %s %4.4X"
                    WRMSG( HHC00950, "D", pLCSPORT->bPort, "primary", pMatchingLCSDEV->sAddr );
#endif // LCS_NO_950_952
            }
            else if (pSecondaryLCSDEV && pSecondaryLCSDEV->fDevStarted)
            {
                pMatchingLCSDEV = pSecondaryLCSDEV;

#ifndef LCS_NO_950_952 // (HHC00950 and HHC00952 are rarely interesting)
                if (pLCSPORT->pLCSBLK->fDebug)
                    // "CTC: lcs device port %2.2X: no match found, selecting %s %4.4X"
                    WRMSG( HHC00950, "D", pLCSPORT->bPort, "secondary", pMatchingLCSDEV->sAddr );
#endif // LCS_NO_950_952
            }
        }

        // No match found, discard frame
        if (!pMatchingLCSDEV)
        {
            if (pLCSPORT->pLCSBLK->fDebug)
                // "CTC: lcs device port %2.2X: no match found, discarding frame"
                WRMSG( HHC00951, "D", pLCSPORT->bPort );

            continue;
        }

#ifndef LCS_NO_950_952 // (HHC00950 and HHC00952 are rarely interesting)
        if (pLCSPORT->pLCSBLK->fDebug)
        {
            union converter { struct { unsigned char a, b, c, d; } b; U32 i; } c;
            char  str[40];

            c.i = ntohl(pMatchingLCSDEV->lIPAddress);
            MSGBUF( str, "%8.08X %d.%d.%d.%d", c.i, c.b.d, c.b.c, c.b.b, c.b.a );

            // "CTC: lcs device port %2.2X: enqueing frame to device %4.4X %s"
            WRMSG( HHC00952, "D", pLCSPORT->bPort, pMatchingLCSDEV->sAddr, str );
        }
#endif // LCS_NO_950_952

        // Match was found. Enqueue frame on buffer.

        LCS_EnqueueEthFrame( pMatchingLCSDEV, pLCSPORT->bPort, szBuff, iLength );

    } // end for (;;)

    PTT_DEBUG( "PORTHRD Closing...", pLCSPORT->fPortStarted, pDEVBLK->devnum, pLCSPORT->bPort );

    // We must do the close since we were the one doing the i/o...

    VERIFY( pLCSPORT->fd == -1 || TUNTAP_Close( pLCSPORT->fd ) == 0 );

    // Housekeeping - Cleanup Port Block

    memset( pLCSPORT->MAC_Address,  0, sizeof( MAC ) );
    memset( pLCSPORT->szNetIfName, 0, IFNAMSIZ );
    memset( pLCSPORT->szMACAddress, 0, 32 );

    for (pLCSRTE = pLCSPORT->pRoutes; pLCSRTE; pLCSRTE = pLCSPORT->pRoutes)
    {
        pLCSPORT->pRoutes = pLCSRTE->pNext;
        free( pLCSRTE );
        pLCSRTE = NULL;
    }

    pLCSPORT->sIPAssistsSupported = 0;  // (reset)
    pLCSPORT->sIPAssistsEnabled   = 0;  // (reset)
    pLCSPORT->fDoCkSumOffload     = 0;  // (reset)
    pLCSPORT->fDoMCastAssist      = 0;  // (reset)

    pLCSPORT->fUsed        = 0;
    pLCSPORT->fLocalMAC    = 0;
    pLCSPORT->fPortCreated = 0;
    PTT_DEBUG( "PORTHRD started=NO", 000, pDEVBLK->devnum, pLCSPORT->bPort );
    pLCSPORT->fPortStarted = 0;
    pLCSPORT->fRouteAdded  = 0;
    pLCSPORT->fd           = -1;

    PTT_DEBUG( "PORTHRD: EXIT     ", 000, pDEVBLK->devnum, pLCSPORT->bPort );

    return NULL;

} // end of LCS_PortThread

// ====================================================================
//                       LCS_AttnThread
// ====================================================================
//
// This is the thread that generates Attention interrupts to the guest.
// Only used when there are one or more SNA devices.
//
// --------------------------------------------------------------------

static void*  LCS_AttnThread( void* arg)
{

    PLCSBLK     pLCSBLK;
    PLCSATTN    pLCSATTN;
    PLCSATTN    pLCSATTNprev, pLCSATTNcurr, pLCSATTNnext;
    PLCSDEV     pLCSDEV;
    DEVBLK*     pDEVBLK;
    /* --------------------------------------------------------------------- */
    int         interval;              /* interval between attempts  FixMe! Configurable? */
    int         dev_attn_rc;           /* device_attention RC    */
//  int         attn_can;              /* = 1 : Atttention Cancelled */
    int         busy_waits;            /* Number of times waited for */
                                       /* a Busy condition to end    */
    /* --------------------------------------------------------------------- */


//  {                                                                          /* FixMe! Remove! */
//      char    tmp[256];                                                      /* FixMe! Remove! */
//      snprintf( (char*)tmp, 256, "LCS_AttnThread activated" );               /* FixMe! Remove! */
//      WRMSG(HHC03984, "D", tmp );                                            /* FixMe! Remove! */
//  }                                                                          /* FixMe! Remove! */

    PTT_DEBUG( "ATTNTHRD: ENTRY", 000, 000, 000 );

    /* Point to the LCSBLK and obtain the pid of this thread. */
    pLCSBLK = (PLCSBLK) arg;
    pLCSBLK->AttnPid = getpid();

    for ( ; ; )
    {

        /* */
        PTT_DEBUG( "GET  AttnEventLock", 000, 000, 000 );
        obtain_lock( &pLCSBLK->AttnEventLock );
        PTT_DEBUG( "GOT  AttnEventLock", 000, 000, 000 );
        {
            for( ; ; )
            {
                if ( pLCSBLK->fCloseInProgress )
                {
                    PTT_DEBUG( "ATTNTHRD Closing...", 000, 000, 000 );
                    break;
                }

                if ( pLCSBLK->pAttns )
                {
                    PTT_DEBUG( "ATTNTHRD Attn...", 000, 000, 000 );
                    break;
                }

                PTT_DEBUG( "WAIT AttnEventLock", 000, 000, 000 );
                timed_wait_condition_relative_usecs
                (
                    &pLCSBLK->AttnEvent,         // ptr to condition to wait on
                    &pLCSBLK->AttnEventLock,     // ptr to controlling lock (must be held!)
                    3*1000*1000,                 // max #of microseconds to wait, i.e. 3 seconds
                    NULL                         // [OPTIONAL] ptr to tod value (may be NULL)
                );
                PTT_DEBUG( "WOKE AttnEventLock", 000, 000, 000 );
            }
        }
        PTT_DEBUG( "REL  AttnEventLock", 000, 000, 000 );
        release_lock( &pLCSBLK->AttnEventLock );

        /* Exit when told... */
        if ( pLCSBLK->fCloseInProgress )
        {
            PTT_DEBUG( "ATTNTHRD Closing...", 000, 000, 000 );
            break;
        }

        /* Remove the chain of LCSATTN blocks */
        PTT_DEBUG( "GET  AttnLock", 000, 000, 000 );
        obtain_lock( &pLCSBLK->AttnLock );
        PTT_DEBUG( "GOT  AttnLock", 000, 000, 000 );
        {
            pLCSATTN = pLCSBLK->pAttns;
            pLCSBLK->pAttns = NULL;
            if ( pLCSATTN )
            {
                PTT_DEBUG( "REM  Attn (All)", pLCSATTN, 000, 000 );
            }
            else
            {
                PTT_DEBUG( "REM  Attn (Non)", 000, 000, 000 );
            }
        }
        PTT_DEBUG( "REL  AttnLock", 000, 000, 000 );
        release_lock( &pLCSBLK->AttnLock );

        /* Reverse the chain of LCSATTN blocks */
        if ( pLCSATTN )
        {
            pLCSATTNprev = NULL;
            pLCSATTNcurr = pLCSATTN;
            while( pLCSATTNcurr )
            {
                pLCSATTNnext = pLCSATTNcurr->pNext;
                pLCSATTNcurr->pNext = pLCSATTNprev;
                pLCSATTNprev = pLCSATTNcurr;
                pLCSATTNcurr = pLCSATTNnext;
            }
            pLCSATTN = pLCSATTNprev;
        }

        /* Process the chain of LCSATTN blocks */
        while ( pLCSATTN )
        {
            /* Point to the next LCSATTN block in the chain, assuming there is one */
            pLCSATTNnext = pLCSATTN->pNext;

            /* Point to the LCSDEV and the read DEVBLK for the command */
            pLCSDEV = pLCSATTN->pDevice;
            pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

//          if (pLCSBLK->fDebug)                                                                            /* FixMe! Remove! */
//              net_data_trace( pDEVBLK, (BYTE*)pLCSATTN, sizeof( LCSATTN ), ' ', 'D', "LCSATTN out", 0 );  /* FixMe! Remove! */

            PTT_DEBUG( "PRC  Attn", pLCSATTN, pDEVBLK->devnum, 000 );

            /* --------------------------------------------------------------------- */
            interval = 50;
            dev_attn_rc = 0;       /* device_attention RC    */
//          attn_can = 0;          /* = 1 : Atttention Cancelled */
            busy_waits = 0;        /* Number of times waited for */
                                   /* a Busy condition to end    */

            for( ; ; )
            {

                // Wait an (increasingly) small amount of time.
                usleep(interval);

                // is there still something in our frame buffer?
                if (!pLCSDEV->fDataPending && !pLCSDEV->fReplyPending)
                {
                    break;
                }

                // Raise Attention
                dev_attn_rc = device_attention( pDEVBLK, CSW_ATTN );
                PTT_DEBUG( "Raise Attn   ", 000, pDEVBLK->devnum, dev_attn_rc );

    {                                                                                                        /* FixMe! Remove? */
        char    tmp[256];                                                                                    /* FixMe! Remove? */
        snprintf( (char*)tmp, 256, "device_attention rc=%d  %d  %d", dev_attn_rc, busy_waits, interval );    /* FixMe! Remove? */
        WRMSG(HHC03991, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname, tmp );          /* FixMe! Remove? */
    }                                                                                                        /* FixMe! Remove? */

                // ATTN RC=1 means a device busy status did
                // appear so that the signal did not work.
                // We will retry after some (increasingly)
                // small amount of time.
                if ( dev_attn_rc != 1 )
                {
                    break;
                }

                busy_waits++;

                if ( busy_waits >= 20 )
                {
                    break;
                }

                interval = interval * 2;

            }   // end for ( ; ; )

            /* --------------------------------------------------------------------- */

            /* Free the LCSATTN block that has just been processed */
            free (pLCSATTN);

            /* Point to the next LCSATTN block in the chain, assuming there is one */
            pLCSATTN = pLCSATTNnext;
        }  // end while (pLCSATTN)

    }  // end for ( ; ; )

    PTT_DEBUG( "ATTNTHRD: EXIT", 000, 000, 000 );

//  {                                                                          /* FixMe! Remove! */
//      char    tmp[256];                                                      /* FixMe! Remove! */
//      snprintf( (char*)tmp, 256, "LCS_AttnThread terminated" );              /* FixMe! Remove! */
//      WRMSG(HHC03984, "D", tmp );                                            /* FixMe! Remove! */
//  }                                                                          /* FixMe! Remove! */

    return NULL;
} // End of LCS_AttnThread

// ====================================================================
//                       LCS_EnqueueEthFrame
// ====================================================================
//
// Places the provided ethernet frame in the next available frame
// slot in the adapter buffer. If buffer is full, keep trying.
// The LCS device data lock must NOT be held when called!
//
// --------------------------------------------------------------------

static void LCS_EnqueueEthFrame( PLCSDEV pLCSDEV, BYTE bPort, BYTE* pData, size_t iSize )
{
    time_t    t1, t2;
    PLCSPORT  pLCSPORT;
    DEVBLK*   pDEVBLK;

    pLCSPORT = &pLCSDEV->pLCSBLK->Port[ bPort ];
    pDEVBLK  = pLCSPORT->pLCSBLK->pDevices->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

    PTT_DEBUG( "ENQ EthFrame ENTRY", 000, pDEVBLK->devnum, bPort );

    time( &t1 );

    PTT_TIMING( "b4 enqueue", 0, iSize, 0 );

    // While port open, not close in progress, and frame buffer full...

    while (1
        &&  pLCSPORT->fd != -1
        && !pLCSPORT->fCloseInProgress
        && LCS_DoEnqueueEthFrame( pLCSDEV, bPort, pData, iSize ) < 0
    )
    {
        if (EMSGSIZE == errno)
        {
            // "CTC: lcs device port %2.2X: packet frame too big, dropped"
            WRMSG( HHC00953, "W", bPort );
            PTT_TIMING( "*enq drop", 0, iSize, 0 );
            break;
        }

        if (pLCSDEV->pLCSBLK->fDebug)
        {
            // Limit message rate to only once every few seconds...

            time( &t2 );

            if ((t2 - t1) >= 3)     // (only once every 3 seconds)
            {
                union converter { struct { unsigned char a, b, c, d; } b; U32 i; } c;
                char  str[40];

                t1 = t2;

                c.i = ntohl( pLCSDEV->lIPAddress );
                MSGBUF( str, "%8.08X %d.%d.%d.%d", c.i, c.b.d, c.b.c, c.b.b, c.b.a );

                // "CTC: lcs device port %2.2X: STILL trying to enqueue frame to device %4.4X %s"
                WRMSG( HHC00965, "D", bPort, pLCSDEV->sAddr, str );
            }
        }
        PTT_TIMING( "*enq wait", 0, iSize, 0 );

        // Wait for LCS_Read to empty the buffer...

        ASSERT( ENOBUFS == errno );
        usleep( CTC_DELAY_USECS );
    }
    PTT_TIMING( "af enqueue", 0, iSize, 0 );
    PTT_DEBUG( "ENQ EthFrame EXIT ", 000, pDEVBLK->devnum, bPort );
}

// ====================================================================
//                       LCS_DoEnqueueEthFrame
// ====================================================================
//
// Places the provided ethernet frame in the next available frame
// slot in the adapter buffer.
//
//   pData       points the the Ethernet packet just received
//   iSize       is the size of the Ethernet packet
//
// Returns:
//
//  0 == Success
// -1 == Failure; errno = ENOBUFS:  No buffer space available
//                        EMSGSIZE: Message too long
//
// The LCS device data lock must NOT be held when called!
//
// --------------------------------------------------------------------

static int  LCS_DoEnqueueEthFrame( PLCSDEV pLCSDEV, BYTE bPort, BYTE* pData, size_t iSize )
{
    PLCSETHFRM  pLCSEthFrame;
    DEVBLK*     pDEVBLK;

    pDEVBLK = pLCSDEV->pDEVBLK[ LCSDEV_READ_SUBCHANN ];

    // Will frame NEVER fit into buffer??
    if (iSize > MAX_LCS_ETH_FRAME_SIZE( pLCSDEV ) || iSize > 9000)
    {
        PTT_DEBUG( "*DoENQEth EMSGSIZE", 000, pDEVBLK->devnum, bPort );
        errno = EMSGSIZE;   // Message too long
        return -1;          // (-1==failure)
    }

    PTT_DEBUG(       "GET  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    obtain_lock( &pLCSDEV->DevDataLock );
    PTT_DEBUG(       "GOT  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    {
        // Ensure we dont overflow the buffer
        if ((pLCSDEV->iFrameOffset +                    // Current buffer Offset
              sizeof( LCSETHFRM ) +                     // Size of Frame Header
              iSize +                                   // Size of Ethernet packet
              sizeof(pLCSEthFrame->bLCSHdr.hwOffset) )  // Size of Frame terminator
            > pLCSDEV->iMaxFrameBufferSize)             // Size of Frame buffer
        {
            PTT_DEBUG( "*DoENQEth ENOBUFS ", 000, pDEVBLK->devnum, bPort );
            PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
            release_lock( &pLCSDEV->DevDataLock );
            errno = ENOBUFS;    // No buffer space available
            return -1;          // (-1==failure)
        }

        // Point to next available LCS Frame slot in our buffer
        pLCSEthFrame = (PLCSETHFRM)( pLCSDEV->bFrameBuffer +
                                     pLCSDEV->iFrameOffset );

        // Increment offset to NEXT available slot (after ours)
        pLCSDEV->iFrameOffset += (U16)(sizeof(LCSETHFRM) + iSize);

        // Plug updated offset to next frame into our frame header
        STORE_HW( pLCSEthFrame->bLCSHdr.hwOffset, pLCSDEV->iFrameOffset );

        // Finish building the LCS Ethernet Passthru frame header
        pLCSEthFrame->bLCSHdr.bType = LCS_FRMTYP_ENET;
        pLCSEthFrame->bLCSHdr.bSlot = bPort;

        // Copy Ethernet packet to LCS Ethernet Passthru frame
        memcpy( pLCSEthFrame->bData, pData, iSize );

        // Tell "LCS_Read" function that data is available for reading
        PTT_DEBUG( "SET  DataPending  ", 1, pDEVBLK->devnum, bPort );
        pLCSDEV->fDataPending = 1;
    }
    PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, bPort );
    release_lock( &pLCSDEV->DevDataLock );

    // (wake up "LCS_Read" function)
    PTT_DEBUG(       "GET  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    obtain_lock( &pLCSDEV->DevEventLock );
    PTT_DEBUG(       "GOT  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    {
        PTT_DEBUG(            "SIG  DevEvent     ", 000, pDEVBLK->devnum, bPort );
        signal_condition( &pLCSDEV->DevEvent );
    }
    PTT_DEBUG(        "REL  DevEventLock ", 000, pDEVBLK->devnum, bPort );
    release_lock( &pLCSDEV->DevEventLock );

    return 0;       // (success)
}

// ====================================================================
//                     lcs_halt_or_clear
// ====================================================================
// The channel is processing a Halt Subchannel or Clear Subchannel
// instruction and is notifying us of that fact so we can stop our
// LCS_Read CCW processing loop.
// --------------------------------------------------------------------

static void lcs_halt_or_clear( DEVBLK* pDEVBLK )
{
    PLCSDEV pLCSDEV = (PLCSDEV) pDEVBLK->dev_data;
    obtain_lock( &pLCSDEV->DevEventLock );
    {
        if (pLCSDEV->fReadWaiting)
        {
            pLCSDEV->fHaltOrClear = 1;
            signal_condition( &pLCSDEV->DevEvent );
        }
    }
    release_lock( &pLCSDEV->DevEventLock );
}

// ====================================================================
//                         LCS_Read
// ====================================================================
// The guest o/s is issuing a Read CCW for our LCS device. Return to
// it all available LCS Frames that we have buffered up in our buffer.
// --------------------------------------------------------------------

void  LCS_Read( DEVBLK* pDEVBLK,   U32   sCount,
                BYTE*   pIOBuf,    BYTE* pUnitStat,
                U32*    pResidual, BYTE* pMore )
{
    PLCSHDR     pLCSHdr;
    PLCSDEV     pLCSDEV = (PLCSDEV)pDEVBLK->dev_data;
    size_t      iLength = 0;
    BYTE*       pBaffle;
    U16         hwBaffleLen;

    struct timespec  waittime;
    struct timeval   now;

    // FIXME: we currently don't support data-chaining but
    // probably should if real LCS devices do (I was unable
    // to determine whether they do or not). -- Fish

    PTT_DEBUG( "READ: ENTRY       ", 000, pDEVBLK->devnum, -1 );

    for (;;)
    {
        // Has anything arrived in our frame buffer yet?

        PTT_DEBUG(       "GET  DevDataLock  ", 000, pDEVBLK->devnum, -1 );
        obtain_lock( &pLCSDEV->DevDataLock );
        PTT_DEBUG(       "GOT  DevDataLock  ", 000, pDEVBLK->devnum, -1 );
        {
            if (pLCSDEV->fDataPending || pLCSDEV->fReplyPending)
                break;
        }
        PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, -1 );
        release_lock( &pLCSDEV->DevDataLock );

        // Keep waiting for LCS Frames to arrive in our frame buffer...

        gettimeofday( &now, NULL );

        waittime.tv_sec  = now.tv_sec  + DEF_NET_READ_TIMEOUT_SECS;
        waittime.tv_nsec = now.tv_usec * 1000;

        PTT_DEBUG(       "GET  DevEventLock ", 000, pDEVBLK->devnum, -1 );
        obtain_lock( &pLCSDEV->DevEventLock );
        PTT_DEBUG(       "GOT  DevEventLock ", 000, pDEVBLK->devnum, -1 );
        {
            PTT_DEBUG( "WAIT DevEventLock ", 000, pDEVBLK->devnum, -1 );
            pLCSDEV->fReadWaiting = 1;
            timed_wait_condition( &pLCSDEV->DevEvent,
                                  &pLCSDEV->DevEventLock,
                                  &waittime );
            pLCSDEV->fReadWaiting = 0;
        }

        PTT_DEBUG(        "WOKE DevEventLock ", 000, pDEVBLK->devnum, -1 );

        // Check for channel conditions...

        if (pLCSDEV->fHaltOrClear)
        {
            *pUnitStat = 0;
            *pResidual = sCount;
            pLCSDEV->fHaltOrClear=0;

            PTT_DEBUG(    "*HALT or CLEAR*   ", *pUnitStat, pDEVBLK->devnum, sCount );

            if (pDEVBLK->ccwtrace || pDEVBLK->ccwstep || pLCSDEV->pLCSBLK->fDebug)
                // "%1d:%04X %s: halt or clear recognized"
                WRMSG( HHC00904, "I", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname );
            release_lock( &pLCSDEV->DevEventLock );
            return;
        }
        release_lock( &pLCSDEV->DevEventLock );

    } // end for (;;)

    // We have frame buffer data to return to the guest...
    // (i.e. Ethernet packets or cmd reply frames)

    PTT_DEBUG( "READ using buffer ", 000, pDEVBLK->devnum, -1 );

    // Point to the end of all buffered LCS Frames (where
    // the next Frame *would* go if there was one), and
    // mark the end of this batch of LCS Frames by setting
    // the "offset to NEXT frame" LCS Header field to zero
    // (a zero "next Frame offset" is like an "EOF" flag).

    pLCSHdr = (PLCSHDR)( pLCSDEV->bFrameBuffer +
                         pLCSDEV->iFrameOffset );
    STORE_HW( pLCSHdr->hwOffset, 0x0000 );

    // Calculate how much data we're going to be giving them.
    // Since 'iFrameOffset' points to the next available LCS
    // Frame slot in our buffer, the total amount of LCS Frame
    // data we have is exactly that amount. We give them two
    // extra bytes however so that they can optionally chase
    // the "hwOffset" field in each LCS Frame's LCS Header to
    // eventually reach our zero hwOffset "EOF" flag).

    iLength = pLCSDEV->iFrameOffset + sizeof(pLCSHdr->hwOffset);

    //

    if ( pLCSDEV->fPendingBaffle )
    {
        pBaffle = pLCSDEV->bFrameBuffer;
        FETCH_HW( hwBaffleLen, pBaffle );
        hwBaffleLen += (U16) ( iLength - SIZEOF_BAFFLE );
        STORE_HW( pBaffle, hwBaffleLen );
    }

    // (calculate residual and set memcpy amount)

    // FIXME: we currently don't support data-chaining but
    // probably should if real LCS devices do (I was unable
    // to determine whether they do or not). -- Fish

    if (sCount < iLength)
    {
        *pMore     = 1;
        *pResidual = 0;

        iLength = sCount;

        // PROGRAMMING NOTE: As a result of the caller asking
        // for less data than we actually have available, the
        // remainder of their unread data they didn't ask for
        // will end up being silently discarded. Refer to the
        // other NOTEs and FIXME's sprinkled throughout this
        // function...
    }
    else
    {
        *pMore      = 0;
        *pResidual -= iLength;
    }

    *pUnitStat = CSW_CE | CSW_DE;

    memcpy( pIOBuf, pLCSDEV->bFrameBuffer, iLength );

    // Display the data read by the guest, if debug is active.
    if (pLCSDEV->pLCSBLK->fDebug)
    {
        // "%1d:%04X %s: Present data of size %d bytes to guest"
        WRMSG(HHC00982, "D", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname, (int)iLength );
        net_data_trace( pDEVBLK, pIOBuf, iLength, '>', 'D', "data", 0 );
    }

    // Reset frame buffer to empty...

    // PROGRAMMING NOTE: even though not all available data
    // may have been read by the guest, we don't currently
    // support data-chaining. Thus any unread data is always
    // discarded by resetting all of the iFrameOffset,
    // fDataPending and fReplyPending fields to 0 so that the
    // next read always grabs a new batch of LCS Frames starting
    // at the very beginning of our frame buffer again. (I was
    // unable to determine whether real LCS devices support
    // data-chaining or not, but if they do we should fix this).

    PTT_DEBUG( "READ empty buffer ", 000, pDEVBLK->devnum, -1 );
    pLCSDEV->iFrameOffset  = 0;
    pLCSDEV->fReplyPending = 0;
    pLCSDEV->fDataPending  = 0;
    pLCSDEV->fPendingBaffle = 0;

    PTT_DEBUG(        "REL  DevDataLock  ", 000, pDEVBLK->devnum, -1 );
    release_lock( &pLCSDEV->DevDataLock );

    PTT_DEBUG( "READ: EXIT        ", 000, pDEVBLK->devnum, -1 );
}

// ====================================================================
//                         ParseArgs
// ====================================================================

int  ParseArgs( DEVBLK* pDEVBLK, PLCSBLK pLCSBLK,
                int argc, char** argx )
{
    struct in_addr  addr;               // Work area for addresses
    MAC             mac;
    int             i;
#if defined(OPTION_W32_CTCI)
    int             iKernBuff;
    int             iIOBuff;
#endif
    char            *argn[MAX_ARGS];
    char            **argv = argn;
    int             saw_if = 0;        /* -x (or --if) specified */
    int             saw_conf = 0;      /* Other configuration flags present */
    BYTE            bMode = LCSDEV_MODE_IP;      /* Default mode is IP */


    // Build a copy of the argv list.
    // getopt() and getopt_long() expect argv[0] to be a program name.
    // We need to shift the arguments and insert a dummy argv[0].
    if (argc > (MAX_ARGS-1))
        argc = (MAX_ARGS-1);
    for (i=0; i < argc; i++)
        argn[i+1] = argx[i];
    argc++;
    argn[0] = pDEVBLK->typname;

    // Housekeeping
    memset( &addr, 0, sizeof( struct in_addr ) );

    // Set some initial defaults
    pLCSBLK->pszTUNDevice   = strdup( DEF_NETDEV );
    pLCSBLK->pszOATFilename = NULL;
    pLCSBLK->pszIPAddress   = NULL;
#if defined( OPTION_W32_CTCI )
    pLCSBLK->iKernBuff = DEF_CAPTURE_BUFFSIZE;
    pLCSBLK->iIOBuff   = DEF_PACKET_BUFFSIZE;
#endif

    // Initialize getopt's counter. This is necessary in the case
    // that getopt was used previously for another device.
    OPTRESET();
    optind = 0;

    // Parse any optional arguments
    while (1)
    {
        int     c;

#if defined( OPTION_W32_CTCI )
  #define  LCS_OPTSTRING    "e:n:m:o:dk:i:w"
#else
  #define  LCS_OPTSTRING    "e:n:x:m:o:d"
#endif
#if defined( HAVE_GETOPT_LONG )
        int     iOpt;

        static struct option options[] =
        {
            { "mode",   required_argument, NULL, 'e' },
            { "dev",    required_argument, NULL, 'n' },
#if !defined(OPTION_W32_CTCI)
            { "if",     required_argument, NULL, 'x' },
#endif /*!defined(OPTION_W32_CTCI)*/
            { "mac",    required_argument, NULL, 'm' },
            { "oat",    required_argument, NULL, 'o' },
            { "debug",  no_argument,       NULL, 'd' },
#if defined( OPTION_W32_CTCI )
            { "kbuff",  required_argument, NULL, 'k' },
            { "ibuff",  required_argument, NULL, 'i' },
            { "swrite", no_argument,       NULL, 'w' },
#endif
            { NULL,     0,                 NULL,  0  }
        };

        c = getopt_long( argc, argv, LCS_OPTSTRING, options, &iOpt );

#else /* defined( HAVE_GETOPT_LONG ) */

        c = getopt( argc, argv, LCS_OPTSTRING );

#endif /* defined( HAVE_GETOPT_LONG ) */

        if (c == -1)
            break;

        switch (c)
        {

        case 'e':  /* Mode */
            if (strcmp( optarg, "SNA" ) == 0 )
            {
                bMode = LCSDEV_MODE_SNA;      /* Mode is SNA */
            }
            else if (strcmp( optarg, "IP" ) == 0 )
            {
                bMode = LCSDEV_MODE_IP;       /* Mode is IP (the default) */
            }
            else
            {
                // "%1d:%04X CTC: option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "device mode", optarg );
                return -1;
            }
            break;

        case 'n':

            if (strlen( optarg ) > sizeof( pDEVBLK->filename ) - 1)
            {
                // "%1d:%04X CTC: option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "device name", optarg );
                return -1;
            }

            pLCSBLK->pszTUNDevice = strdup( optarg );
            break;

#if !defined(OPTION_W32_CTCI)
        case 'x':  /* TAP network interface name */
            if (strlen( optarg ) > sizeof(pLCSBLK->Port[0].szNetIfName)-1)
            {
                // "%1d:%04X %s: option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "TAP device name", optarg );
                return -1;
            }
            STRLCPY( pLCSBLK->Port[0].szNetIfName, optarg );
            saw_if = 1;
            break;
#endif /*!defined(OPTION_W32_CTCI)*/

        case 'm':

            if (0
                || ParseMAC( optarg, mac ) != 0 // (invalid format)
                || !(mac[0] & 0x02)             // (locally assigned MAC bit not ON)
                ||  (mac[0] & 0x01)             // (broadcast bit is ON)
            )
            {
                // "%1d:%04X %s: Option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "MAC address", optarg );
                return -1;
            }

            STRLCPY( pLCSBLK->Port[0].szMACAddress, optarg );
            memcpy( pLCSBLK->Port[0].MAC_Address, &mac, sizeof(MAC) );
            pLCSBLK->Port[0].fLocalMAC = TRUE;
            saw_conf = 1;
            break;

        case 'o':

            pLCSBLK->pszOATFilename = strdup( optarg );
            saw_conf = 1;
            break;

        case 'd':

            pLCSBLK->fDebug = TRUE;
            break;

#if defined( OPTION_W32_CTCI )

        case 'k':     // Kernel Buffer Size (Windows only)

            iKernBuff = atoi( optarg );

            if (iKernBuff * 1024 < MIN_CAPTURE_BUFFSIZE    ||
                iKernBuff * 1024 > MAX_CAPTURE_BUFFSIZE)
            {
                // "%1d:%04X CTC: option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "kernel buffer size", optarg );
                return -1;
            }

            pLCSBLK->iKernBuff = iKernBuff * 1024;
            break;

        case 'i':     // I/O Buffer Size (Windows only)

            iIOBuff = atoi( optarg );

            if (iIOBuff * 1024 < MIN_PACKET_BUFFSIZE    ||
                iIOBuff * 1024 > MAX_PACKET_BUFFSIZE)
            {
                // "%1d:%04X CTC: option %s value %s invalid"
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "dll i/o buffer size", optarg );
                return -1;
            }

            pLCSBLK->iIOBuff = iIOBuff * 1024;
            break;

        case 'w':     // Single packet writes mode (Windows only)

            pLCSBLK->fNoMultiWrite = TRUE;
            break;

#endif // defined( OPTION_W32_CTCI )

        default:
            break;
        }
    }

    argc -= optind;
    argv += optind;

#if !defined(OPTION_W32_CTCI)
    /* If the -x option and one of the configuration options (e.g. the */
    /* -m or the -o options has been specified, reject the -x option.  */
    if (saw_if && saw_conf)
    {
        /* HHC00916 "%1d:%04X %s: option %s value %s invalid" */
        WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
               "TAP device name", pLCSBLK->Port[0].szNetIfName );
        return -1;
    }
#endif /*!defined(OPTION_W32_CTCI)*/

    if (argc > 1)
    {
        /* There are two or more arguments. */
        /* HHC00915 "%1d:%04X %s: incorrect number of parameters" */
        WRMSG(HHC00915, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname );
        return -1;
    }
    else if (argc == 1)
    {
        /* There is one argument, check for an IPv4 address. */
        if (inet_aton( *argv, &addr ) != 0)
        {
            /* The argument is an IPv4 address. If the -x option was specified, */
            /* it has pre-named the TAP interface that LCS will use (*nix).     */
            if ( bMode == LCSDEV_MODE_SNA )      /* Is the mode SNA? */
            {
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "IP address", *argv );
                return -1;
            }
            if ( pLCSBLK->pszIPAddress ) { free( pLCSBLK->pszIPAddress ); pLCSBLK->pszIPAddress = NULL; }
            pLCSBLK->pszIPAddress = strdup( *argv );
            pLCSBLK->Port[0].fPreconfigured = FALSE;
        } else {
#if defined(OPTION_W32_CTCI)
            WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                   "IP address", *argv );
            return -1;
#else /*defined(OPTION_W32_CTCI)*/
            /* The argument is not an IPv4 address. If the -x option was */
            /* specified, the argument shouldn't have been specified.    */
            if (saw_if ) {
                WRMSG( HHC00916, "E", SSID_TO_LCSS(pDEVBLK->ssid), pDEVBLK->devnum, pDEVBLK->typname,
                       "IP address", *argv );
                return -1;
            }
            /* The -x option was not specified, so the argument should be the  */
            /* the name of the pre-configured TAP interface that LCS will use. */
            STRLCPY( pLCSBLK->Port[0].szNetIfName, argv[0] );
            pLCSBLK->Port[0].fPreconfigured = TRUE;
#endif /*defined(OPTION_W32_CTCI)*/
        }
    }
#if !defined(OPTION_W32_CTCI)
    else
    {
        /* There are no arguments. If the -x option was specified, it */
        /* named a pre-configured TAP interface that LCS will use.    */
        if (saw_if)
            pLCSBLK->Port[0].fPreconfigured = TRUE;
        else
            pLCSBLK->Port[0].fPreconfigured = FALSE;
    }
#endif /*!defined(OPTION_W32_CTCI)*/

    return bMode;
}

// ====================================================================
//                           BuildOAT
// ====================================================================

#define OAT_STMT_BUFSZ      255

static int  BuildOAT( char* pszOATName, PLCSBLK pLCSBLK )
{
    FILE*       fp;
    char        szBuff[OAT_STMT_BUFSZ];

    int         i;
    char        c;                      // Character work area
    char*       pszStatement = NULL;    // -> Resolved statement
    char*       pszKeyword;             // -> Statement keyword
    char*       pszOperand;             // -> Statement operand
    int         argc;                   // Number of args
    char*       argv[MAX_ARGS];         // Argument array

    PLCSPORT    pLCSPORT;
    PLCSDEV     pLCSDev;
    PLCSRTE     pLCSRTE;

    U16         sPort;
    BYTE        bMode;
    U16         sDevNum;
    BYTE        bType;
    U32         lIPAddr      = 0;       // (network byte order)
    char*       pszIPAddress = NULL;
    char*       pszNetAddr   = NULL;
    char*       pszNetMask   = NULL;
    char*       strtok_str = NULL;

    struct in_addr  addr;               // Work area for addresses
    char        pathname[MAX_PATH];     // pszOATName in host path format

    // Open the configuration file
    hostpath(pathname, pszOATName, sizeof(pathname));
    fp = fopen( pathname, "r" );
    if (!fp)
    {
        char msgbuf[MAX_PATH+80];
        MSGBUF( msgbuf, "fopen(%s, \"r\")", pathname);
        // "CTC: error in function %s: %s"
        WRMSG( HHC00940, "E", msgbuf, strerror( errno ) );
        return -1;
    }

    for (;;)
    {
        // Read next record from the OAT file
        if (!ReadOAT( pszOATName, fp, szBuff ))
        {
            fclose( fp );
            return 0;
        }

        if (pszStatement)
        {
            free( pszStatement );
            pszStatement = NULL;
        }

        // Make a copy of the OAT statement
        pszStatement = strdup( szBuff );

        /* Perform variable substitution */
        {
            char *cl;

            set_symbol( "CUU",  "$(CUU)"  );
            set_symbol( "CCUU", "$(CCUU)" );
            set_symbol( "DEVN", "$(DEVN)" );

            cl = resolve_symbol_string( pszStatement );

            if (cl)
            {
                STRLCPY( szBuff, cl );
                free( cl );
                free( pszStatement );
                pszStatement = strdup( szBuff );
            }
        }

        sPort        = 0;
        bMode        = 0;
        sDevNum      = 0;
        bType        = 0;
        pszIPAddress = NULL;
        pszNetAddr   = NULL;
        pszNetMask   = NULL;

        memset( &addr, 0, sizeof( addr ) );

        // Split the statement into keyword and first operand
        pszKeyword = strtok_r( pszStatement, " \t", &strtok_str );
        pszOperand = strtok_r( NULL,   " \t", &strtok_str );

        // Extract any arguments
        for (argc = 0;
             argc < MAX_ARGS &&
                 ( argv[argc] = strtok_r( NULL, " \t", &strtok_str ) ) != NULL &&
                 argv[argc][0] != '#';
             argc++)
                 /* nop */
                 ;

        // Clear any unused argument pointers
        for (i = argc; i < MAX_ARGS; i++)
            argv[i] = NULL;

        if (strcasecmp( pszKeyword, "HWADD" ) == 0)
        {
            if (!pszOperand        ||
                argc       != 1    ||
                sscanf( pszOperand, "%hi%c", &sPort, &c ) != 1)
            {
                // "CTC: invalid statement %s in file %s: %s"
                WRMSG( HHC00954, "E", "HWADD", pszOATName, szBuff );
                return -1;
            }

            pLCSPORT = &pLCSBLK->Port[sPort];

            if (0
                || ParseMAC( argv[0], pLCSPORT->MAC_Address ) != 0
                || !(pLCSPORT->MAC_Address[0] & 0x02)
                ||  (pLCSPORT->MAC_Address[0] & 0x01)
            )
            {
                // "CTC: invalid %s %s in statement %s in file %s: %s"
                WRMSG( HHC00955, "E", "MAC", argv[0], "HWADD", pszOATName, szBuff );

                memset( pLCSPORT->MAC_Address, 0, sizeof(MAC) );
                return -1;
            }

            STRLCPY( pLCSPORT->szMACAddress, argv[0] );
            pLCSPORT->fLocalMAC = TRUE;
        }
        else if (strcasecmp( pszKeyword, "ROUTE" ) == 0)
        {
            if (!pszOperand        ||
                argc       != 2    ||
                sscanf( pszOperand, "%hi%c", &sPort, &c ) != 1)
            {
                // "CTC: invalid statement %s in file %s: %s"
                WRMSG( HHC00954, "E", "ROUTE", pszOATName, szBuff );
                return -1;
            }

            if (inet_aton( argv[0], &addr ) == 0)
            {
                // "CTC: invalid %s %s in statement %s in file %s: %s"
                WRMSG( HHC00955, "E", "net address", argv[0], "ROUTE", pszOATName, szBuff );
                return -1;
            }

            pszNetAddr = strdup( argv[0] );

            if (inet_aton( argv[1], &addr ) == 0)
            {
                free(pszNetAddr);
                // "CTC: invalid %s %s in statement %s in file %s: %s"
                WRMSG( HHC00955, "E", "netmask", argv[1], "ROUTE", pszOATName, szBuff );
                return -1;
            }

            pszNetMask = strdup( argv[1] );

            pLCSPORT = &pLCSBLK->Port[sPort];

            if (!pLCSPORT->pRoutes)
            {
                pLCSPORT->pRoutes = malloc( sizeof( LCSRTE ) );
                pLCSRTE = pLCSPORT->pRoutes;
            }
            else
            {
                for (pLCSRTE = pLCSPORT->pRoutes;
                     pLCSRTE->pNext;
                     pLCSRTE = pLCSRTE->pNext);

                pLCSRTE->pNext = malloc( sizeof( LCSRTE ) );
                pLCSRTE = pLCSRTE->pNext;
            }

            pLCSRTE->pszNetAddr = pszNetAddr;
            pLCSRTE->pszNetMask = pszNetMask;
            pLCSRTE->pNext      = NULL;
        }
        else // (presumed OAT file device statement)
        {
            if (!pszKeyword || !pszOperand)
            {
                // "CTC: error in file %s: missing device number or mode"
                WRMSG( HHC00956, "E", pszOATName );
                return -1;
            }

            /*                                                       */
            /* The keyword is a device address.                      */
            /*                                                       */
            /* If the operand, i.e. the mode, is IP, the device      */
            /* address can be either the even or the odd address of  */
            /* an even/odd device address pair, e.g. either 0E42 or  */
            /* 0E43. Whichever device address is specified will      */
            /* become the read device, and the other device address  */
            /* of the even/odd pair will become the write device.    */
            /* For example, if device address 0E42 is specified,     */
            /* device address 0E42 will become the read device, and  */
            /* device address 0E43 will become the write device.     */
            /* However, if device address 0E43 is specified, device  */
            /* address 0E43 will become the read device, and device  */
            /* address 0E42 will become the write device.            */
            /*                                                       */
            /* If the operand, i.e. the mode, is SNA, the device     */
            /* address can be any device address, e.g. 0E42 or 0E43. */
            /* SNA uses a single device for both read and write.     */
            /*                                                       */
            /* The following extract from an OAT illustrates the     */
            /* above. The guest will have four interfaces, two IP    */
            /* and two SNA. All four interfaces will use the same    */
            /* port, i.e the tap. The first IP interface will use    */
            /* the even device address 0E40 as the read device and   */
            /* the odd device address 0E41 as the write device.      */
            /* However, the second IP interface will use the odd     */
            /* device address 0E43 as the read device and the even   */
            /* device address 0E42 as the write device. The first    */
            /* SNA interface will use the even device address 0E44,  */
            /* and the second SNA interface will use the odd device  */
            /* address 0E45.                                         */
            /*                                                       */
            /*   0E40  IP   00  NO  192.168.1.1                      */
            /*   0E43  IP   00  NO  192.168.1.2                      */
            /*   0E44  SNA  00                                       */
            /*   0E45  SNA  00                                       */
            /*                                                       */
            if (strlen( pszKeyword ) > 4 ||
                sscanf( pszKeyword, "%hx%c", &sDevNum, &c ) != 1)
            {
                // "CTC: error in file %s: invalid %s value %s"
                WRMSG( HHC00957, "E", pszOATName, "device number", pszKeyword );
                return -1;
            }

            if (strcasecmp( pszOperand, "IP" ) == 0)
            {
                bMode = LCSDEV_MODE_IP;

                if (argc < 1)
                {
                    // "CTC: error in file %s: %s: missing port number"
                    WRMSG( HHC00958, "E", pszOATName, szBuff );
                    return -1;
                }

                if (sscanf( argv[0], "%hi%c", &sPort, &c ) != 1)
                {
                    // "CTC: error in file %s: invalid %s value %s"
                    WRMSG( HHC00957, "E", pszOATName, "port number", argv[0] );
                    return -1;
                }

                if (argc > 1)
                {
                         if (strcasecmp( argv[1], "PRI" ) == 0) bType = LCSDEV_TYPE_PRIMARY;
                    else if (strcasecmp( argv[1], "SEC" ) == 0) bType = LCSDEV_TYPE_SECONDARY;
                    else if (strcasecmp( argv[1], "NO"  ) == 0) bType = LCSDEV_TYPE_NONE;
                    else
                    {
                        // "CTC: error in file %s: %s: invalid entry starting at %s"
                        WRMSG( HHC00959, "E", pszOATName, szBuff, argv[1] );
                        return -1;
                    }

                    if (argc > 2)
                    {
                        pszIPAddress = strdup( argv[2] );

                        if (inet_aton( pszIPAddress, &addr ) == 0)
                        {
                            // "CTC: error in file %s: invalid %s value %s"
                            WRMSG( HHC00957, "E", pszOATName, "IP address", pszIPAddress );
                            return -1;
                        }

                        lIPAddr = addr.s_addr;  // (network byte order)
                    }
                }
            }
            else if (strcasecmp( pszOperand, "SNA" ) == 0)
            {
                bMode = LCSDEV_MODE_SNA;

                if (argc < 1)
                {
                    // "CTC: error in file %s: %s: missing port number"
                    WRMSG( HHC00958, "E", pszOATName, szBuff );
                    return -1;
                }

                if (sscanf( argv[0], "%hi%c", &sPort, &c ) != 1)
                {
                    // "CTC: error in file %s: invalid %s value %s"
                    WRMSG( HHC00957, "E", pszOATName, "port number", argv[0] );
                    return -1;
                }

                if (argc > 1)
                {
                    // "CTC: error in file %s: %s: SNA does not accept any arguments"
                    WRMSG( HHC00960, "E", pszOATName, szBuff );
                    return -1;
                }
            }
            else
            {
                // "CTC: error in file %s: %s: invalid mode"
                WRMSG( HHC00961, "E", pszOATName, pszOperand );
                return -1;
            }

            // Create new LCS Device...

            pLCSDev = malloc( sizeof( LCSDEV ) );
            memset( pLCSDev, 0, sizeof( LCSDEV ) );

            pLCSDev->sAddr        = sDevNum;
            pLCSDev->bMode        = bMode;
            pLCSDev->bPort        = sPort;
            pLCSDev->bType        = bType;
            pLCSDev->lIPAddress   = lIPAddr;   // (network byte order)
            pLCSDev->pszIPAddress = pszIPAddress;
            pLCSDev->pNext        = NULL;

            // Add it to end of chain...

            if (!pLCSBLK->pDevices)
                pLCSBLK->pDevices = pLCSDev; // (first link in chain)
            else
            {
                PLCSDEV pOldLastLCSDEV;
                // (find last link in chain)
                for (pOldLastLCSDEV = pLCSBLK->pDevices;
                     pOldLastLCSDEV->pNext;
                     pOldLastLCSDEV = pOldLastLCSDEV->pNext);
                // (add new link to end of chain)
                pOldLastLCSDEV->pNext = pLCSDev;
            }

            // Count it...

            if (pLCSDev->bMode == LCSDEV_MODE_IP)
                pLCSBLK->icDevices += 2;
            else
                pLCSBLK->icDevices += 1;

        } // end OAT file statement

    } // end for (;;)

    UNREACHABLE_CODE( return -1 );
}

// ====================================================================
//                           ReadOAT
// ====================================================================

static char*  ReadOAT( char* pszOATName, FILE* fp, char* pszBuff )
{
    int     c;                          // Character work area
    int     iLine = 0;                  // Statement number
    int     iLen;                       // Statement length

    while (1)
    {
        // Increment statement number
        iLine++;

        // Read next statement from OAT
        for (iLen = 0 ; ; )
        {
            // Read character from OAT
            c = fgetc( fp );

            // Check for I/O error
            if (ferror( fp ))
            {
                // "CTC: error in file %s: reading line %d: %s"
                WRMSG( HHC00962, "E", pszOATName, iLine, strerror( errno ) );
                return NULL;
            }

            // Check for end of file
            if (iLen == 0 && ( c == EOF || c == '\x1A' ))
                return NULL;

            // Check for end of line
            if (c == '\n' || c == EOF || c == '\x1A')
                break;

            // Ignore leading blanks and tabs
            if (iLen == 0 && ( c == ' ' || c == '\t' ))
                continue;

            // Ignore nulls and carriage returns
            if (c == '\0' || c == '\r')
                continue;

            // Check that statement does not overflow bufffer
            if (iLen >= OAT_STMT_BUFSZ)
            {
                // "CTC: error in file %s: line %d is too long"
                WRMSG( HHC00963, "E", pszOATName, iLine );
                exit(1);
            }

            // Append character to buffer
            pszBuff[iLen++] = c;
        }

        // Null terminate buffer
        pszBuff[ iLen ] = 0;

        // Remove trailing whitespace
        RTRIM( pszBuff );
        iLen = (int) strlen( pszBuff );

        // Ignore comments and null statements
        if (!iLen || pszBuff[0] == '*' || pszBuff[0] == '#')
            continue;

        break;
    }

    return pszBuff;
}

// ====================================================================
//                 Device Handler Information
// ====================================================================

/* NOTE : lcs_device_hndinfo is NEVER static as it is referenced by the CTC meta driver */
DEVHND lcs_device_hndinfo =
{
        &LCS_Init,                     /* Device Initialization      */
        &LCS_ExecuteCCW,               /* Device CCW execute         */
        &LCS_Close,                    /* Device Close               */
        &LCS_Query,                    /* Device Query               */
        NULL,                          /* Device Extended Query      */
        NULL,                          /* Device Start channel pgm   */
        NULL,                          /* Device End channel pgm     */
        NULL,                          /* Device Resume channel pgm  */
        NULL,                          /* Device Suspend channel pgm */
        &lcs_halt_or_clear,            /* Device Halt channel pgm    */
        NULL,                          /* Device Read                */
        NULL,                          /* Device Write               */
        NULL,                          /* Device Query used          */
        NULL,                          /* Device Reserve             */
        NULL,                          /* Device Release             */
        NULL,                          /* Device Attention           */
        CTC_Immed_Commands,            /* Immediate CCW Codes        */
        NULL,                          /* Signal Adapter Input       */
        NULL,                          /* Signal Adapter Output      */
        NULL,                          /* Signal Adapter Sync        */
        NULL,                          /* Signal Adapter Output Mult */
        NULL,                          /* QDIO subsys desc           */
        NULL,                          /* QDIO set subchan ind       */
        NULL,                          /* Hercules suspend           */
        NULL                           /* Hercules resume            */
};


/* Libtool static name colision resolution */
/* note : lt_dlopen will look for symbol & modulename_LTX_symbol */

#if defined( HDL_USE_LIBTOOL )
#define hdl_ddev hdt3088_LTX_hdl_ddev
#define hdl_depc hdt3088_LTX_hdl_depc
#define hdl_reso hdt3088_LTX_hdl_reso
#define hdl_init hdt3088_LTX_hdl_init
#define hdl_fini hdt3088_LTX_hdl_fini
#endif

HDL_DEPENDENCY_SECTION;
{
     HDL_DEPENDENCY(HERCULES);
     HDL_DEPENDENCY(DEVBLK);
}
END_DEPENDENCY_SECTION

HDL_REGISTER_SECTION;       // ("Register" our entry-points)

//                 Hercules's          Our
//                 registered          overriding
//                 entry-point         entry-point
//                 name                value

#if defined( WIN32 )
  HDL_REGISTER ( debug_tt32_stats,   display_tt32_stats        );
  HDL_REGISTER ( debug_tt32_tracing, enable_tt32_debug_tracing );
#else
  UNREFERENCED( regsym );   // (HDL_REGISTER_SECTION parameter)
#endif

END_REGISTER_SECTION


HDL_DEVICE_SECTION;
{
    HDL_DEVICE( LCS, lcs_device_hndinfo );

// ZZ the following device types should be moved to
// ZZ their own loadable modules

    HDL_DEVICE( CTCI, ctci_device_hndinfo );
    HDL_DEVICE( CTCT, ctct_device_hndinfo );
    HDL_DEVICE( CTCE, ctce_device_hndinfo );
}
END_DEVICE_SECTION

#if defined( _MSVC_ ) && defined( NO_LCS_OPTIMIZE )
  #pragma optimize( "", on )            // restore previous settings
#endif

#endif /* !defined(__SOLARIS__)  jbs 10/2007 10/2007 */
