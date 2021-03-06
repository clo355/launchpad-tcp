// Standard includes
#include <stdlib.h>
#include <string.h>

// simplelink includes 
#include "simplelink.h"
#include "wlan.h"

// driverlib includes 
#include "hw_ints.h"
#include "hw_types.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "uart.h"
#include "utils.h"

// common interface includes 
#include "udma_if.h"
#include "common.h"
#ifndef NOTERM
#include "uart_if.h"
#endif

#include "pinmux.h"

#define IP_ADDR             0x23A10648 /*EC2 IP: 35.161.6.72, convert to hex*/
#define PORT_NUM            5001
#define BUF_SIZE            1024
#define TCP_PACKET_COUNT    1

// Application specific status/error codes
typedef enum{
    // Choosing -0x7D0 to avoid overlap w/ host-driver's error codes
    SOCKET_CREATE_ERROR = -0x7D0,
    BIND_ERROR = SOCKET_CREATE_ERROR - 1,
    LISTEN_ERROR = BIND_ERROR -1,
    SOCKET_OPT_ERROR = LISTEN_ERROR -1,
    CONNECT_ERROR = SOCKET_OPT_ERROR -1,
    ACCEPT_ERROR = CONNECT_ERROR - 1,
    SEND_ERROR = ACCEPT_ERROR -1,
    RECV_ERROR = SEND_ERROR -1,
    SOCKET_CLOSE_ERROR = RECV_ERROR -1,
    DEVICE_NOT_IN_STATION_MODE = SOCKET_CLOSE_ERROR - 1,
    STATUS_CODE_MAX = -0xBB8
}e_AppStatusCodes;


//****************************************************************************
//                      LOCAL FUNCTION PROTOTYPES
//****************************************************************************
int BsdTcpClient(unsigned short usPort);
int BsdTcpServer(unsigned short usPort);
static long WlanConnect();
static void BoardInit();
static void InitializeAppVariables();


//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************
volatile unsigned long  g_ulStatus = 0;//SimpleLink Status
unsigned long  g_ulGatewayIP = 0; //Network Gateway IP address
unsigned char  g_ucConnectionSSID[SSID_LEN_MAX+1]; //Connection SSID
unsigned char  g_ucConnectionBSSID[BSSID_LEN_MAX]; //Connection BSSID
unsigned long  g_ulDestinationIp = IP_ADDR;
unsigned int   g_uiPortNum = PORT_NUM;
volatile unsigned long  g_ulPacketCount = TCP_PACKET_COUNT;
unsigned char  g_ucConnectionStatus = 0;
unsigned char  g_ucSimplelinkstarted = 0;
unsigned long  g_ulIpAddr = 0;
char g_cBsdBuf[BUF_SIZE];

#if defined(ccs) || defined (gcc)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif
//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************



//*****************************************************************************
// SimpleLink Asynchronous Event Handlers -- Start
//*****************************************************************************


//*****************************************************************************
//
//! \brief The Function Handles WLAN Events
//!
//*****************************************************************************
void SimpleLinkWlanEventHandler(SlWlanEvent_t *pWlanEvent)
{
    if(!pWlanEvent)
    {
        return;
    }

    switch(pWlanEvent->Event)
    {
        case SL_WLAN_CONNECT_EVENT:
        {
            SET_STATUS_BIT(g_ulStatus, STATUS_BIT_CONNECTION);

            //
            // Information about the connected AP (like name, MAC etc) will be
            // available in 'slWlanConnectAsyncResponse_t'-Applications
            // can use it if required
            //
            //  slWlanConnectAsyncResponse_t *pEventData = NULL;
            // pEventData = &pWlanEvent->EventData.STAandP2PModeWlanConnected;
            //

            // Copy new connection SSID and BSSID to global parameters
            memcpy(g_ucConnectionSSID,pWlanEvent->EventData.
                   STAandP2PModeWlanConnected.ssid_name,
                   pWlanEvent->EventData.STAandP2PModeWlanConnected.ssid_len);
            memcpy(g_ucConnectionBSSID,
                   pWlanEvent->EventData.STAandP2PModeWlanConnected.bssid,
                   SL_BSSID_LENGTH);
        }
        break;

        case SL_WLAN_DISCONNECT_EVENT:
        {
            slWlanConnectAsyncResponse_t*  pEventData = NULL;

            CLR_STATUS_BIT(g_ulStatus, STATUS_BIT_CONNECTION);
            CLR_STATUS_BIT(g_ulStatus, STATUS_BIT_IP_AQUIRED);

            pEventData = &pWlanEvent->EventData.STAandP2PModeDisconnected;

            // If the user has initiated 'Disconnect' request,
            //'reason_code' is SL_USER_INITIATED_DISCONNECTION
            if(SL_USER_INITIATED_DISCONNECTION == pEventData->reason_code)
            {
                UART_PRINT("[WLAN EVENT]Device disconnected from the AP: %s,"
                "BSSID: %x:%x:%x:%x:%x:%x on application's request \n\r",
                           g_ucConnectionSSID,g_ucConnectionBSSID[0],
                           g_ucConnectionBSSID[1],g_ucConnectionBSSID[2],
                           g_ucConnectionBSSID[3],g_ucConnectionBSSID[4],
                           g_ucConnectionBSSID[5]);
            }
            else
            {
                UART_PRINT("[WLAN ERROR]Device disconnected from the AP AP: %s,"
                            "BSSID: %x:%x:%x:%x:%x:%x on an ERROR..!! \n\r",
                           g_ucConnectionSSID,g_ucConnectionBSSID[0],
                           g_ucConnectionBSSID[1],g_ucConnectionBSSID[2],
                           g_ucConnectionBSSID[3],g_ucConnectionBSSID[4],
                           g_ucConnectionBSSID[5]);
            }
            memset(g_ucConnectionSSID,0,sizeof(g_ucConnectionSSID));
            memset(g_ucConnectionBSSID,0,sizeof(g_ucConnectionBSSID));
        }
        break;

        default:
        {
            UART_PRINT("[WLAN EVENT] Unexpected event [0x%x]\n\r",
                       pWlanEvent->Event);
        }
        break;
    }
}

//*****************************************************************************
//
//! \brief This function handles network events such as IP acquisition, IP
//!           leased, IP released etc.
//!
//*****************************************************************************
void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent)
{
    if(!pNetAppEvent)
    {
        return;
    }

    switch(pNetAppEvent->Event)
    {
        case SL_NETAPP_IPV4_IPACQUIRED_EVENT:
        {
            SlIpV4AcquiredAsync_t *pEventData = NULL;

            SET_STATUS_BIT(g_ulStatus, STATUS_BIT_IP_AQUIRED);

            //Ip Acquired Event Data
            pEventData = &pNetAppEvent->EventData.ipAcquiredV4;
            g_ulIpAddr = pEventData->ip;

            //Gateway IP address
            g_ulGatewayIP = pEventData->gateway;
        }
        break;

        default:
        {
            UART_PRINT("[NETAPP EVENT] Unexpected event [0x%x] \n\r",
                       pNetAppEvent->Event);
        }
        break;
    }
}


//*****************************************************************************
//
//! \brief This function handles HTTP server events
//!
//! \param[in]  pServerEvent - Contains the relevant event information
//! \param[in]    pServerResponse - Should be filled by the user with the
//!                                      relevant response information
//!
//****************************************************************************
void SimpleLinkHttpServerCallback(SlHttpServerEvent_t *pHttpEvent,
                                  SlHttpServerResponse_t *pHttpResponse){
    // Unused
}

//*****************************************************************************
//
//! \brief This function handles General Events
//!
//! \param[in]     pDevEvent - Pointer to General Event Info
//!
//*****************************************************************************
void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *pDevEvent){
    if(!pDevEvent){
        return;
    }

    // Most of the general errors are not FATAL are are to be handled
    // appropriately by the application
    //
    UART_PRINT("[GENERAL EVENT] - ID=[%d] Sender=[%d]\n\n",
               pDevEvent->EventData.deviceEvent.status,
               pDevEvent->EventData.deviceEvent.sender);
}


//*****************************************************************************
//
//! This function handles socket events indication
//!
//! \param[in]      pSock - Pointer to Socket Event Info
//!
//*****************************************************************************
void SimpleLinkSockEventHandler(SlSockEvent_t *pSock)
{
    if(!pSock){
        return;
    }

    // This application doesn't work w/ socket - Events are not expected
    switch(pSock->Event){
        case SL_SOCKET_TX_FAILED_EVENT:
            switch( pSock->socketAsyncEvent.SockTxFailData.status)
            {
                case SL_ECLOSE: 
                    UART_PRINT("[SOCK ERROR] - close socket (%d) operation "
                                "failed to transmit all queued packets\n\n", 
                                    pSock->socketAsyncEvent.SockTxFailData.sd);
                    break;
                default: 
                    UART_PRINT("[SOCK ERROR] - TX FAILED  :  socket %d , reason "
                                "(%d) \n\n",
                                pSock->socketAsyncEvent.SockTxFailData.sd, pSock->socketAsyncEvent.SockTxFailData.status);
                  break;
            }
            break;

        default:
        	UART_PRINT("[SOCK EVENT] - Unexpected Event [%x0x]\n\n",pSock->Event);
          break;
    }
}


//*****************************************************************************
// SimpleLink Asynchronous Event Handlers -- End
//*****************************************************************************



//*****************************************************************************
//
//! This function initializes the application variables
//!
//*****************************************************************************
static void InitializeAppVariables()
{
    g_ulStatus = 0;
    g_ulGatewayIP = 0;
    memset(g_ucConnectionSSID,0,sizeof(g_ucConnectionSSID));
    memset(g_ucConnectionBSSID,0,sizeof(g_ucConnectionBSSID));
    g_ulDestinationIp = IP_ADDR;
    g_uiPortNum = PORT_NUM;
    g_ulPacketCount = TCP_PACKET_COUNT;
}

//*****************************************************************************
//! \brief This function puts the device in its default state. It:
//!           - Set the mode to STATION
//!           - Configures connection policy to Auto and AutoSmartConfig
//!           - Deletes all the stored profiles
//!           - Enables DHCP
//!           - Disables Scan policy
//!           - Sets Tx power to maximum
//!           - Sets power policy to normal
//!           - Unregister mDNS services
//!           - Remove all filters
//!
//! \param   none
//! \return  On success, zero is returned. On error, negative is returned
//*****************************************************************************
static long ConfigureSimpleLinkToDefaultState()
{
    SlVersionFull   ver = {0};
    _WlanRxFilterOperationCommandBuff_t  RxFilterIdMask = {0};

    unsigned char ucVal = 1;
    unsigned char ucConfigOpt = 0;
    unsigned char ucConfigLen = 0;
    unsigned char ucPower = 0;

    long lRetVal = -1;
    long lMode = -1;

    lMode = sl_Start(0, 0, 0);
    ASSERT_ON_ERROR(lMode);

    // If the device is not in station-mode, try configuring it in station-mode 
    if (ROLE_STA != lMode)
    {
        if (ROLE_AP == lMode)
        {
            // If the device is in AP mode, we need to wait for this event 
            // before doing anything 
            while(!IS_IP_ACQUIRED(g_ulStatus))
            {
#ifndef SL_PLATFORM_MULTI_THREADED
              _SlNonOsMainLoopTask(); 
#endif
            }
        }

        // Switch to STA role and restart 
        lRetVal = sl_WlanSetMode(ROLE_STA);
        ASSERT_ON_ERROR(lRetVal);

        lRetVal = sl_Stop(0xFF);
        ASSERT_ON_ERROR(lRetVal);

        lRetVal = sl_Start(0, 0, 0);
        ASSERT_ON_ERROR(lRetVal);

        // Check if the device is in station again 
        if (ROLE_STA != lRetVal)
        {
            // We don't want to proceed if the device is not coming up in STA-mode 
            return DEVICE_NOT_IN_STATION_MODE;
        }
    }
    
    // Get the device's version-information
    ucConfigOpt = SL_DEVICE_GENERAL_VERSION;
    ucConfigLen = sizeof(ver);
    lRetVal = sl_DevGet(SL_DEVICE_GENERAL_CONFIGURATION, &ucConfigOpt, 
                                &ucConfigLen, (unsigned char *)(&ver));
    ASSERT_ON_ERROR(lRetVal);

    // Set connection policy to Auto + SmartConfig 
    //      (Device's default connection policy)
    lRetVal = sl_WlanPolicySet(SL_POLICY_CONNECTION, 
                                SL_CONNECTION_POLICY(1, 0, 0, 0, 1), NULL, 0);
    ASSERT_ON_ERROR(lRetVal);

    // Remove all profiles
    lRetVal = sl_WlanProfileDel(0xFF);
    ASSERT_ON_ERROR(lRetVal);

    

    //
    // Device in station-mode. Disconnect previous connection if any
    // The function returns 0 if 'Disconnected done', negative number if already
    // disconnected Wait for 'disconnection' event if 0 is returned, Ignore 
    // other return-codes
    //
    lRetVal = sl_WlanDisconnect();
    if(0 == lRetVal)
    {
        // Wait
        while(IS_CONNECTED(g_ulStatus))
        {
#ifndef SL_PLATFORM_MULTI_THREADED
              _SlNonOsMainLoopTask(); 
#endif
        }
    }

    // Enable DHCP client
    lRetVal = sl_NetCfgSet(SL_IPV4_STA_P2P_CL_DHCP_ENABLE,1,1,&ucVal);
    ASSERT_ON_ERROR(lRetVal);

    // Disable scan
    ucConfigOpt = SL_SCAN_POLICY(0);
    lRetVal = sl_WlanPolicySet(SL_POLICY_SCAN , ucConfigOpt, NULL, 0);
    ASSERT_ON_ERROR(lRetVal);

    // Set Tx power level for station mode
    // Number between 0-15, as dB offset from max power - 0 will set max power
    ucPower = 0;
    lRetVal = sl_WlanSet(SL_WLAN_CFG_GENERAL_PARAM_ID, 
            WLAN_GENERAL_PARAM_OPT_STA_TX_POWER, 1, (unsigned char *)&ucPower);
    ASSERT_ON_ERROR(lRetVal);

    // Set PM policy to normal
    lRetVal = sl_WlanPolicySet(SL_POLICY_PM , SL_NORMAL_POLICY, NULL, 0);
    ASSERT_ON_ERROR(lRetVal);

    // Unregister mDNS services
    lRetVal = sl_NetAppMDNSUnRegisterService(0, 0);
    ASSERT_ON_ERROR(lRetVal);

    // Remove  all 64 filters (8*8)
    memset(RxFilterIdMask.FilterIdMask, 0xFF, 8);
    lRetVal = sl_WlanRxFilterSet(SL_REMOVE_RX_FILTER, (_u8 *)&RxFilterIdMask,
                       sizeof(_WlanRxFilterOperationCommandBuff_t));
    ASSERT_ON_ERROR(lRetVal);

    lRetVal = sl_Stop(SL_STOP_TIMEOUT);
    ASSERT_ON_ERROR(lRetVal);

    InitializeAppVariables();
    
    return lRetVal; // Success
}



//****************************************************************************
//
//!    \brief Parse the input IP address from the user
//!
//!    \param[in]                     ucCMD (char pointer to input string)
//!
//!    \return                        0 : if correct IP, -1 : incorrect IP
//
//****************************************************************************
int IpAddressParser(char *ucCMD)
{
    volatile int i=0;
    unsigned int uiUserInputData;
    unsigned long ulUserIpAddress = 0;
    char *ucInpString;
    ucInpString = strtok(ucCMD, ".");
    uiUserInputData = (int)strtoul(ucInpString,0,10);
    while(i<4)
    {
        //
       // Check Whether IP is valid
       //
       if((ucInpString != NULL) && (uiUserInputData < 256))
       {
           ulUserIpAddress |= uiUserInputData;
           if(i < 3)
               ulUserIpAddress = ulUserIpAddress << 8;
           ucInpString=strtok(NULL,".");
           uiUserInputData = (int)strtoul(ucInpString,0,10);
           i++;
       }
       else
       {
           return -1;
       }
    }
    g_ulDestinationIp = ulUserIpAddress;
    return SUCCESS;
}
//*****************************************************************************
//
//! UserInput
//
//*****************************************************************************
long UserInput()
{
    int iInput = 0;
    char acCmdStore[50];
    int lRetVal;
    int iRightInput = 0;
    unsigned long ulUserInputData = 0;

    UART_PRINT("SSID Name: %s\n\rPORT: %d\n\rDestination IP: %d.%d.%d.%d\n\r",
                    SSID_NAME, g_uiPortNum,
                    SL_IPV4_BYTE(g_ulDestinationIp,3),
                    SL_IPV4_BYTE(g_ulDestinationIp,2),
                    SL_IPV4_BYTE(g_ulDestinationIp,1),
                    SL_IPV4_BYTE(g_ulDestinationIp,0));

    do
    {
        UART_PRINT("\r\n1 = Send file as TCP client\r\n2 = Receive "
                    "file as TCP server\r\n");
        UART_PRINT("Enter: ");
        lRetVal = GetCmd(acCmdStore, sizeof(acCmdStore));
        if(lRetVal == 0){
          UART_PRINT("\n\n\rEnter Valid Input.");
        }
        else{
            iInput  = (int)strtoul(acCmdStore,0,10);
          if(iInput  == 1){
              UART_PRINT("Connecting to EC2 and receiving file...\n\r");
              lRetVal = BsdTcpServer(g_uiPortNum);
          }
          else{
            UART_PRINT("\n\n\rWrong Input");
          }
        }
        UART_PRINT("\n\r");
    }while(1);

    return SUCCESS;

}


//****************************************************************************
//! Listens for a connection request from EC2 client, then receives data.
//****************************************************************************
int BsdTcpServer(unsigned short usPort)
{
    SlSockAddrIn_t  sAddr;
    int             iAddrSize;
    int             iSockID;
    int             iStatus;
    int             iTestBufLen = BUF_SIZE;
    int 			resetBufCount = 0;

    //File IO from:
    //http://www.multisilicon.com/_a/blog/a25365269~/group___file_system.html#ga6a9aaae1813255fa13c7d6bc26c2904c
    //Requires fs.h
    char*			myFileName = "LP2_received_data.txt";
    long            myFileHandle = -1; //get's replaced
    long            LP2_RetVal;        //negative retval is an error
    unsigned long   Offset = 0;	       //keep track of how much of the file has been sent

    // open file (create mode)
    LP2_RetVal = sl_FsOpen((unsigned char *) myFileName,
    		FS_MODE_OPEN_CREATE(65536,  _FS_FILE_OPEN_FLAG_NO_SIGNATURE_TEST | _FS_FILE_OPEN_FLAG_COMMIT ),
			NULL,
			&myFileHandle);

    //filling the TCP server socket address
    sAddr.sin_family = SL_AF_INET;
    sAddr.sin_port = sl_Htons((unsigned short)usPort);
    sAddr.sin_addr.s_addr = sl_Htonl((unsigned int)g_ulDestinationIp);

    iAddrSize = sizeof(SlSockAddrIn_t);

    // creating a TCP socket
    iSockID = sl_Socket(SL_AF_INET,SL_SOCK_STREAM, 0);
    if( iSockID < 0 ){
        ASSERT_ON_ERROR(SOCKET_CREATE_ERROR);
    }

    //connecting to TCP server
    iStatus = sl_Connect(iSockID, ( SlSockAddr_t *)&sAddr, iAddrSize);
    if( iStatus < 0 ){
        // error
        sl_Close(iSockID);       
        ASSERT_ON_ERROR(CONNECT_ERROR);
    }

    //Receiving packets from EC2
    iStatus = 1024;
    Offset = 0;
    iStatus = sl_Recv(iSockID, g_cBsdBuf, iTestBufLen, 0); //returns # of bytes received
    UART_PRINT("\n\rReceived %d bytes\n\r", iStatus);
    LP2_RetVal = sl_FsWrite(myFileHandle, Offset, (unsigned char *) g_cBsdBuf, 1024); //Write g_cBsdBuf to file
    Offset += LP2_RetVal;
    if( iStatus <= 0 ){
      // error
      sl_Close(iSockID);
      UART_PRINT("iStatus is %d\n\r", iStatus);
      ASSERT_ON_ERROR(RECV_ERROR);
    }
    
    for(resetBufCount = 0; resetBufCount<1024; resetBufCount++){ //clear g_cBsdBuf
      g_cBsdBuf[resetBufCount] = '\0';
    }

    Report("Recieved %u packets successfully\n\n\n\n\n\n\n\r",g_ulPacketCount);

    //closing the socket after sending packets
    iStatus = sl_Close(iSockID);
    ASSERT_ON_ERROR(iStatus);

    // close file from write mode
    LP2_RetVal = sl_FsClose(myFileHandle, NULL, NULL , 0);

    // open file in read mode to print to console
    LP2_RetVal = sl_FsOpen((unsigned char *) myFileName, FS_MODE_OPEN_READ, NULL, &myFileHandle);
    Offset = 0;
    LP2_RetVal = 1024;
    while(LP2_RetVal == 1024){
    	int resetBufCount;
    	LP2_RetVal = sl_FsRead(myFileHandle, Offset, (unsigned char *) g_cBsdBuf, 1024);
    	Offset += 1024;
    	UART_PRINT(g_cBsdBuf);
    	for(resetBufCount = 0; resetBufCount<1024; resetBufCount++){ //clear g_cBsdBuf
    		g_cBsdBuf[resetBufCount] = '\0';
    	}
    }

    return 0; //return 0 means success
}

//****************************************************************************
int BsdTcpClient(unsigned short usPort){
  return SUCCESS;
}

//****************************************************************************
//
//!  \brief Connecting to a WLAN Accesspoint
//!
//!   This function connects to the required AP (SSID_NAME) with Security
//!   parameters specified in te form of macros at the top of this file
//!
//!   \param[in]              None
//!
//!   \return     Status value
//!
//!   \warning    If the WLAN connection fails or we don't aquire an IP
//!            address, It will be stuck in this function forever.
//
//****************************************************************************
static long WlanConnect()
{
    SlSecParams_t secParams = {0};
    long lRetVal = 0;

    secParams.Key = (signed char*)SECURITY_KEY;
    secParams.KeyLen = strlen(SECURITY_KEY);
    secParams.Type = SECURITY_TYPE;

    lRetVal = sl_WlanConnect((signed char*)SSID_NAME, strlen(SSID_NAME), 0, &secParams, 0);
    ASSERT_ON_ERROR(lRetVal);

    /* Wait */
    while((!IS_CONNECTED(g_ulStatus)) || (!IS_IP_ACQUIRED(g_ulStatus)))
    {
        // Wait for WLAN Event
#ifndef SL_PLATFORM_MULTI_THREADED
        _SlNonOsMainLoopTask();
#endif
    }

    return SUCCESS;

}

//*****************************************************************************
//
//! Board Initialization & Configuration
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void
BoardInit(void)
{
/* In case of TI-RTOS vector table is initialize by OS itself */
#ifndef USE_TIRTOS
  //
  // Set vector table base
  //
#if defined(ccs) || defined (gcc)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
#endif
#if defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
#endif
#endif
    //
    // Enable Processor
    //
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);

    PRCMCC3200MCUInit();
}

//****************************************************************************
//                            MAIN FUNCTION
//****************************************************************************
void main()
{
    long lRetVal = -1;
    BoardInit(); //Board initialization
    UDMAInit(); //uDMA initialization
    PinMuxConfig(); //Configure pinmux
    InitTerm(); //Configure UART
    InitializeAppVariables();

    lRetVal = ConfigureSimpleLinkToDefaultState();
    if(lRetVal < 0){
      if (DEVICE_NOT_IN_STATION_MODE == lRetVal){
         UART_PRINT("Failed to configure the device in its default state \n\r");
      }
      LOOP_FOREVER();
    }

    UART_PRINT("Device is configured in default state \n\r");

    lRetVal = sl_Start(0, 0, 0);
    if (lRetVal < 0){
        UART_PRINT("Failed to start the device \n\r");
        LOOP_FOREVER();
    }

    UART_PRINT("Connecting to AP: %s...\r\n", SSID_NAME);

    lRetVal = WlanConnect(); //Connect to WLAN AP with parameters from common.h
    if(lRetVal < 0){
        UART_PRINT("Connection to AP failed \n\r");
        LOOP_FOREVER();
    }

    UART_PRINT("Connected to AP: %s \n\r", SSID_NAME);

    UART_PRINT("Device local IP: %d.%d.%d.%d\n\r\n\r",
                      SL_IPV4_BYTE(g_ulIpAddr,3),
                      SL_IPV4_BYTE(g_ulIpAddr,2),
                      SL_IPV4_BYTE(g_ulIpAddr,1),
                      SL_IPV4_BYTE(g_ulIpAddr,0));

#ifdef USER_INPUT_ENABLE
    lRetVal = UserInput();
    if(lRetVal < 0){
        ERR_PRINT(lRetVal);
        LOOP_FOREVER();
    }
#else
    lRetVal = BsdTcpClient(PORT_NUM);
    if(lRetVal < 0){
        UART_PRINT("TCP Client failed\n\r");
        LOOP_FOREVER();
    }

    lRetVal = BsdTcpServer(PORT_NUM);
    if(lRetVal < 0)
    {
        UART_PRINT("TCP Server failed\n\r");
        LOOP_FOREVER();
    }
#endif

    UART_PRINT("Exiting Application ...\n\r");
    lRetVal = sl_Stop(SL_STOP_TIMEOUT); //Turn off network processor
    while (1){
        _SlNonOsMainLoopTask();
    }
}
