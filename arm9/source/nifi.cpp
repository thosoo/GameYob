#include <nds.h>
#include <dswifi9.h>
#include "nifi.h"
#include "mmu.h"
#include "main.h"
#include "gameboy.h"
#include "gbcpu.h"
#include "console.h"

volatile int linkReceivedData = -1;
volatile int linkSendData;
volatile bool transferWaiting = false;
volatile bool transferReady = false;
volatile int nifiSendid = 0;

bool nifiEnabled = true;

volatile bool readyToSend = true;

int lastSendid = 0xff;

/**
 * @brief Timeout function for handling transfer waiting events.
 * 
 * This function is called when the transfer waiting timer expires. It sets the transferWaiting
 * flag to false and stops the timer.
 */
void transferWaitingTimeoutFunc()
{
    // Set the transferWaiting flag to false
    transferWaiting = false;

    // Stop the timer
    timerStop(2);
}


/**
 * @brief Packet handler function for processing received packets.
 * 
 * This function is called by the Wi-Fi library when a packet is received. It reads the packet,
 * checks if it is the correct type, processes the command and data, and handles the received
 * data accordingly.
 * 
 * Wifi_RxRawReadPacket:  Allows user code to read a packet from within the WifiPacketHandler function
 * long packetID:		a non-unique identifier which locates the packet specified in the internal buffer
 * long readlength:		number of bytes to read (actually reads (number+1)&~1 bytes)
 * unsigned short * data:	location for the data to be read into
 * 
 * @param packetID A non-unique identifier which locates the packet specified in the internal buffer.
 * @param readlength Number of bytes to read (actually reads (number+1)&~1 bytes).
 */
void packetHandler(int packetID, int readlength)
{
    // Buffer to hold the received packet data
    static char data[4096];

    // Read the packet data into the buffer
    Wifi_RxRawReadPacket(packetID, readlength, (unsigned short *)data);

    // Check if the packet is the correct type ('Y' and 'O' at positions 32 and 33)
    if (data[32] == 'Y' && data[33] == 'O')
    {
        // Extract command, value, and sendid from the received packet
        u8 command = data[34];
        u8 val = data[35];
        int sendid = *((int *)(data + 36));

        // Check if the last sendid is the same as the current sendid
        if (lastSendid == sendid)
        {
            if (!(ioRam[0x02] & 0x01))
            {
                nifiSendid--;
                sendPacketByte(56, linkSendData);
                nifiSendid++;
            }
            return;
        }
        lastSendid = sendid;

        // Process received data based on the command
        if (command == 55 || command == 56)
        {
            linkReceivedData = val;
        }

        // Switch statement to handle different commands
        switch (command)
        {
        // Command sent from "internal clock"
        case 55:
            if (ioRam[0x02] & 0x80)
            {
                // Falls through to case 56
            }
            else
            {
                printLog("Not ready!\n");
                transferWaiting = true;
                timerStart(2, ClockDivider_64, 10000, transferWaitingTimeoutFunc);
                break;
            }
            // Internal clock receives a response from external clock
        case 56:
            transferReady = true;
            cyclesToExecute = -1;
            nifiSendid++;
            break;
        default:
            // Unknown packet command
            // printLog("Unknown packet\n");
            break;
        }
    }
}


/* void enableNifi()
{
    Wifi_InitDefault(false);

    // Wifi_SetPromiscuousMode: Allows the DS to enter or leave a "promsicuous" mode, in which
    //   all data that can be received is forwarded to the arm9 for user processing.
    //   Best used with Wifi_RawSetPacketHandler, to allow user code to use the data
    //   (well, the lib won't use 'em, so they're just wasting CPU otherwise.)
    //  int enable:  0 to disable promiscuous mode, nonzero to engage
    Wifi_SetPromiscuousMode(1);

    // Wifi_EnableWifi: Instructs the ARM7 to go into a basic "active" mode, not actually
    //   associated to an AP, but actively receiving and potentially transmitting
    Wifi_EnableWifi();

    // Wifi_RawSetPacketHandler: Set a handler to process all raw incoming packets
    //  WifiPacketHandler wphfunc:  Pointer to packet handler (see WifiPacketHandler definition for more info)
    Wifi_RawSetPacketHandler(packetHandler);

    // Wifi_SetChannel: If the wifi system is not connected or connecting to an access point, instruct
    //   the chipset to change channel
    //  int channel: the channel to change to, in the range of 1-13
    Wifi_SetChannel(10);

    transferWaiting = false;
    nifiEnabled = true;
} */

/**
 * @brief Enables nifi and configures the Wi-Fi for communication.
 * 
 * This function initializes the Wi-Fi with default settings, sets it to promiscuous mode,
 * enables Wi-Fi, and sets a custom packet handler. It also sets the Wi-Fi channel and
 * enables nifi for communication.
 */
void enableNifi()
{
    // Initialize Wi-Fi with default settings
    Wifi_InitDefault(false);

    // Set promiscuous mode to receive all data for user processing
    // Wifi_SetPromiscuousMode: Allows the DS to enter or leave a "promsicuous" mode, in which
    //   all data that can be received is forwarded to the arm9 for user processing.
    //   Best used with Wifi_RawSetPacketHandler, to allow user code to use the data
    //   (well, the lib won't use 'em, so they're just wasting CPU otherwise.)
    //  int enable:  0 to disable promiscuous mode, nonzero to engage
    Wifi_SetPromiscuousMode(1);

    // Enable Wi-Fi in a basic "active" mode without being associated to an access point
    Wifi_EnableWifi();

    // Set a custom packet handler to process all raw incoming packets
    // Wifi_RawSetPacketHandler: Set a handler to process all raw incoming packets
    //  WifiPacketHandler wphfunc:  Pointer to packet handler (see WifiPacketHandler definition for more info)
    Wifi_RawSetPacketHandler(packetHandler);

    // Set the Wi-Fi channel (1-13) to use for communication
    // Wifi_SetChannel: If the wifi system is not connected or connecting to an access point, instruct
    //   the chipset to change channel
    //  int channel: the channel to change to, in the range of 1-13
    Wifi_SetChannel(10);

    // Set transferWaiting flag to false and enable nifi for communication
    transferWaiting = false;
    nifiEnabled = true;
}

/**
 * @brief Disables nifi and the Wi-Fi communication.
 * 
 * This function disables the Wi-Fi and sets the nifiEnabled flag to false.
 */
void disableNifi()
{
    // Disable Wi-Fi communication
    Wifi_DisableWifi();

    // Set nifiEnabled flag to false
    nifiEnabled = false;
}

/**
 * @brief Sends a packet byte over the network with the specified command and data.
 * 
 * This function sends a packet byte with a specific command and data if nifi is enabled. It
 * constructs a packet and sends it using the Wifi_RawTxFrame function. It also logs any
 * errors that occur during transmission.
 * 
 * @param command The command to be included in the packet.
 * @param data The data to be included in the packet.
 */
void sendPacketByte(u8 command, u8 data)
{
    // Check if nifi is enabled; if not, return immediately
    if (!nifiEnabled)
        return;

    // Buffer to hold the packet data
    unsigned char buffer[8];

    // Set the initial characters of the buffer to 'Y' and 'O'
    buffer[0] = 'Y';
    buffer[1] = 'O';

    // Set the command and data in the buffer
    buffer[2] = command;
    buffer[3] = data;

    // Set the nifiSendid in the buffer (little-endian)
    *((int *)(buffer + 4)) = nifiSendid;

    // Send the packet using Wifi_RawTxFrame and log any errors
    if (Wifi_RawTxFrame(8, 0x0014, (unsigned short *)buffer) != 0)
        printLog("Nifi send error\n");
}
