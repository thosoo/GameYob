#include <nds.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
// #include <iostream>
// #include <string>
#include "gbgfx.h"
#include "gbcpu.h"
#include "inputhelper.h"
#include "filechooser.h"
#include "timer.h"
#include "gbsnd.h"
#include "gameboy.h"
#include "mmu.h"
#include "main.h"
#include "console.h"
#include "nifi.h"
#include "cheats.h"
#include "gbs.h"
#include "common.h"

extern time_t rawTime;
extern time_t lastRawTime;

volatile SharedData *sharedData;

/**
 * @brief Handle 32-bit FIFO messages.
 *
 * This function is used to handle 32-bit FIFO messages, specifically to signal sleep mode starting or ending.
 *
 * @param[in] value The 32-bit value.
 * @param[in] user_data Pointer to user-defined data.
 *
 * @return void
 *
 * @see pauseGameboy(), isGameboyPaused(), unpauseGameboy()
 */
void fifoValue32Handler(u32 value, void *user_data)
{
    static u8 scalingWasOn; /**< The scaling value before entering sleep mode. */
    static bool soundWasDisabled; /**< The sound state before entering sleep mode. */
    static bool wasPaused; /**< Whether the Game Boy was paused before entering sleep mode. */

    switch (value)
    {
    case FIFOMSG_LID_CLOSED:
        // Entering sleep mode
        scalingWasOn = sharedData->scalingOn;
        soundWasDisabled = soundDisabled;
        wasPaused = isGameboyPaused();

        sharedData->scalingOn = 0;
        soundDisabled = true;
        pauseGameboy();
        break;
    case FIFOMSG_LID_OPENED:
        // Exiting sleep mode
        sharedData->scalingOn = scalingWasOn;
        soundDisabled = soundWasDisabled;
        if (!wasPaused)
            unpauseGameboy();

        // Time isn't incremented properly in sleep mode, compensate here.
        time(&rawTime);
        lastRawTime = rawTime;
        break;
    }
}


/**
 * @brief Loads a ROM file and initializes the Gameboy emulator for the first time.
 * 
 * This function unloads the current ROM, displays a file chooser dialog to let the user
 * select a new ROM file to load, and initializes the emulator with the selected ROM.
 * If a BIOS file is present, it is loaded and used by the emulator as well.
 * 
 * @note This function must be called after the emulator has been initialized at least once.
 */
void selectRom()
{
    // Unload the current ROM
    unloadRom();

    // Display a file chooser dialog to let the user select a new ROM file to load
    loadFileChooserState(&romChooserState);
    const char *extraExtensions[] = {"gbs"};
    int len = (sizeof(extraExtensions) / sizeof(const char *));
    char *filename = startFileChooser(extraExtensions, len, true);
    saveFileChooserState(&romChooserState);

    // Load the BIOS file if it exists
    if (!biosExists)
    {
        FILE *file;
        file = fopen("gbc_bios.bin", "rb");
        biosExists = file != NULL;
        if (biosExists)
        {
            fread(bios, 1, 0x900, file);
            fclose(file);
        }
    }

    // Load the selected ROM file and update the screens
    loadRom(filename);
    free(filename);
    updateScreens();

    // Initialize the emulator for the first time
    initializeGameboyFirstTime();
}

/**
 * @brief This function initializes the Game Boy mode based on the current ROM's header values.
 *
 * It checks if the SGB mode option is enabled and if the ROM header contains the SGB identifier. 
 * If both conditions are met, it sets the Game Boy mode to SGB (Super Game Boy) mode. Otherwise,
 * it sets the Game Boy mode to GB (Game Boy) mode.
 *
 * @return void
 */ 
void initGBMode()
{
    if (sgbModeOption != 0 && romSlot0[0x14b] == 0x33 && romSlot0[0x146] == 0x03)
        resultantGBMode = 2; // SGB mode
    else
    {
        resultantGBMode = 0; // GB mode
    }
}

/**
 * @brief This function initializes the Game Boy Color mode based on the current ROM's header values.
 *
 * It checks if the SGB mode option is set to 2 and if the ROM header contains the SGB identifier. 
 * If both conditions are met, it sets the Game Boy Color mode to SGB (Super Game Boy) mode. Otherwise,
 * it sets the Game Boy Color mode to GBC (Game Boy Color) mode.
 *
 * @return void
 */ 
void initGBCMode()
{
    if (sgbModeOption == 2 && romSlot0[0x14b] == 0x33 && romSlot0[0x146] == 0x03)
        resultantGBMode = 2; // SGB mode
    else
    {
        resultantGBMode = 1; // GBC mode
    }
}

/**
 * @brief Initializes the Gameboy emulator and sets the resulting GB mode depending on the game type and user preferences.
* This function initializes the Gameboy emulator by enabling sleep mode, setting frame counter to zero,
* and initializing the SGB mode to false. Depending on the type of game being loaded and the user's preferences,
* the resultant GB mode is determined either as GB, GBC, or SGB. MMU, CPU, LCD, GFX, and SND are also initialized
* and GBS mode is handled accordingly.
* @return void
*/
void initializeGameboy()
{
    enableSleepMode(); // Set the Gameboy to sleep mode
    gameboyFrameCounter = 0; // Reset the frame counter to 0
    sgbMode = false; // Set the Super Gameboy mode to false

    if (gbsMode) // Check if it is in Gameboy Sound mode
    {
        resultantGBMode = 1; // Set the resultant Gameboy mode to Gameboy Color
        probingForBorder = false; // Stop probing for border
    }
    else // If not in Gameboy Sound mode
    {
        switch (gbcModeOption) // Check the Gameboy Color mode option
        {
        case 0: // Gameboy mode
            initGBMode(); // Initialize Gameboy mode
            break;
        case 1: // Gameboy Color mode if needed
            if (romSlot0[0x143] == 0xC0)
                initGBCMode(); // Initialize Gameboy Color mode
            else
                initGBMode(); // Initialize Gameboy mode
            break;
        case 2: // Gameboy Color mode
            if (romSlot0[0x143] == 0x80 || romSlot0[0x143] == 0xC0)
                initGBCMode(); // Initialize Gameboy Color mode
            else
                initGBMode(); // Initialize Gameboy mode
            break;
        }

        bool sgbEnhanced = romSlot0[0x14b] == 0x33 && romSlot0[0x146] == 0x03; // Check if the ROM is Super Gameboy enhanced
        if (sgbEnhanced && resultantGBMode != 2 && probingForBorder)
        {
            resultantGBMode = 2; // Set the resultant Gameboy mode to Super Gameboy
        }
        else
        {
            probingForBorder = false; // Stop probing for border
        }
    } // !gbsMode

    initMMU(); // Initialize the Memory Management Unit
    initCPU(); // Initialize the Central Processing Unit
    initLCD(); // Initialize the Liquid Crystal Display
    initGFX(); // Initialize the Graphics
    initSND(); // Initialize the Sound

    if (!gbsMode && !probingForBorder && suspendStateExists)
    {
        loadState(-1); // Load the suspended state
    }

    if (gbsMode)
        gbsInit(); // Initialize the Gameboy Sound mode

    // We haven't calculated the # of cycles to the next hardware event.
    cyclesToEvent = 1; // Set the number of cycles to the next hardware event to 1
}

/**
 * @brief Initializes the Gameboy system for the first time.
 * If SGB borders are enabled, sets probingForBorder flag to true, which will be ignored if starting in SGB mode or if there is no SGB mode. Then sets sgbBorderLoaded flag to false, which effectively unloads any SGB border. Calls initializeGameboy() to continue with Gameboy initialization.
 * If in GBS mode, disables several menu options related to save states. Otherwise, enables menu options related to save states and the ability to exit without saving. If the GBC BIOS exists, enables the option to load it from the menu.
 * @return void
*/
void initializeGameboyFirstTime()
{
    if (sgbBordersEnabled)
        probingForBorder = true; // This will be ignored if starting in sgb mode, or if there is no sgb mode.
    sgbBorderLoaded = false;     // Effectively unloads any sgb border

    initializeGameboy();

    if (gbsMode)
    {
        disableMenuOption("State Slot");
        disableMenuOption("Save State");
        disableMenuOption("Load State");
        disableMenuOption("Delete State");
        disableMenuOption("Suspend");
        disableMenuOption("Exit without saving");
    }
    else
    {
        enableMenuOption("State Slot");
        enableMenuOption("Save State");
        enableMenuOption("Suspend");
        if (checkStateExists(stateNum))
        {
            enableMenuOption("Load State");
            enableMenuOption("Delete State");
        }
        else
        {
            disableMenuOption("Load State");
            disableMenuOption("Delete State");
        }

        if (numRamBanks && !autoSavingEnabled)
            enableMenuOption("Exit without saving");
        else
            disableMenuOption("Exit without saving");
    }

    if (biosExists)
        enableMenuOption("GBC Bios");
    else
        disableMenuOption("GBC Bios");
}

/**
 * @brief Main function for the Gameboy emulator.
 * This function initializes the emulator by setting various parameters and calling the necessary initialization functions.
 * It also loads a ROM file if passed as a command-line argument, otherwise it prompts the user to select a ROM file.
 * Finally, it runs the emulator by calling the runEmul() function and returns 0 when finished.
 * @param argc The number of command-line arguments passed to the program.
 * @param argv An array of command-line argument strings.
 * @return An integer indicating the success of the program (always returns 0).
 */
int main(int argc, char *argv[])
{
    REG_POWERCNT = POWER_ALL & ~(POWER_MATRIX | POWER_3D_CORE); // don't need 3D
    consoleDebugInit(DebugDevice_CONSOLE);

    defaultExceptionHandler();

    time(&rawTime);
    lastRawTime = rawTime;
    timerStart(0, ClockDivider_1024, TIMER_FREQ_1024(1), clockUpdater);

    /* Reset the EZ3in1 if present */
    if (!__dsimode)
    {
        sysSetCartOwner(BUS_OWNER_ARM9);

        GBA_BUS[0x0000] = 0xF0;
        GBA_BUS[0x1000] = 0xF0;
    }

    fifoSetValue32Handler(FIFO_USER_02, fifoValue32Handler, NULL);

    sharedData = (SharedData *)memUncached(malloc(sizeof(SharedData)));
    sharedData->scalingOn = false;
    sharedData->enableSleepMode = true;
    // It might make more sense to use "fifoSendAddress" here.
    // However there may have been something wrong with it in dsi mode.
    fifoSendValue32(FIFO_USER_03, ((u32)sharedData) & 0x00ffffff);

    initInput();
    setMenuDefaults();
    readConfigFile();
    swiWaitForVBlank();
    swiWaitForVBlank();
    // initGFX is called in initializeGameboy, but I also call it from here to
    // set up the vblank handler asap.
    initGFX();

    consoleInitialized = false;

    if (argc >= 2)
    {
        char *filename = argv[1];
        loadRom(filename);
        updateScreens();
        initializeGameboyFirstTime();
    }
    else
    {
        selectRom();
    }

    runEmul();

    return 0;
}
