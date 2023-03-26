#include "sgb.h"
#include "console.h"
#include "mmu.h"
#include "gbgfx.h"
#include "gbcpu.h"

int sgbPacketLength;       // Number of packets to be transferred this command
int sgbPacketsTransferred; // Number of packets which have been transferred so far
int sgbPacketBit = -1;     // Next bit # to be sent in the packet. -1 if no packet is being transferred.
u8 sgbPacket[16];
u8 sgbCommand;

u8 *sgbPalettes = vram[1]; // Borrow vram bank 1. We don't need it in sgb mode.
u8 *sgbAttrFiles = vram[1] + 0x1000;
u8 sgbMap[20 * 18];

u8 numControllers;
u8 selectedController;
u8 buttonsChecked;

/**
 * @brief This struct defines the data for various different commands.
 * 
 * @param numDataSets The number of datasets.
 * 
 * @param attrBlock A struct containing 6 bytes of data and the number of data bytes.
 * 
 * @param attrChr A struct containing write style, x, and y values.
 */
struct CmdData
{
    int numDataSets;

    union
    {
        struct
        {
            u8 data[6];
            u8 dataBytes;
        } attrBlock;

        struct
        {
            u8 writeStyle;
            u8 x, y;
        } attrChr;
    };
} cmdData;

/**
 * @brief Initializes the Super Game Boy.
 * 
 * This function resets the variables associated with the Super Game Boy (SGB) mode, including the length of 
 * the packet, the number of controllers, the selected controller, the transferred packets, and the checked 
 * buttons. Additionally, the SGB map is reset using memset().
 * 
 * @return void
 */
void initSGB()
{
    sgbPacketLength = 0;
    numControllers = 1;
    selectedController = 0;
    sgbPacketBit = -1;
    sgbPacketsTransferred = 0;
    buttonsChecked = 0;

    memset(sgbMap, 0, 20 * 18);
}

/**
 * @brief Transfers data from the VRAM to a destination buffer.
 * 
 * This function transfers data from VRAM to a destination buffer.
 * The transfer is made based on the current video mode and scroll position.
 * The destination buffer is assumed to have enough space for the transfer.
 * 
 * @param dest Pointer to the destination buffer.
 * @return void
 */
void doVramTransfer(u8 *dest)
{
    // Set the base address of the background map based on the value in IO register 0x40.
    int map = 0x1800 + ((ioRam[0x40] >> 3) & 1) * 0x400;
    int index = 0;
    
    // Iterate through each tile in the background map and copy its data to the destination buffer.
    for (int y = 0; y < 18; y++)
    {
        for (int x = 0; x < 20; x++)
        {
            // Stop copying data if we reach the end of the destination buffer.
            if (index == 0x1000)
                return;

            // Get the index of the tile in the VRAM based on its position in the background map.
            int tile = vram[0][map + y * 32 + x];

            // Copy the tile data to the destination buffer.
            if (ioRam[0x40] & 0x10)
                memcpy(dest + index, vram[0] + tile * 16, 16);
            else
                memcpy(dest + index, vram[0] + 0x1000 + ((s8)tile) * 16, 16);

            // Increment the index to the next chunk of data in the destination buffer.
            index += 16;
        }
    }
}

/**
 * @brief Set the backdrop color of the screen.
 * 
 * The setBackdrop function sets the backdrop color of the screen. It takes a 16-bit value val as an argument, and sets the background palette, sprite palette, and off-map palette (used when the screen is disabled) to this value. If the loaded border type is SGB, it also sets the first background palette entry to this value.
 * The function first sets the off-map palette and the first background palette entry if the loaded border type is SGB. It then iterates over four color palettes, setting their first two entries to the backdrop color. This is done by writing the lower 8 bits of val to the first byte and the upper 8 bits of val to the second byte of each of the three palette entries in the palette.
 * 
 * @param val The 16-bit value representing the color to set as the backdrop.
 */
void setBackdrop(u16 val)
{
    if (loadedBorderType == BORDER_SGB)
        BG_PALETTE[0] = val;
    BG_PALETTE[15 * 16 + 1] = val; // "off map" palette, for when the screen is disabled
    for (int i = 0; i < 4; i++)
    {
        bgPaletteData[i * 8] = val & 0xff;
        bgPaletteData[i * 8 + 1] = val >> 8;
        sprPaletteData[i * 8] = val & 0xff;
        sprPaletteData[i * 8 + 1] = val >> 8;
        sprPaletteData[(i + 4) * 8] = val & 0xff;
        sprPaletteData[(i + 4) * 8 + 1] = val >> 8;
    }
}

/**
* @brief Loads an SGB attribute file at the given index
* This function loads an SGB attribute file at the given index. If the index is greater than 0x2c, it prints an error message and returns.
* The function then iterates over 20 x 18 / 4 (90) elements in the sgbAttrFiles array, and for each element, it extracts 4 pairs of 2-bit attribute dataand stores them in the sgbMap array.
* @param index The index of the attribute file to load
* @return void
*/
void sgbLoadAttrFile(int index)
{
    if (index > 0x2c)
    {
        printLog("Bad Attr %x\n", index);
        return;
    }
    // printLog("Load Attr %x\n", index);
    int src = index * 90;
    int dest = 0;
    for (int i = 0; i < 20 * 18 / 4; i++)
    {
        sgbMap[dest++] = (sgbAttrFiles[src] >> 6) & 3;
        sgbMap[dest++] = (sgbAttrFiles[src] >> 4) & 3;
        sgbMap[dest++] = (sgbAttrFiles[src] >> 2) & 3;
        sgbMap[dest++] = (sgbAttrFiles[src] >> 0) & 3;
        src++;
    }
}

/**
 * @brief Sets the SGB color palettes
 * Sets the SGB color palettes based on the SGB command type and packet data.
 * @param block The SGB palette block number to use. 
 * @return void
 */
void sgbPalXX(int block)
{
    int s1, s2;
    switch (sgbCommand)
    {
    case 0:
        s1 = 0;
        s2 = 1;
        break;
    case 1:
        s1 = 2;
        s2 = 3;
        break;
    case 2:
        s1 = 0;
        s2 = 3;
        break;
    case 3:
        s1 = 1;
        s2 = 2;
        break;
    default:
        return;
    }
    memcpy(bgPaletteData + s1 * 8 + 2, sgbPacket + 3, 6);
    memcpy(sprPaletteData + s1 * 8 + 2, sgbPacket + 3, 6);

    memcpy(bgPaletteData + s2 * 8 + 2, sgbPacket + 9, 6);
    memcpy(sprPaletteData + s2 * 8 + 2, sgbPacket + 9, 6);

    setBackdrop(sgbPacket[1] | sgbPacket[2] << 8);
}

/**
 * @brief Sets SGB's attribute block for map data.
 * This function sets the SGB's attribute block for map data. It takes in an integer block number,
 * which is used to indicate the start of a new block. The function then fills the data for the
 * attribute block by parsing the packet data. The attribute block's data sets are stored in cmdData
 * as an array of u8s. This function then uses the stored data to update the SGB map according to the
 * data sets.
 * @param block the integer block number. 
 * @return void.
 */
void sgbAttrBlock(int block)
{
    int pos;
    if (block == 0)
    {
        cmdData.attrBlock.dataBytes = 0;
        cmdData.numDataSets = sgbPacket[1];
        pos = 2;
    }
    else
        pos = 0;

    u8 *const data = cmdData.attrBlock.data;
    while (pos < 16 && cmdData.numDataSets > 0)
    {
        for (; cmdData.attrBlock.dataBytes < 6 && pos < 16; cmdData.attrBlock.dataBytes++, pos++)
        {
            data[cmdData.attrBlock.dataBytes] = sgbPacket[pos];
        }
        if (cmdData.attrBlock.dataBytes == 6)
        {
            int pIn = data[1] & 3;
            int pLine = (data[1] >> 2) & 3;
            int pOut = (data[1] >> 4) & 3;
            int x1 = data[2];
            int y1 = data[3];
            int x2 = data[4];
            int y2 = data[5];
            bool changeLine = data[0] & 2;
            if (!changeLine)
            {
                if ((data[0] & 7) == 1)
                {
                    changeLine = true;
                    pLine = pIn;
                }
                else if ((data[0] & 7) == 4)
                {
                    changeLine = true;
                    pLine = pOut;
                }
            }

            if (data[0] & 1)
            { // Inside block
                for (int x = x1 + 1; x < x2; x++)
                {
                    for (int y = y1 + 1; y < y2; y++)
                    {
                        sgbMap[y * 20 + x] = pIn;
                    }
                }
            }
            if (data[0] & 4)
            { // Outside block
                for (int x = 0; x < 20; x++)
                {
                    if (x < x1 || x > x2)
                    {
                        for (int y = 0; y < 18; y++)
                        {
                            if (y < y1 || y > y2)
                            {
                                sgbMap[y * 20 + x] = pOut;
                            }
                        }
                    }
                }
            }
            if (changeLine)
            { // Line surrounding block
                for (int x = x1; x <= x2; x++)
                {
                    sgbMap[y1 * 20 + x] = pLine;
                    sgbMap[y2 * 20 + x] = pLine;
                }
                for (int y = y1; y <= y2; y++)
                {
                    sgbMap[y * 20 + x1] = pLine;
                    sgbMap[y * 20 + x2] = pLine;
                }
            }

            cmdData.attrBlock.dataBytes = 0;
            cmdData.numDataSets--;
        }
    }
}

/**
 * Applies an SGB line attribute to the tile map.
 *
 * This function parses the line attribute data in the SGB packet and modifies
 * the tile map accordingly. Each line attribute specifies a line number and a
 * palette index, as well as whether the line is horizontal or vertical. For
 * horizontal lines, the entire row of tiles with the specified line number is
 * set to the specified palette index. For vertical lines, the entire column of
 * tiles with the specified line number is set to the specified palette index.
 *
 * @param block The index of the attribute block to process. If this is zero,
 *              the function initializes the attribute block and sets the
 *              number of data sets.
 */
void sgbAttrLin(int block)
{
    int index = 0;

    // If this is the first data set, initialize the attribute block
    if (block == 0)
    {
        cmdData.numDataSets = sgbPacket[1];
        index = 2;
    }

    // Process each data set in the attribute block
    while (cmdData.numDataSets > 0 && index < 16)
    {
        u8 dat = sgbPacket[index++]; // Get the next byte of data
        cmdData.numDataSets--;

        int line = dat & 0x1f;         // Extract the line number
        int pal = (dat >> 5) & 3;      // Extract the palette index

        if (dat & 0x80)
        { // If the line is horizontal
            // Set the palette index for each tile in the row
            for (int i = 0; i < 20; i++)
            {
                sgbMap[i + line * 20] = pal;
            }
        }
        else
        { // If the line is vertical
            // Set the palette index for each tile in the column
            for (int i = 0; i < 18; i++)
            {
                sgbMap[line + i * 20] = pal;
            }
        }
    }
}

/**
 * @brief Sets the color of the SGB screen based on the data in the SGB packet.
 *
 * This function handles setting the color of the SGB screen based on the data in the SGB packet.
 * It uses the data in the packet to set the color of different sections of the screen.
 *
 * @param block The block of data in the SGB packet to process.
 */
void sgbAttrDiv(int block)
{
    int p0 = (sgbPacket[1] >> 2) & 3;
    int p1 = (sgbPacket[1] >> 4) & 3;
    int p2 = (sgbPacket[1] >> 0) & 3;

    // Check if the data is for horizontal or vertical divisions
    if (sgbPacket[1] & 0x40)
    {
        // Horizontal divisions
        for (int y = 0; y < sgbPacket[2] && y < 18; y++)
        {
            for (int x = 0; x < 20; x++)
                sgbMap[y * 20 + x] = p0;
        }
        if (sgbPacket[2] < 18)
        {
            for (int x = 0; x < 20; x++)
                sgbMap[sgbPacket[2] * 20 + x] = p1;
            for (int y = sgbPacket[2] + 1; y < 18; y++)
            {
                for (int x = 0; x < 20; x++)
                    sgbMap[y * 20 + x] = p2;
            }
        }
    }
    else
    {
        // Vertical divisions
        for (int x = 0; x < sgbPacket[2] && x < 20; x++)
        {
            for (int y = 0; y < 18; y++)
                sgbMap[y * 20 + x] = p0;
        }
        if (sgbPacket[2] < 20)
        {
            for (int y = 0; y < 18; y++)
                sgbMap[y * 20 + sgbPacket[2]] = p1;
            for (int x = sgbPacket[2] + 1; x < 20; x++)
            {
                for (int y = 0; y < 18; y++)
                    sgbMap[y * 20 + x] = p2;
            }
        }
    }
}

/**
 * Sets the character data of the SGB map using the attribute information
 * contained in the SGB packet. The x and y position of the data is defined
 * by the x and y variables of cmdData.attrChr.
 *
 * @param block Index of the SGB packet to start reading attribute data from.
 */
void sgbAttrChr(int block)
{
    // Reference to x and y variables of cmdData.attrChr.
    u8 &x = cmdData.attrChr.x;
    u8 &y = cmdData.attrChr.y;

    int index = 0;
    if (block == 0)
    {
        // Number of attribute data sets.
        cmdData.numDataSets = sgbPacket[3] | (sgbPacket[4] << 8);
        // Write style for the attribute data.
        cmdData.attrChr.writeStyle = sgbPacket[5] & 1;
        // Set the x and y position for the first data set.
        x = (sgbPacket[1] >= 20 ? 19 : sgbPacket[1]);
        y = (sgbPacket[2] >= 18 ? 17 : sgbPacket[2]);

        // Index of the first attribute data set.
        index = 6 * 4;
    }

    // Iterate through all the attribute data sets and set the SGB map
    // character data based on the attribute information.
    while (cmdData.numDataSets != 0 && index < 16 * 4)
    {
        // Get the palette index for the current data set.
        int palIndex = (sgbPacket[index / 4] >> (6 - (index & 3) * 2)) & 3;

        // Set the character data of the SGB map using the palette index
        // and the x and y position.
        sgbMap[x + y * 20] = palIndex;

        // Update the x and y position based on the write style.
        if (cmdData.attrChr.writeStyle == 0)
        {
            x++;
            if (x == 20)
            {
                x = 0;
                y++;
                if (y == 18)
                    y = 0;
            }
        }
        else
        {
            y++;
            if (y == 18)
            {
                y = 0;
                x++;
                if (x == 20)
                    x = 0;
            }
        }
        index++;
        cmdData.numDataSets--;
    }
}

/**
 * @brief Sets the background and sprite palettes and loads attribute file if required.
 * 
 * This function sets the background and sprite palettes according to the data in the SGB packet. It reads
 * four 15-bit indices from the SGB packet, sets the corresponding background and sprite palettes, and 
 * updates the backdrop color as well. It also loads the attribute file if specified in the SGB packet. 
 * 
 * @param block An integer specifying the block in the SGB packet to read from.
 */
void sgbPalSet(int block)
{
    // Loop through each of the four palettes in the SGB packet and update the background and sprite palettes
    for (int i = 0; i < 4; i++)
    {
        // Extract the 15-bit palette ID from the SGB packet and copy the corresponding palette data into the
        // background and sprite palette data arrays.
        int paletteid = (sgbPacket[i * 2 + 1] | (sgbPacket[i * 2 + 2] << 8)) & 0x1ff;
        memcpy(bgPaletteData + i * 8 + 2, sgbPalettes + paletteid * 8 + 2, 6);
        memcpy(sprPaletteData + i * 8 + 2, sgbPalettes + paletteid * 8 + 2, 6);
    }

    // Extract the 15-bit palette ID for color 0 from the SGB packet and set the backdrop color accordingly.
    int color0Paletteid = (sgbPacket[1] | (sgbPacket[2] << 8)) & 0x1ff;
    setBackdrop(sgbPalettes[color0Paletteid * 8] | (sgbPalettes[color0Paletteid * 8 + 1] << 8));

    // Check the SGB packet for attribute file load request and load the file if necessary.
    if (sgbPacket[9] & 0x80)
    {
        sgbLoadAttrFile(sgbPacket[9] & 0x3f);
    }

    // Check the SGB packet for graphics mask request and set the mask if necessary.
    if (sgbPacket[9] & 0x40)
    {
        setGFXMask(0);
    }
}

/**
 * @brief Transfers SGB palettes to the VRAM
 * @param block SGB command packet block number
 */
void sgbPalTrn(int block)
{
    doVramTransfer(sgbPalettes);
}

/**
 * @brief Handles SGB sound data commands
 * @param block SGB command packet block number
 */
void sgbDataSnd(int block)
{
    // printLog("SND %.2x -> %.2x:%.2x%.2x\n", sgbPacket[4], sgbPacket[3], sgbPacket[2], sgbPacket[1]);
}

/**
 * @brief Handles SGB multiplayer request commands
 * @param block SGB command packet block number
 */
void sgbMltReq(int block)
{
    numControllers = (sgbPacket[1] & 3) + 1;
    if (numControllers > 1)
        selectedController = 1;
    else
        selectedController = 0;
}

/**
 * @brief Transfers SGB character data to the VRAM
 * @param block SGB command packet block number
 */
void sgbChrTrn(int block)
{
    u8 *data = (u8 *)malloc(0x1000);
    doVramTransfer(data);
    setSgbTiles(data, sgbPacket[1]);
    free(data);
}

/**
 * @brief Transfers a pattern table from the SGB to the Game Boy VRAM.
 *
 * @param block SGB block number.
 */
void sgbPctTrn(int block)
{
    u8 *data = (u8 *)malloc(0x1000); // Allocate memory for the pattern table data.
    doVramTransfer(data); // Transfer the pattern table data to the allocated memory.
    setSgbMap(data); // Set the pattern table to the VRAM.
    free(data); // Free the allocated memory.
}

/**
 * @brief Transfers attribute files to VRAM.
 * 
 * @param block The SGB block number.
 */
void sgbAttrTrn(int block)
{
    doVramTransfer(sgbAttrFiles);
}

/**
 * @brief Sets the SGB attributes.
 * 
 * @param block The block to set the attributes for.
 */
void sgbAttrSet(int block)
{
    // Load the attribute file based on the second byte of the packet.
    sgbLoadAttrFile(sgbPacket[1] & 0x3f);
    
    // If the 7th bit of the second byte is set, clear the GFX mask.
    if (sgbPacket[1] & 0x40)
        setGFXMask(0);
}

/**
 * @brief Sets the graphics mask for SGB.
 * 
 * This function sets the graphics mask for SGB by calling the setGFXMask function
 * with the value of the second byte of the sgbPacket ANDed with 3.
 *
 * @param block an integer representing the block.
 * 
 * @return void.
 */
void sgbMask(int block)
{
    setGFXMask(sgbPacket[1] & 3);
}

/**
 * @brief array of function pointers named sgbCommands
 * This code defines an array of function pointers named sgbCommands. Each element in the array is a pointer to a function that takes an integer parameter and returns no value (void).
 */
void (*sgbCommands[])(int) = {
    sgbPalXX, sgbPalXX, sgbPalXX, sgbPalXX, sgbAttrBlock, sgbAttrLin, sgbAttrDiv, sgbAttrChr,
    NULL, NULL, sgbPalSet, sgbPalTrn, NULL, NULL, NULL, sgbDataSnd,
    NULL, sgbMltReq, NULL, sgbChrTrn, sgbPctTrn, sgbAttrTrn, sgbAttrSet, sgbMask,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

/**
 * @brief Handle SGB packets received over P1 port
 * This function handles SGB packets received over the P1 port. It assembles
 * packets from individual bits and executes the corresponding command when
 * a complete packet has been received. It also handles controller selection
 * when the P1 port is not being used for packet transfer. 
 * @param val The value of the incoming packet.
 */
void sgbHandleP1(u8 val)
{
    if ((val & 0x30) == 0)
    {
        // Start packet transfer
        sgbPacketBit = 0;
        ioRam[0x00] = 0xcf;
        return;
    }

    if (sgbPacketBit != -1)
    {
        // Continue packet transfer
        u8 oldVal = ioRam[0x00];
        ioRam[0x00] = val;

        int shift = sgbPacketBit % 8;
        int byte = (sgbPacketBit / 8) % 16;

        // If the current bit is the first bit of a byte, clear it first.
        if (shift == 0)
            sgbPacket[byte] = 0;

        int bit;
        if ((oldVal & 0x30) == 0 && (val & 0x30) != 0x30)
        { // A bit of speculation here. Fixes Castlevania.
            // If a packet transfer is interrupted, stop it.
            sgbPacketBit = -1;
            return;
        }
        if (!(val & 0x10))
            bit = 0;
        else if (!(val & 0x20))
            bit = 1;
        else
            return;

        // Add the current bit to the packet.
        sgbPacket[byte] |= bit << shift;
        sgbPacketBit++;

        // If the packet is complete, execute the corresponding command.
        if (sgbPacketBit == 128)
        {
            if (sgbPacketsTransferred == 0)
            {
                sgbCommand = sgbPacket[0] / 8;
                sgbPacketLength = sgbPacket[0] & 7;
                // printLog("CMD %x\n", sgbCommand);
            }
            if (sgbCommands[sgbCommand] != 0)
                sgbCommands[sgbCommand](sgbPacketsTransferred);

            sgbPacketBit = -1;
            sgbPacketsTransferred++;

            // If all packets for a command have been transferred, reset the counters.
            if (sgbPacketsTransferred == sgbPacketLength)
            {
                sgbPacketLength = 0;
                sgbPacketsTransferred = 0;
            }
        }
    }
    else
    {
        // Handle controller input
        if ((val & 0x30) == 0x30)
        {
            if (buttonsChecked == 3)
            {
                selectedController++;
                if (selectedController >= numControllers)
                    selectedController = 0;
                buttonsChecked = 0;
            }
            ioRam[0x00] = 0xff - selectedController;
        }
        else
        {
            ioRam[0x00] = val | 0xcf;
            if ((val & 0x30) == 0x10)
                buttonsChecked |= 1;
            else if ((val & 0x30) == 0x20)
                buttonsChecked |= 2;
        }
    }
}