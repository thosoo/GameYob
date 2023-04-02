#include <stdio.h>
#include <cstdlib>
#include "mmu.h"
#include "gbcpu.h"
#include "gameboy.h"
#include "gbgfx.h"
#include "gbsnd.h"
#include "inputhelper.h"
#include "main.h"
#include "nifi.h"
#include "sgb.h"
#include "console.h"
#include "gbs.h"
#ifdef DS
#include <nds.h>
#endif

extern time_t rawTime;

#define refreshVramBank() \
    { \
        memory[0x8] = vram[vramBank]; \
        memory[0x9] = vram[vramBank] + 0x1000; \
    }
#define refreshWramBank() \
    { \
        memory[0xd] = wram[wramBank]; \
    }

bool hasRumble;
int rumbleStrength;
int rumbleInserted = 0;
bool rumbleValue = 0;
bool lastRumbleValue = 0;

clockStruct gbClock;

int numRomBanks = 0;
int numRamBanks = 0;

int resultantGBMode;

u8 bios[0x900];
bool biosExists = false;
int biosEnabled;
bool biosOn = false;

u8 buttonsPressed = 0xff;

u8 *memory[0x10]
#ifdef DS
    DTCM_BSS
#endif
    ;
u8 vram[2][0x2000];
u8 *externRam = NULL;
u8 wram[8][0x1000];

u8 highram[0x1000];
u8 *const hram = highram + 0xe00;
u8 *const ioRam = hram + 0x100;

int wramBank;
int vramBank;

int MBC;
int memoryModel;

bool rockmanMapper;

int romBank;
int currentRamBank;

u16 dmaSource;
u16 dmaDest;
u16 dmaLength;
int dmaMode;

// Autosaving stuff
bool saveModified = false;
bool dirtySectors[MAX_SRAM_SIZE / AUTOSAVE_SECTOR_SIZE];
int numSaveWrites = 0;
bool autosaveStarted = false;

/* MBC flags */
bool ramEnabled;

u8 HuC3Mode;
u8 HuC3Value;
u8 HuC3Shift;

typedef void (*mbcWrite)(u16, u8);
typedef u8 (*mbcRead)(u16);

mbcWrite writeFunc;
mbcRead readFunc;

/**
 * @brief Refresh the ROM bank.
 *
 * This function updates the ROM bank by setting the memory pointers
 * to the specified bank in the ROM. If the specified bank is out of range,
 * it logs an error message.
 *
 * @param bank The ROM bank number to switch to.
 */
void refreshRomBank(int bank)
{
    // Check if the specified bank is within the range of available ROM banks
    if (bank < numRomBanks)
    {
        // Update the current ROM bank
        romBank = bank;

        // Load the new ROM bank
        loadRomBank();

        // Update the memory pointers for the new ROM bank
        memory[0x4] = romSlot1;
        memory[0x5] = romSlot1 + 0x1000;
        memory[0x6] = romSlot1 + 0x2000;
        memory[0x7] = romSlot1 + 0x3000;
    }
    else
    {
        // Log an error message if the specified bank is out of range
        printLog("Tried to access bank %x@n", bank);
    }
}


/**
 * @brief Refresh the RAM bank.
 *
 * This function updates the RAM bank by setting the memory pointers
 * to the specified bank in the external RAM.
 *
 * @param bank The RAM bank number to switch to.
 */
void refreshRamBank(int bank)
{
    // Check if the specified bank is within the range of available RAM banks
    if (bank < numRamBanks)
    {
        // Update the current RAM bank
        currentRamBank = bank;

        // Update the memory pointers for the new RAM bank
        memory[0xa] = externRam + currentRamBank * 0x2000;
        memory[0xb] = externRam + currentRamBank * 0x2000 + 0x1000;
    }
}


/**
 * @brief Handle HuC3 specific commands.
 * 
 * This function processes HuC3 specific commands, such as reading the clock, latching,
 * and other HuC3 specific operations.
 * 
 * @param cmd The 8-bit command to be processed.
 */
void handleHuC3Command(u8 cmd)
{
    // Determine the command based on the upper 4 bits
    switch (cmd & 0xf0)
    {
    case 0x10: // Read clock
        if (HuC3Shift > 24)
            break;

        switch (HuC3Shift)
        {
        case 0:
        case 4:
        case 8: // Minutes
            HuC3Value = (gbClock.huc3.m >> HuC3Shift) & 0xf;
            break;
        case 12:
        case 16:
        case 20: // Days
            HuC3Value = (gbClock.huc3.d >> (HuC3Shift - 12)) & 0xf;
            break;
        case 24: // Year
            HuC3Value = gbClock.huc3.y & 0xf;
            break;
        }
        HuC3Shift += 4;
        break;
    case 0x40:
        switch (cmd & 0xf)
        {
        case 0:
        case 4:
        case 7:
            HuC3Shift = 0;
            break;
        }

        // Latch the clock
        latchClock();
        break;
    case 0x50:
        // No operation
        break;
    case 0x60:
        // Set HuC3Value to 1
        HuC3Value = 1;
        break;
    default:
        // Unhandled HuC3 command
        printLog("undandled HuC3 cmd %02x@n", cmd);
    }
}


/* MBC read handlers */

/**
 * @brief HUC3 memory read function.
 * 
 * This function handles memory reads for HUC3, handling address ranges and operations
 * according to the HUC3 specification. It reads data from the HuC3 registers or memory
 * depending on the HuC3 mode.
 * 
 * @param addr The 16-bit address to read from.
 * @return The 8-bit value read from the specified address.
 */
u8 h3r(u16 addr)
{
    // Determine the HuC3 register or memory to read based on the HuC3 mode
    switch (HuC3Mode)
    {
    case 0xc:
        return HuC3Value;
    case 0xb:
    case 0xd:
        /* Return 1 as a fixed value, needed for some games to
         * boot, the meaning is unknown. */
        return 1;
    }
    // Check if RAM is enabled
    return (ramEnabled) ? memory[addr >> 12][addr & 0xfff] : 0xff;
}


/**
 * @brief MBC3 memory read function.
 * 
 * This function handles memory reads for MBC3, handling address ranges and operations
 * according to the MBC3 specification. It reads the data from RTC registers or
 * memory depending on the current RAM bank.
 * 
 * @param addr The 16-bit address to read from.
 * @return The 8-bit value read from the specified address.
 */
u8 m3r(u16 addr)
{
    // Check if RAM is enabled
    if (!ramEnabled)
        return 0xff;

    // Determine the RTC register or memory to read based on the current RAM bank
    switch (currentRamBank)
    {
    case 0x8: // RTC seconds register
        return gbClock.mbc3.s;
    case 0x9: // RTC minutes register
        return gbClock.mbc3.m;
    case 0xA: // RTC hours register
        return gbClock.mbc3.h;
    case 0xB: // RTC lower 8 bits of day counter
        return gbClock.mbc3.d & 0xff;
    case 0xC: // RTC control register
        return gbClock.mbc3.ctrl;
    default: // Not an RTC register, read from memory
        return memory[addr >> 12][addr & 0xfff];
    }
}


const mbcRead mbcReads[] = {
    NULL, NULL, NULL, m3r, NULL, NULL, NULL, h3r, NULL};

/**
 * @brief Write a value to SRAM (Save RAM).
 * 
 * This function writes the provided value to the specified address in SRAM. If the value
 * differs from the current value in externRam, it updates externRam and sets the saveModified
 * flag to true if auto-saving is enabled. It also marks the corresponding sector as dirty.
 * 
 * @param addr The 16-bit address in SRAM to write to.
 * @param val The 8-bit value to be written.
 */
void writeSram(u16 addr, u8 val)
{
    // Calculate the position in the externRam array
    int pos = addr + currentRamBank * 0x2000;

    // Check if the new value is different from the current value in externRam
    if (externRam[pos] != val)
    {
        // Update the value in externRam
        externRam[pos] = val;

        // Check if auto-saving is enabled
        if (autoSavingEnabled)
        {
            // Set the saveModified flag to true
            saveModified = true;

            // Mark the corresponding sector as dirty
            dirtySectors[pos / AUTOSAVE_SECTOR_SIZE] = true;

            // Increment the number of save writes
            numSaveWrites++;
        }
    }
}


/**
 * @brief Write the clock structure to the save file.
 * 
 * This function writes the Game Boy clock structure (gbClock) to the save file if auto-saving is
 * enabled. It sets the saveModified flag to true upon completion.
 */
void writeClockStruct()
{
    // Check if auto-saving is enabled
    if (autoSavingEnabled)
    {
        // Move the file position indicator to the desired offset
        fseek(saveFile, numRamBanks * 0x2000, SEEK_SET);

        // Write the gbClock structure to the save file
        fwrite(&gbClock, 1, sizeof(gbClock), saveFile);

        // Set the saveModified flag to true
        saveModified = true;
    }
}


/* MBC Write handlers */

/**
 * @brief MBC0 memory write function.
 * 
 * This function handles memory writes for MBC0 (no MBC), handling address ranges and operations
 * according to the MBC0 specification.
 * 
 * @param addr The 16-bit address to write to.
 * @param val The 8-bit value to be written.
 */
void m0w(u16 addr, u8 val)
{
    // Determine the address range and perform corresponding actions
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // No operation for this address range
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        // No operation for this address range
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        // No operation for this address range
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // No operation for this address range
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // Write to SRAM if there are RAM banks available
        if (numRamBanks)
            writeSram(addr & 0x1fff, val);
        break;
    }
}


/**
 * @brief MBC2 memory write function.
 * 
 * This function handles memory writes for MBC2, handling address ranges and operations
 * according to the MBC2 specification.
 * 
 * @param addr The 16-bit address to write to.
 * @param val The 8-bit value to be written.
 */
void m2w(u16 addr, u8 val)
{
    // Determine the address range and perform corresponding actions
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // Enable or disable RAM based on the value
        ramEnabled = ((val & 0xf) == 0xa);
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        // Refresh ROM bank with the provided value (set to 1 if value is 0)
        refreshRomBank((val) ? val : 1);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        // No operation for this address range
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // No operation for this address range
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // Write to SRAM if RAM is enabled and there are RAM banks available
        if (ramEnabled && numRamBanks)
            writeSram(addr & 0x1fff, val & 0xf);
        break;
    }
}

/**
 * Write behavior of the MBC3 memory controller used in Game Boy cartridges.
 *
 * @param addr The address to write to.
 * @param val The value to write.
 *
 * If the address is in the range 0x0000-0x1FFF, the function checks if the lower 4 bits of
 * the value are equal to 0xA, which would enable the external RAM. If not, it disables it.
 *
 * If the address is in the range 0x2000-0x3FFF, the function updates the ROM bank selected
 * by updating the bank value with the lower 7 bits of the value. If the bank value is 0, the
 * bank is set to 1 instead.
 *
 * If the address is in the range 0x4000-0x5FFF, the function either selects a RAM bank or an
 * RTC register based on the value written. If the value is between 0x00 and 0x03, it selects
 * a RAM bank by updating the current RAM bank value. If the value is between 0x08 and 0x0C, it
 * selects an RTC register. RTC registers are updated when a write occurs to one of these registers.
 *
 * If the address is in the range 0x6000-0x7FFF, the function checks if the value is non-zero,
 * in which case it latches the current time into the RTC.
 *
 * If the address is in the range 0xA000-0xBFFF, and RAM is enabled, the function checks the
 * current RAM bank and writes to the corresponding RTC register or external RAM bank. If the
 * current RAM bank is not an RTC register, the value is written to the selected RAM bank.
 */
void m3w(u16 addr, u8 val)
{
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        ramEnabled = ((val & 0xf) == 0xa);
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        val &= 0x7f;
        refreshRomBank((val) ? val : 1);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        /* The RTC register is selected by writing values 0x8-0xc, ram banks
         * are selected by values 0x0-0x3 */
        if (val <= 0x3)
            refreshRamBank(val);
        else if (val >= 8 && val <= 0xc)
            currentRamBank = val;
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        if (val)
            latchClock();
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        if (!ramEnabled)
            break;

        switch (currentRamBank)
        { // Check for RTC register
        case 0x8:
            if (gbClock.mbc3.s != val)
            {
                gbClock.mbc3.s = val;
                writeClockStruct();
            }
            return;
        case 0x9:
            if (gbClock.mbc3.m != val)
            {
                gbClock.mbc3.m = val;
                writeClockStruct();
            }
            return;
        case 0xA:
            if (gbClock.mbc3.h != val)
            {
                gbClock.mbc3.h = val;
                writeClockStruct();
            }
            return;
        case 0xB:
            if ((gbClock.mbc3.d & 0xff) != val)
            {
                gbClock.mbc3.d &= 0x100;
                gbClock.mbc3.d |= val;
                writeClockStruct();
            }
            return;
        case 0xC:
            if (gbClock.mbc3.ctrl != val)
            {
                gbClock.mbc3.d &= 0xFF;
                gbClock.mbc3.d |= (val & 1) << 8;
                gbClock.mbc3.ctrl = val;
                writeClockStruct();
            }
            return;
        default: // Not an RTC register
            if (numRamBanks)
                writeSram(addr & 0x1fff, val);
        }
        break;
    }
}

/**
 * @brief Handles writes to memory locations in the MBC1 controller's address range.
 *
 * This function handles writes to memory locations in the MBC1 controller's address range (0x0000-0xBFFF) by interpreting the value of the `addr` parameter and performing the appropriate action. If the address is in the range 0x0000-0x1FFF, the RAM enable status is set based on the value of `val`. If the address is in the range 0x2000-0x3FFF, the current ROM bank is set to the lower 5 bits of `val` in normal mode, or to a bank calculated based on the value of `val` and the current bank in Rockman8 mapper mode. If the address is in the range 0x4000-0x5FFF, the lower 2 bits of `val` are used to set the current RAM bank in RAM mode, or to set the upper 2 bits of the current ROM bank in normal mode. If the address is in the range 0x6000-0x7FFF, the memory model is set to the lowest bit of `val`. If the address is in the range 0xA000-0xBFFF and RAM is enabled with available RAM banks, the `writeSram()` function is called with `addr & 0x1fff` and `val` as parameters.
 *
 * @param[in] addr The address to write to.
 * @param[in] val The value to write.
 * @return void
 */
void m1w(u16 addr, u8 val)
{
    int newBank;

    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // Set RAM enabled status
        ramEnabled = ((val & 0xf) == 0xa);
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        val &= 0x1f;
        if (rockmanMapper)
            newBank = ((val > 0xf) ? val - 8 : val);
        else
            newBank = (romBank & 0xe0) | val;
        // Set current ROM bank to newBank if it is non-zero, otherwise set to 1
        refreshRomBank((newBank) ? newBank : 1);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        val &= 3;
        // ROM mode
        if (memoryModel == 0)
        {
            newBank = (romBank & 0x1F) | (val << 5);
            // Set current ROM bank to newBank if it is non-zero, otherwise set to 1
            refreshRomBank((newBank) ? newBank : 1);
        }
        // RAM mode
        else
            // Set current RAM bank to lower 2 bits of val
            refreshRamBank(val);
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // Set memory model to lowest bit of val
        memoryModel = val & 1;
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // If RAM is enabled and there are RAM banks available, call writeSram()
        if (ramEnabled && numRamBanks)
            writeSram(addr & 0x1fff, val);
        break;
    }
}

/**
 * @brief Handles writes to memory locations in the HuC1 controller's address range.
 *
 * This function handles writes to memory locations in the HuC1 controller's address range (0x0000-0xBFFF) by interpreting the value of the `addr` parameter and performing the appropriate action. If the address is in the range 0x0000-0x1FFF, the RAM enable status is set based on the value of `val`. If the address is in the range 0x2000-0x3FFF, the current ROM bank is set to the lower 6 bits of `val`. If the address is in the range 0x4000-0x5FFF, the current bank is set to the lower 2 bits of `val` in ROM mode, or the lower 2 bits of `val` are used to set the current RAM bank in RAM mode. If the address is in the range 0x6000-0x7FFF, the memory model is set to the lowest bit of `val`. If the address is in the range 0xA000-0xBFFF and RAM is enabled with available RAM banks, the `writeSram()` function is called with `addr & 0x1fff` and `val` as parameters.
 *
 * @param[in] addr The address to write to.
 * @param[in] val The value to write.
 * @return void
 */
void h1w(u16 addr, u8 val)
{
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // Set RAM enabled status
        ramEnabled = ((val & 0xf) == 0xa);
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        // Set current ROM bank to lower 6 bits of val
        refreshRomBank(val & 0x3f);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        val &= 3;
        // ROM mode
        if (memoryModel == 0)
            refreshRomBank(val);
        // RAM mode
        else
            refreshRamBank(val);
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // Set memory model to lowest bit of val
        memoryModel = val & 1;
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // If RAM is enabled and there are RAM banks available, call writeSram()
        if (ramEnabled && numRamBanks)
            writeSram(addr & 0x1fff, val);
        break;
    }
}

/**
 * @brief Handles writes to memory locations in the MBC5 controller's address range.
 *
 * This function handles writes to memory locations in the MBC5 controller's address range (0x0000-0xBFFF) by interpreting the value of the `addr` parameter and performing the appropriate action. If the address is in the range 0x0000-0x1FFF, the RAM enable status is set based on the value of `val`. If the address is in the range 0x2000-0x3FFF, the current ROM bank is set to the lower 8 bits of `val` with the upper bit being preserved. If the address is in the range 0x4000-0x5FFF, the current RAM bank is set to the lower 4 bits of `val`, and if the game has a rumble motor, the 4th bit of the value written is used to trigger the motor. If the address is in the range 0xA000-0xBFFF and RAM is enabled with available RAM banks, the `writeSram()` function is called with `addr & 0x1fff` and `val` as parameters.
 *
 * @param[in] addr The address to write to.
 * @param[in] val The value to write.
 * @return void
 */
void m5w(u16 addr, u8 val)
{
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // Set RAM enabled status
        ramEnabled = ((val & 0xf) == 0xa);
        break;
    case 0x2: /* 2000 - 3fff */
        // Set lower 8 bits of ROM bank
        refreshRomBank((romBank & 0x100) | val);
        break;
    case 0x3:
        // Set upper bit of ROM bank
        refreshRomBank((romBank & 0xff) | (val & 1) << 8);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        // Set current RAM bank and trigger rumble motor if applicable
        val &= 0xf;
        if (hasRumble)
        {
            if (rumbleStrength)
            {
                if (rumbleInserted)
                {
                    rumbleValue = (val & 0x8) ? 1 : 0;
                    if (rumbleValue != lastRumbleValue)
                    {
                        doRumble(rumbleValue);
                        lastRumbleValue = rumbleValue;
                    }
                }
            }
            val &= 0x07;
        }
        refreshRamBank(val);
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // Do nothing
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // If RAM is enabled and there are RAM banks available, call writeSram()
        if (ramEnabled && numRamBanks)
            writeSram(addr & 0x1fff, val);
        break;
    }
}


/**
 * @brief Handles writes to memory locations in the HuC3 controller's address range.
 *
 * This function handles writes to memory locations in the HuC3 controller's address range (0x0000-0xBFFF) by interpreting the value of the `addr` parameter and performing the appropriate action. If the address is in the range 0x0000-0x1FFF or 0x2000-0x3FFF, the RAM enable status and HuC3 mode are set based on the value of `val`. If the address is in the range 0x4000-0x5FFF, the current RAM bank is set based on the value of `val`. If the address is in the range 0xA000-0xBFFF, the function switches on the value of `HuC3Mode` to determine the appropriate action to take. If `HuC3Mode` is 0xB, the `handleHuC3Command()` function is called with `val` as a parameter. If `HuC3Mode` is 0xC, 0xD, or 0xE, no action is taken. Otherwise, if RAM is enabled and there are RAM banks available, the `writeSram()` function is called with `addr & 0x1fff` and `val` as parameters.
 *
 * @param[in] addr The address to write to.
 * @param[in] val The value to write.
 * @return void
 */
void h3w(u16 addr, u8 val)
{
    switch (addr >> 12)
    {
    case 0x0: /* 0000 - 1fff */
    case 0x1:
        // Set RAM enabled status and HuC3 mode
        ramEnabled = ((val & 0xf) == 0xa);
        HuC3Mode = val;
        break;
    case 0x2: /* 2000 - 3fff */
    case 0x3:
        // Set current ROM bank
        refreshRomBank((val) ? val : 1);
        break;
    case 0x4: /* 4000 - 5fff */
    case 0x5:
        // Set current RAM bank
        refreshRamBank(val & 0xf);
        break;
    case 0x6: /* 6000 - 7fff */
    case 0x7:
        // Do nothing
        break;
    case 0xa: /* a000 - bfff */
    case 0xb:
        // Switch on HuC3 mode to determine appropriate action
        switch (HuC3Mode)
        {
        case 0xb:
            // Call handleHuC3Command() with val as parameter
            handleHuC3Command(val);
            break;
        case 0xc:
        case 0xd:
        case 0xe:
            // Do nothing
            break;
        default:
            // If RAM is enabled and there are RAM banks available, call writeSram()
            if (ramEnabled && numRamBanks)
                writeSram(addr & 0x1fff, val);
        }
        break;
    }
}


const mbcWrite mbcWrites[] = {
    m0w, m1w, m2w, m3w, NULL, m5w, NULL, h3w, h1w};

/**
 * @brief Initializes the Memory Management Unit (MMU) by setting initial values for various system variables, mapping the game's memory, and initializing the IO registers and sprite memory.
 *
 * This function initializes the Memory Management Unit (MMU) by setting initial values for various system variables, including the work RAM bank, VRAM bank, ROM bank, current RAM bank, memory model, DMA source and destination addresses, DMA length, DMA mode, SGB status, HuC3 value and shift, Rockman mapper status, MBC read and write functions, BIOS status, and RAM enabled status. It then calls the `mapMemory()` function to map the game's memory to the appropriate locations based on the current state of the system. The function then initializes the work RAM and high RAM by setting all bytes to 0, initializes the IO registers and sprite memory by writing default values to the relevant memory locations, and sets the dirty sectors array to all 0's.
 *
 * @return void
 */
void initMMU()
{
    // Initialize system variables
    wramBank = 1;
    vramBank = 0;
    romBank = 1;
    currentRamBank = 0;
    memoryModel = 0;
    dmaSource = 0;
    dmaDest = 0;
    dmaLength = 0;
    dmaMode = 0;
    initSGB();
    ramEnabled = false;
    HuC3Value = 0;
    HuC3Shift = 0;
    rockmanMapper = !strcmp(getRomTitle(), "ROCKMAN 99");
    readFunc = mbcReads[MBC];
    writeFunc = mbcWrites[MBC];
    biosOn = false;

    // Enable BIOS if it exists and is enabled
    if (biosExists && !probingForBorder && !gbsMode)
    {
        if (biosEnabled == 2)
            biosOn = true;
        else if (biosEnabled == 1 && resultantGBMode == 0)
            biosOn = true;
    }

    // Map game's memory to appropriate locations
    mapMemory();

    // Initialize work RAM and high RAM
    for (int i = 0; i < 8; i++)
        memset(wram[i], 0, 0x1000);
    memset(hram, 0, 0x200);

    // Initialize IO registers and sprite memory
    writeIO(0x02, 0x00);
    writeIO(0x05, 0x00);
    writeIO(0x06, 0x00);
    writeIO(0x07, 0x00);
    writeIO(0x40, 0x91);
    writeIO(0x42, 0x00);
    writeIO(0x43, 0x00);
    writeIO(0x45, 0x00);
    writeIO(0x47, 0xfc);
    writeIO(0x48, 0xff);
    writeIO(0x49, 0xff);
    writeIO(0x4a, 0x00);
    writeIO(0x4b, 0x00);
    writeIO(0xff, 0x00);
    ioRam[0x55] = 0xff;

    // Initialize dirty sectors array
    memset(dirtySectors, 0, sizeof(dirtySectors));
}


/**
 * @brief Maps the game's memory to the appropriate locations based on the current state of the system.
 *
 * This function maps the game's memory to the appropriate locations based on the current state of the system. If the BIOS is enabled, the first 0x100 bytes of memory are mapped to the BIOS. Otherwise, the first 0x100 bytes of memory are mapped to the first ROM bank. The next three memory locations (0x100 - 0x300) are mapped to the second, third, and fourth banks of the ROM, respectively. The ROM bank is refreshed to the current ROM bank value using the `refreshRomBank()` function. The RAM bank is refreshed to the current RAM bank value using the `refreshRamBank()` function. The VRAM bank is refreshed using the `refreshVramBank()` function. The first 0x1000 bytes of work RAM are mapped to memory location 0xC. The work RAM bank is refreshed using the `refreshWramBank()` function. Memory locations 0xE and 0xF are mapped to the first 0x100 bytes of work RAM and the high RAM, respectively. Finally, the DMA source and destination addresses are calculated based on the values in the I/O RAM registers, and are stored in the `dmaSource` and `dmaDest` variables, respectively.
 *
 * @return void
 */
void mapMemory()
{
    // Map first 0x100 bytes of memory to BIOS or first ROM bank
    if (biosOn)
        memory[0x0] = bios;
    else
        memory[0x0] = romSlot0;

    // Map ROM banks 1-3 to memory locations 0x100 - 0x300
    memory[0x1] = romSlot0 + 0x1000;
    memory[0x2] = romSlot0 + 0x2000;
    memory[0x3] = romSlot0 + 0x3000;

    // Refresh ROM bank, RAM bank, and VRAM bank
    refreshRomBank(romBank);
    refreshRamBank(currentRamBank);
    refreshVramBank();

    // Map first 0x1000 bytes of work RAM to memory location 0xC
    memory[0xc] = wram[0];

    // Refresh work RAM bank
    refreshWramBank();

    // Map memory locations 0xE and 0xF to first 0x100 bytes of work RAM and high RAM, respectively
    memory[0xe] = wram[0];
    memory[0xf] = highram;

    // Calculate DMA source and destination addresses based on I/O RAM values
    dmaSource = (ioRam[0x51] << 8) | (ioRam[0x52]);
    dmaSource &= 0xFFF0;
    dmaDest = (ioRam[0x53] << 8) | (ioRam[0x54]);
    dmaDest &= 0x1FF0;
}


/* Increment y if x is greater than val */
#define OVERFLOW(x, val, y) \
    do \
    { \
        while (x >= val) \
        { \
            x -= val; \
            y++; \
        } \
    } while (0)

/**
 * @brief Updates the Game Boy's clock using the current system time.
 * This function calculates the time difference between the last update and
 * the current system time, and uses it to update the Game Boy's clock. The
 * updated clock values are stored in the appropriate fields of the gbClock
 * structure.
 * @note This function assumes that rawTime is set to the current system time (in seconds) prior to its execution.
 * @param None
 * @return None
*/
void latchClock()
{
    // +2h, the same as lameboy
    time_t now = rawTime - 120 * 60;
    time_t difference = now - gbClock.last;
    struct tm *lt = gmtime((const time_t *)&difference);

    switch (MBC)
    {
    case MBC3:
        gbClock.mbc3.s += lt->tm_sec;
        OVERFLOW(gbClock.mbc3.s, 60, gbClock.mbc3.m);
        gbClock.mbc3.m += lt->tm_min;
        OVERFLOW(gbClock.mbc3.m, 60, gbClock.mbc3.h);
        gbClock.mbc3.h += lt->tm_hour;
        OVERFLOW(gbClock.mbc3.h, 24, gbClock.mbc3.d);
        gbClock.mbc3.d += lt->tm_yday;
        /* Overflow! */
        if (gbClock.mbc3.d > 0x1FF)
        {
            /* Set the carry bit */
            gbClock.mbc3.ctrl |= 0x80;
            gbClock.mbc3.d -= 0x200;
        }
        /* The 9th bit of the day register is in the control register */
        gbClock.mbc3.ctrl &= ~1;
        gbClock.mbc3.ctrl |= (gbClock.mbc3.d > 0xff);
        break;
    case HUC3:
        gbClock.huc3.m += lt->tm_min;
        OVERFLOW(gbClock.huc3.m, 60 * 24, gbClock.huc3.d);
        gbClock.huc3.d += lt->tm_yday;
        OVERFLOW(gbClock.huc3.d, 365, gbClock.huc3.y);
        gbClock.huc3.y += lt->tm_year - 70;
        break;
    }

    gbClock.last = now;
}

#ifdef DS
u8 readMemory(u16 addr) ITCM_CODE;
#endif

/**
 * This function reads a single byte from the specified memory address. If the SPEEDHAX flag is not defined, the function checks if
 * the address is within the range of a specific memory area (0x8000 - 0xFFFF) and takes appropriate action based on the area. If the
 * address is not in a special area, the function reads the byte from the memory array at the specified address.
 *
 * @param addr The memory address to read from.
 * @return The byte value read from memory.
 */
u8 readMemory(u16 addr)
{
#ifndef SPEEDHAX
    int area = addr >> 13;

    // Check if the address is in the special range of 0x8000-0xFFFF
    if (area & 0x04)
    {
        // Address is in special range
        if (area == 0xe / 2)
        {
            // Check if the address is in the I/O range
            if (addr >= 0xff00)
                return readIO(addr & 0xff); // Read from I/O register
            // Check if the address is in the "echo" range
            else if (addr < 0xfe00)
                addr -= 0x2000; // Subtract 0x2000 from address to read from the "echo" range
        }
        // Address is in other special range
        else if (area == 0xa / 2)
        {
            // Check if there's a read handler for this MBC
            if (readFunc != NULL)
                return readFunc(addr); // Call read handler function
            // Check if there are no RAM banks
            else if (!numRamBanks)
                return 0xff; // Return 0xFF if there are no RAM banks
        }
    }
#endif

    // Return the byte value from the memory array at the specified address
    return memory[addr >> 12][addr & 0xfff];
}

/**
 * This function reads a 16-bit value from memory starting at the given address. The function calls the `readMemory` function twice
 * to get the low byte and high byte of the 16-bit value, and combines the two bytes into a single 16-bit value using a bitwise OR
 * and shift operations.
 *
 * @param addr The memory address to start reading from.
 * @return The 16-bit value read from memory.
 */
u16 readMemory16(u16 addr)
{
    return readMemory(addr) | readMemory(addr + 1) << 8; // Combine the low byte and high byte into a single 16-bit value.
}

#ifdef DS
u8 readIO(u8 ioReg) ITCM_CODE;
#endif

/**
 * This function reads the byte value of a given I/O register. The function determines which register is being read and returns the
 * appropriate value. If the SPEEDHAX flag is defined, the function simply returns the value of the requested register from the ioRam array.
 *
 * @param ioReg The I/O register to read from.
 * @return The byte value read from the I/O register.
 */
u8 readIO(u8 ioReg)
{
#ifdef SPEEDHAX
    return ioRam[ioReg];
#else
    switch (ioReg)
    {
    case 0x00:
        if (!(ioRam[ioReg] & 0x20))
            return 0xc0 | (ioRam[ioReg] & 0xF0) | (buttonsPressed & 0xF);
        else if (!(ioRam[ioReg] & 0x10))
            return 0xc0 | (ioRam[ioReg] & 0xF0) | ((buttonsPressed & 0xF0) >> 4);
        else
            return ioRam[ioReg];
    case 0x10: // NR10, sweep register 1, bit 7 set on read
        return ioRam[ioReg] | 0x80;
    case 0x11: // NR11, sound length/pattern duty 1, bits 5-0 set on read
        return ioRam[ioReg] | 0x3F;
    case 0x13: // NR13, sound frequency low byte 1, all bits set on read
        return 0xFF;
    case 0x14: // NR14, sound frequency high byte 1, bits 7,5-0 set on read
        return ioRam[ioReg] | 0xBF;
    case 0x15: // No register, all bits set on read
        return 0xFF;
    case 0x16: // NR21, sound length/pattern duty 2, bits 5-0 set on read
        return ioRam[ioReg] | 0x3F;
    case 0x18: // NR23, sound frequency low byte 2, all bits set on read
        return 0xFF;
    case 0x19: // NR24, sound frequency high byte 2, bits 7,5-0 set on read
        return ioRam[ioReg] | 0xBF;
    case 0x1A: // NR30, sound mode 3, bits 6-0 set on read
        return ioRam[ioReg] | 0x7F;
    case 0x1B: // NR31, sound length 3, all bits set on read
        return 0xFF;
    case 0x1C: // NR32, sound output level 3, bits 7,4-0 set on read
        return ioRam[ioReg] | 0x9F;
    case 0x1D: // NR33, sound frequency low byte 2, all bits set on read
        return 0xFF;
    case 0x1E: // NR34, sound frequency high byte 2, bits 7,5-0 set on read
        return ioRam[ioReg] | 0xBF;
    case 0x1F: // No register, all bits set on read
        return 0xFF;
    case 0x20: // NR41, sound mode/length 4, all bits set on read
        return 0xFF;
    case 0x23: // NR44, sound counter/consecutive, bits 7,5-0 set on read
        return ioRam[ioReg] | 0xBF;
    case 0x26: // NR52, global sound status, bits 6-4 set on read
        return ioRam[ioReg] | 0x70;
    case 0x27: // No register, all bits set on read
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        return 0xFF;
    case 0x70: // wram register
        return ioRam[ioReg] | 0xf8;
    default:
        return ioRam[ioReg];
    }
#endif
}

#ifdef DS
void writeMemory(u16 addr, u8 val) ITCM_CODE;
#endif

/**
 * This function writes a byte value to a given memory address. The function determines which memory area the address belongs to
 * and writes the value accordingly. If the address belongs to a specific area, the function calls the appropriate write function.
 *
 * @param addr The memory address to write to.
 * @param val The byte value to write.
 */
void writeMemory(u16 addr, u8 val)
{
    switch (addr >> 12) // Determine which memory area the address belongs to.
    {
    case 0x8:
    case 0x9:
        writeVram(addr & 0x1fff, val); // If the address belongs to VRAM, write to VRAM using the writeVram() function.
        return;
    case 0xC:
        wram[0][addr & 0xFFF] = val; // If the address belongs to WRAM bank 0, write to WRAM bank 0.
        return;
    case 0xD:
        wram[wramBank][addr & 0xFFF] = val; // If the address belongs to the selected WRAM bank, write to that bank.
        return;
    case 0xE: // If the address belongs to the echo area, write to WRAM bank 0.
        wram[0][addr & 0xFFF] = val;
        return;
    case 0xF:
        if (addr >= 0xFF00) // If the address belongs to I/O registers, write to the appropriate register using the writeIO() function.
            writeIO(addr & 0xFF, val);
        else if (addr >= 0xFE00) // If the address belongs to HRAM, write to HRAM using the writeHram() function.
            writeHram(addr & 0x1ff, val);
        else // If the address belongs to the echo area, write to the selected WRAM bank.
            wram[wramBank][addr & 0xFFF] = val;
        return;
    }

    if (writeFunc != NULL) // If a write function is specified, call that function with the address and value.
        writeFunc(addr, val);
}

bool nifiTryAgain = true;
/**
 * This function is called when a Nifi timeout occurs. If a Nifi communication is ongoing and has not received a response,
 * it attempts to send the packet again. If there is still no response, it assumes that there is no connection and sets the
 * appropriate flags and interrupts.
 */
void nifiTimeoutFunc()
{
    printLog("Nifi timeout@n"); // Log a message indicating that a Nifi timeout has occurred.
    
    if (nifiTryAgain) // If a Nifi communication is ongoing and has not received a response, attempt to send the packet again.
    {
        nifiSendid--;
        sendPacketByte(55, ioRam[0x01]);
        nifiSendid++;
        nifiTryAgain = false;
    }
    else // If there is still no response, assume that there is no connection and set the appropriate flags and interrupts.
    {
        printLog("No nifi response received@n"); // Log a message indicating that no Nifi response was received.
        ioRam[0x01] = 0xff;
        requestInterrupt(SERIAL);
        ioRam[0x02] &= ~0x80;
        timerStop(2);
    }
}

#ifdef DS
void writeIO(u8 ioReg, u8 val) ITCM_CODE;
#endif

/**
 * @brief Writes a value to an I/O register.
 * 
 * This function handles the writing of a given value to a specific I/O register in the system. It manages the
 * operations related to the different registers, such as sound, video, timers, and others. The behavior of the
 * function depends on the input I/O register and the value being written. The following operations are performed:
 * 
 * - For specific I/O registers, the Super Game Boy (SGB) or Game Boy (GB) mode is handled.
 * - Timers and serial communication are managed for certain I/O registers.
 * - Sound registers are updated based on the input register and value.
 * - Video registers are updated accordingly, and VRAM and WRAM banks are refreshed when necessary.
 * - Direct Memory Access (DMA) operations are performed for certain I/O registers in Color Game Boy (CGB) mode.
 * - Interrupts are requested and cycles are executed based on the values of specific I/O registers.
 * 
 * @param ioReg The I/O register to be written to (8-bit unsigned integer). Determines the specific register operation.
 * @param val The value to be written to the specified I/O register (8-bit unsigned integer). Defines the data for the register operation.
 */
void writeIO(u8 ioReg, u8 val)
{
    switch (ioReg)
    {
    case 0x00:
        if (sgbMode)
            sgbHandleP1(val);
        else
        {
            ioRam[0x00] = val;
            refreshP1();
        }
        return;
    case 0x02:
    {
        ioRam[ioReg] = val;
        if (!nifiEnabled)
        {
            if (val & 0x80 && val & 0x01)
            {
                serialCounter = clockSpeed / 1024;
                if (cyclesToExecute > serialCounter)
                    cyclesToExecute = serialCounter;
            }
            else
                serialCounter = 0;
            return;
        }
        linkSendData = ioRam[0x01];
        if (val & 0x80)
        {
            if (transferWaiting)
            {
                sendPacketByte(56, ioRam[0x01]);
                ioRam[0x01] = linkReceivedData;
                requestInterrupt(SERIAL);
                ioRam[ioReg] &= ~0x80;
                transferWaiting = false;
            }
            if (val & 1)
            {
                nifiTryAgain = true;
                timerStart(2, ClockDivider_64, 10000, nifiTimeoutFunc);
                sendPacketByte(55, ioRam[0x01]);
                nifiSendid++;
            }
        }
        return;
    }
    case 0x04:
        ioRam[ioReg] = 0;
        return;
    case 0x05:
        ioRam[ioReg] = val;
        break;
    case 0x06:
        ioRam[ioReg] = val;
        break;
    case 0x07:
        timerPeriod = periods[val & 0x3];
        ioRam[ioReg] = val;
        break;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
        handleSoundRegister(ioReg, val);
        return;
    case 0x40:
    case 0x42:
    case 0x43:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x69:
    case 0x6B:
        handleVideoRegister(ioReg, val);
        return;
    case 0x41:
        ioRam[ioReg] &= 0x7;
        ioRam[ioReg] |= val & 0xF8;
        return;
    case 0x44:
        // ioRam[0x44] = 0;
        printLog("LY Write %d@n", val);
        return;
    case 0x45:
        ioRam[ioReg] = val;
        checkLYC();
        cyclesToExecute = -1;
        return;
    case 0x68:
        ioRam[ioReg] = val;
        ioRam[0x69] = bgPaletteData[val & 0x3F];
        return;
    case 0x6A:
        ioRam[ioReg] = val;
        ioRam[0x6B] = sprPaletteData[val & 0x3F];
        return;
    case 0x4D:
        ioRam[ioReg] &= 0x80;
        ioRam[ioReg] |= (val & 1);
        return;
    case 0x4F: // Vram bank
        if (gbMode == CGB)
        {
            vramBank = val & 1;
            refreshVramBank();
        }
        ioRam[ioReg] = val & 1;
        return;
        // Special register, used by the gameboy bios
    case 0x50:
        biosOn = 0;
        memory[0x0] = romSlot0;
        initGameboyMode();
        return;
    case 0x55: // CGB DMA
        if (gbMode == CGB)
        {
            if (dmaLength > 0)
            {
                if ((val & 0x80) == 0)
                {
                    ioRam[ioReg] |= 0x80;
                    dmaLength = 0;
                }
                return;
            }
            dmaLength = ((val & 0x7F) + 1);
            dmaSource = (ioRam[0x51] << 8) | (ioRam[0x52]);
            dmaSource &= 0xFFF0;
            dmaDest = (ioRam[0x53] << 8) | (ioRam[0x54]);
            dmaDest &= 0x1FF0;
            dmaMode = val >> 7;
            ioRam[ioReg] = dmaLength - 1;
            if (dmaMode == 0)
            {
                int i;
                for (i = 0; i < dmaLength; i++)
                {
                    writeVram16(dmaDest, dmaSource);
                    dmaDest += 0x10;
                    dmaSource += 0x10;
                }
                extraCycles += dmaLength * 8 * (doubleSpeed + 1);
                dmaLength = 0;
                ioRam[ioReg] = 0xFF;
                ioRam[0x51] = dmaSource >> 8;
                ioRam[0x52] = dmaSource & 0xff;
                ioRam[0x53] = dmaDest >> 8;
                ioRam[0x54] = dmaDest & 0xff;
            }
        }
        else
            ioRam[ioReg] = val;
        return;
    case 0x70: // WRAM bank, for CGB only
        if (gbMode == CGB)
        {
            wramBank = val & 0x7;
            if (wramBank == 0)
                wramBank = 1;
            refreshWramBank();
        }
        ioRam[ioReg] = val & 0x7;
        return;
    case 0x0F:
        ioRam[ioReg] = val;
        if (val & ioRam[0xff])
            cyclesToExecute = -1;
        break;
    case 0xFF:
        ioRam[ioReg] = val;
        if (val & ioRam[0x0f])
            cyclesToExecute = -1;
        break;
    default:
        ioRam[ioReg] = val;
        return;
    }
}

/**
 * This function updates the input register (0xFF00) based on the current state of the buttons pressed.
 * If the SGB packet bit is not set, the function sets the input register based on the button state and the
 * selected input type. If the SGB packet bit is set, the function checks if the game is in SGB mode and
 * sets the input register accordingly.
 */
void refreshP1()
{
    // Check if input register is being used for SGB packets.
    if (sgbPacketBit == -1)
    {
        if ((ioRam[0x00] & 0x30) == 0x30) // Check if input type is set to "multibutton".
        {
            if (!sgbMode) // If not in SGB mode, set all buttons to pressed.
                ioRam[0x00] |= 0x0F;
        }
        else // If input type is not "multibutton", set input register based on the buttons pressed.
        {
            ioRam[0x00] &= 0xF0;
            if (!(ioRam[0x00] & 0x20))
                ioRam[0x00] |= (buttonsPressed & 0xF);
            else
                ioRam[0x00] |= ((buttonsPressed & 0xF0) >> 4);
        }
        ioRam[0x00] |= 0xc0; // Set the input register mode to "joypad" and enable the button input.
    }
}

/**
 * This function updates the Hblank DMA by writing 16-bit values from the source to the destination address
 * in VRAM, and decrementing the DMA length by 1. It also updates the relevant memory addresses to reflect
 * the new DMA parameters.
 *
 * @return A boolean value indicating whether the DMA is still ongoing or has completed.
 */
bool updateHblankDMA()
{
    if (dmaLength > 0) // Check if there are still bytes left to transfer.
    {
        // Write 16-bit values from the source to the destination address in VRAM.
        writeVram16(dmaDest, dmaSource);
        
        // Increment the destination and source addresses, and decrement the DMA length by 1.
        dmaDest += 16;
        dmaSource += 16;
        dmaLength--;
        
        if (dmaLength == 0) // If the DMA has completed, set a flag in memory.
        {
            ioRam[0x55] = 0xFF;
        }
        else // If the DMA is ongoing, update the relevant memory addresses.
        {
            ioRam[0x55] = dmaLength - 1;
            ioRam[0x51] = dmaSource >> 8;
            ioRam[0x52] = dmaSource & 0xff;
            ioRam[0x53] = dmaDest >> 8;
            ioRam[0x54] = dmaDest & 0xff;
        }
        
        return true; // Return true to indicate that the DMA is still ongoing.
    }
    else
    {
        return false; // Return false to indicate that the DMA has completed.
    }
}

/**
 * This function sets the rumble feature of the game controller if it is inserted.
 * If it is not inserted, it sets the rumble feature using direct memory access.
 *
 * @param rumbleVal A boolean value representing the state of the rumble feature.
 */
void doRumble(bool rumbleVal)
{
    if (rumbleInserted == 1) // Check if the rumble feature is inserted.
    {
        setRumble(rumbleVal); // Set the rumble feature using the setRumble() function.
    }
    else if (rumbleInserted == 2) // If the rumble feature is not inserted, set the rumble using direct memory access.
    {
        // Set the values of various memory addresses to enable or disable the rumble feature.
        GBA_BUS[0x1FE0000 / 2] = 0xd200;
        GBA_BUS[0x0000000 / 2] = 0x1500;
        GBA_BUS[0x0020000 / 2] = 0xd200;
        GBA_BUS[0x0040000 / 2] = 0x1500;
        
        // Set the strength of the rumble feature, if enabled.
        GBA_BUS[0x1E20000 / 2] = rumbleVal ? (0xF0 + rumbleStrength) : 0x08;
        
        // Set the value of a memory address to enable or disable the rumble feature.
        GBA_BUS[0x1FC0000 / 2] = 0x1500;
    }
}
