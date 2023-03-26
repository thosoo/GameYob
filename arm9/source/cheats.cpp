#include <string.h>
#include <algorithm>
#include <nds.h>
#include "gameboy.h"
#include "mmu.h"
#include "console.h"
#include "main.h"
#include "cheats.h"
#include "inputhelper.h"
#include "gbgfx.h"

#define TO_INT(a) ((a) >= 'a' ? (a) - 'a' + 10 : (a) >= 'A' ? (a) - 'A' + 10 \
                                                            : (a) - '0')

bool cheatsEnabled = true;
cheat_t cheats[MAX_CHEATS];
int numCheats = 0;

// Use this to check whether another rom has been loaded
char cheatsRomTitle[20] = "\0";

/**
 * @brief Enables or disables cheats.
 *
 * This function enables or disables cheats based on the `enable` parameter. If
 * `enable` is set to true, cheats are enabled; if it's set to false, cheats are
 * disabled.
 *
 * @param enable A boolean indicating whether cheats should be enabled or disabled.
 */
void enableCheats(bool enable)
{
    cheatsEnabled = enable;
}

/**
 * @brief Adds a new cheat to the cheats array.
 * 
 * This function attempts to add a new cheat to the cheats array based on the given cheat string.
 * The function checks the length of the string to determine whether the cheat is a Game Genie cheat
 * or a Gameshark cheat. The function then extracts the cheat data from the string and sets the appropriate
 * flags and data fields in the cheats array. If the cheat is added successfully, the function increments
 * the numCheats variable and returns true. If the cheats array is already full or the cheat string is not
 * recognized, the function returns false.
 * 
 * @param str The cheat string to add to the cheats array.
 * 
 * @return A boolean value indicating whether the cheat was added successfully (true) or not (false).
 */
bool addCheat(const char *str)
{
    int len;
    int i = numCheats;

    if (i == MAX_CHEATS)
        return false;

    // Clear all flags and data fields for the new cheat
    cheats[i].flags = 0;
    cheats[i].patchedBanks = std::vector<int>();
    cheats[i].patchedValues = std::vector<int>();

    len = strlen(str);
    strncpy(cheats[i].cheatString, str, 12);

    // GameGenie AAA-BBB-CCC
    if (len == 11)
    {
        cheats[i].flags |= FLAG_GAMEGENIE;

        cheats[i].data = TO_INT(str[0]) << 4 | TO_INT(str[1]);
        cheats[i].address = TO_INT(str[6]) << 12 | TO_INT(str[2]) << 8 |
                            TO_INT(str[4]) << 4 | TO_INT(str[5]);
        cheats[i].compare = TO_INT(str[8]) << 4 | TO_INT(str[10]);

        cheats[i].address ^= 0xf000;
        cheats[i].compare = (cheats[i].compare >> 2) | (cheats[i].compare & 0x3) << 6;
        cheats[i].compare ^= 0xba;

        printLog("GG %04x / %02x -> %02x\n", cheats[i].address, cheats[i].data, cheats[i].compare);
    }
    // GameGenie (6digit version) AAA-BBB
    else if (len == 7)
    {
        cheats[i].flags |= FLAG_GAMEGENIE1;

        cheats[i].data = TO_INT(str[0]) << 4 | TO_INT(str[1]);
        cheats[i].address = TO_INT(str[6]) << 12 | TO_INT(str[2]) << 8 |
                            TO_INT(str[4]) << 4 | TO_INT(str[5]);

        printLog("GG1 %04x / %02x\n", cheats[i].address, cheats[i].data);
    }
    // Gameshark AAAAAAAA
    else if (len == 8)
    {
        cheats[i].flags |= FLAG_GAMESHARK;

        cheats[i].data = TO_INT(str[2]) << 4 | TO_INT(str[3]);
        cheats[i].bank = TO_INT(str[0]) << 4 | TO_INT(str[1]);
        cheats[i].address = TO_INT(str[6]) << 12 | TO_INT(str[7]) << 8 |
                            TO_INT(str[4]) << 4 | TO_INT(str[5]);

        printLog("GS (%02x)%04x/ %02x\n", cheats[i].bank, cheats[i].address, cheats[i].data);
    }
    else
    { // dafuq did i just read ?
        return false;
    }

    numCheats++;
    return true;
}

/**
 * @brief Toggles a cheat on or off.
 * 
 * This function toggles a specified cheat on or off based on the value of the 'enabled' parameter.
 * If the cheat is being enabled, the function sets the 'FLAG_ENABLED' flag for the cheat and applies
 * the cheat to all ROM banks (if the cheat is a Game Genie cheat). If the cheat is being disabled, the
 * function unapplies the cheat and clears the 'FLAG_ENABLED' flag.
 * 
 * @param i The index of the cheat to toggle.
 * @param enabled A boolean value indicating whether the cheat should be enabled (true) or disabled (false).
 * 
 * @return void
 */
void toggleCheat(int i, bool enabled)
{
    if (enabled)
    {
        // Enable the cheat by setting the FLAG_ENABLED flag and applying the cheat to all ROM banks (if necessary).
        cheats[i].flags |= FLAG_ENABLED;
        if ((cheats[i].flags & FLAG_TYPE_MASK) != FLAG_GAMESHARK)
        {
            for (int j = 0; j < numRomBanks; j++)
            {
                if (isRomBankLoaded(j))
                    applyGGCheatsToBank(j);
            }
        }
    }
    else
    {
        // Disable the cheat by unapplying the cheat and clearing the FLAG_ENABLED flag.
        unapplyGGCheat(i);
        cheats[i].flags &= ~FLAG_ENABLED;
    }
}


/**
 * @brief Unapplies a Game Genie cheat.
 * 
 * This function unapplies a specified Game Genie cheat by restoring the original ROM values
 * that were patched by the cheat. The function iterates through the list of banks that were
 * patched by the cheat and restores the original ROM value for each bank.
 * 
 * @param cheat The index of the cheat to unapply.
 * 
 * @return void
 */
void unapplyGGCheat(int cheat)
{
    if ((cheats[cheat].flags & FLAG_TYPE_MASK) != FLAG_GAMESHARK)
    {
        for (unsigned int i = 0; i < cheats[cheat].patchedBanks.size(); i++)
        {
            int bank = cheats[cheat].patchedBanks[i];
            if (isRomBankLoaded(bank))
            {
                // Restore the original ROM value for the specified bank and address.
                getRomBank(bank)[cheats[cheat].address & 0x3fff] = cheats[cheat].patchedValues[i];
            }
        }
        // Clear the list of patched banks and values for this cheat.
        cheats[cheat].patchedBanks = std::vector<int>();
        cheats[cheat].patchedValues = std::vector<int>();
    }
}

/**
 * @brief Applies Game Genie cheats to a ROM bank.
 * 
 * This function applies enabled Game Genie cheats to a specified ROM bank.
 * For each enabled Game Genie cheat, the function checks whether the cheat should be applied
 * to the specified bank based on the cheat's address and the ROM bank number. If the cheat should
 * be applied to the bank, the function patches the ROM with the cheat data and saves the original
 * ROM value to a list for later unpatching.
 * 
 * @param bank The ROM bank number to apply the cheats to.
 * 
 * @return void
 */
void applyGGCheatsToBank(int bank)
{
    u8 *bankPtr = getRomBank(bank);
    for (int i = 0; i < numCheats; i++)
    {
        if (cheats[i].flags & FLAG_ENABLED && ((cheats[i].flags & FLAG_TYPE_MASK) != FLAG_GAMESHARK))
        {
            int bankSlot = cheats[i].address / 0x4000;
            if ((bankSlot == 0 && bank == 0) || (bankSlot == 1 && bank != 0))
            {
                int address = cheats[i].address & 0x3fff;
                if (((cheats[i].flags & FLAG_TYPE_MASK) == FLAG_GAMEGENIE1 || bankPtr[address] == cheats[i].compare) &&
                    find(cheats[i].patchedBanks.begin(), cheats[i].patchedBanks.end(), bank) == cheats[i].patchedBanks.end())
                {
                    // Add the bank to the list of patched banks for this cheat.
                    cheats[i].patchedBanks.push_back(bank);
                    // Save the original ROM value to a list for later unpatching.
                    cheats[i].patchedValues.push_back(bankPtr[address]);
                    // Patch the ROM with the cheat data.
                    bankPtr[address] = cheats[i].data;
                }
            }
        }
    }
}

void applyGSCheats(void) ITCM_CODE;

/**
 * @brief Applies GameShark cheats to the Game Boy's memory.
 * 
 * This function applies enabled GameShark cheats to the Game Boy's memory.
 * For each enabled GameShark cheat, the function determines the bank to apply the cheat to
 * based on the cheat's address and the type of bank. Then it writes the cheat data to the
 * appropriate bank.
 * 
 * @return void
 */
void applyGSCheats(void)
{
    int i;
    int compareBank;

    for (i = 0; i < numCheats; i++)
    {
        if (cheats[i].flags & FLAG_ENABLED && ((cheats[i].flags & FLAG_TYPE_MASK) == FLAG_GAMESHARK))
        {
            switch (cheats[i].bank & 0xf0)
            {
            case 0x90:
                compareBank = wramBank;
                wramBank = cheats[i].bank & 0x7;
                writeMemory(cheats[i].address, cheats[i].data);
                wramBank = compareBank;
                break;
            case 0x80: /* TODO : Find info and stuff */
                break;
            case 0x00:
                writeMemory(cheats[i].address, cheats[i].data);
                break;
            }
        }
    }
}


/**
 * @brief Loads cheats from a file.
 * 
 * This function loads cheats from a file with the specified filename.
 * If the ROM title matches the current ROM, any existing cheats are unapplied.
 * If the ROM title has changed, the existing cheats are discarded.
 * 
 * @param filename The filename of the cheat file to load.
 * 
 * @return void
 */
void loadCheats(const char *filename)
{
    if (strcmp(cheatsRomTitle, getRomTitle()) == 0)
    {
        // If the ROM hasn't been changed, unapply any existing cheats.
        for (int i = 0; i < numCheats; i++)
            unapplyGGCheat(i);
    }
    else
    {
        // If the ROM has been changed, discard any existing cheats and update the ROM title.
        strncpy(cheatsRomTitle, getRomTitle(), 20);
        cheatsRomTitle[19] = '\0';
        numCheats = 0;
    }

    // Open the cheat file for reading.
    FILE *file = fopen(filename, "r");
    if (file == NULL)
    {
        disableMenuOption("Manage Cheats");
        return;
    }

    // Read each line of the file and add the cheat to the cheats array.
    while (!feof(file))
    {
        int i = numCheats;

        char line[100];
        fgets(line, 100, file);

        if (*line != '\0')
        {
            char *spacePos = strchr(line, ' ');
            if (spacePos != NULL)
            {
                *spacePos = '\0';
                if (strlen(spacePos + 1) >= 1 && addCheat(line))
                {
                    strncpy(cheats[i].name, spacePos + 2, MAX_CHEAT_NAME_LEN);
                    cheats[i].name[MAX_CHEAT_NAME_LEN] = '\0';
                    char c;
                    while ((c = cheats[i].name[strlen(cheats[i].name) - 1]) == '\n' || c == '\r')
                        cheats[i].name[strlen(cheats[i].name) - 1] = '\0';
                    toggleCheat(i, *(spacePos + 1) == '1');
                }
            }
        }
    }

    fclose(file); // Close the file.

    enableMenuOption("Manage Cheats"); // Enable the "Manage Cheats" menu option.
}

const int cheatsPerPage = 18;
int cheatMenuSelection = 0;
bool cheatMenu_gameboyWasPaused;

/**
 * @brief Redraws the cheat menu with the current selection and pagination.
 * 
 * This function clears the console screen and prints the cheat menu with the current selection
 * and pagination. The number of cheats per page is determined by the cheatsPerPage global variable.
 * 
 * @return void
 */
void redrawCheatMenu()
{
    int numPages = (numCheats - 1) / cheatsPerPage + 1; // Calculate the number of pages needed for pagination.

    int page = cheatMenuSelection / cheatsPerPage; // Calculate the current page based on the current selection.
    consoleClear(); // Clear the console screen.
    iprintf("          Cheat Menu      ");
    iprintf("%d/%d\n\n", page + 1, numPages); // Print the current page number and total number of pages.

    // Iterate through the cheats to be displayed on the current page.
    for (int i = page * cheatsPerPage; i < numCheats && i < (page + 1) * cheatsPerPage; i++)
    {
        int nameColor = (cheatMenuSelection == i ? CONSOLE_COLOR_LIGHT_YELLOW : CONSOLE_COLOR_WHITE); // Set the color for the cheat name based on whether it is currently selected.
        iprintfColored(nameColor, cheats[i].name); // Print the cheat name.
        for (unsigned int j = 0; j < 25 - strlen(cheats[i].name); j++)
            iprintf(" "); // Print spaces to align the cheat name and status.
        if (cheats[i].flags & FLAG_ENABLED)
        {
            if (cheatMenuSelection == i)
            {
                iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, "* ");
                iprintfColored(CONSOLE_COLOR_LIGHT_GREEN, "On");
                iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, " * "); // Print the status of the cheat in green and surrounded by asterisks if it is enabled and currently selected.
            }
            else
                iprintfColored(CONSOLE_COLOR_WHITE, "  On   "); // Print the status of the cheat in white if it is enabled but not currently selected.
        }
        else
        {
            if (cheatMenuSelection == i)
            {
                iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, "* ");
                iprintfColored(CONSOLE_COLOR_LIGHT_GREEN, "Off");
                iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, " *"); // Print the status of the cheat in green and surrounded by asterisks if it is disabled and currently selected.
            }
            else
                iprintfColored(CONSOLE_COLOR_WHITE, "  Off  "); // Print the status of the cheat in white if it is disabled but not currently selected.
        }
    }
}


/**
 * @brief Updates the cheat menu based on user input.
 * 
 * This function handles user input for navigating and toggling cheats in the cheat menu.
 * If the user selects a cheat or toggles its state, the cheat menu is redrawn.
 * 
 * @return void
 */
void updateCheatMenu()
{
    bool redraw = false; // Flag to indicate whether the cheat menu needs to be redrawn.

    if (cheatMenuSelection >= numCheats)
    {
        cheatMenuSelection = 0; // Reset the selection if it is out of bounds.
    }

    if (keyPressedAutoRepeat(KEY_UP))
    {
        if (cheatMenuSelection > 0)
        {
            cheatMenuSelection--; // Decrement the selection and mark the cheat menu for redraw.
            redraw = true;
        }
    }
    else if (keyPressedAutoRepeat(KEY_DOWN))
    {
        if (cheatMenuSelection < numCheats - 1)
        {
            cheatMenuSelection++; // Increment the selection and mark the cheat menu for redraw.
            redraw = true;
        }
    }
    else if (keyJustPressed(KEY_RIGHT | KEY_LEFT))
    {
        toggleCheat(cheatMenuSelection, !(cheats[cheatMenuSelection].flags & FLAG_ENABLED)); // Toggle the state of the currently selected cheat and mark the cheat menu for redraw.
        redraw = true;
    }
    else if (keyJustPressed(KEY_R))
    {
        cheatMenuSelection += cheatsPerPage; // Move the selection to the next page and wrap around if necessary.
        if (cheatMenuSelection >= numCheats)
            cheatMenuSelection = 0;
        redraw = true;
    }
    else if (keyJustPressed(KEY_L))
    {
        cheatMenuSelection -= cheatsPerPage; // Move the selection to the previous page and wrap around if necessary.
        if (cheatMenuSelection < 0)
            cheatMenuSelection = numCheats - 1;
        redraw = true;
    }

    if (keyJustPressed(KEY_B))
    {
        closeSubMenu(); // Close the cheat menu and unpause the game if it was not already paused.
        if (!cheatMenu_gameboyWasPaused)
            unpauseGameboy();
    }

    if (redraw)
        doAtVBlank(redrawCheatMenu); // If the cheat menu needs to be redrawn, schedule it for the next VBlank interrupt.
}

/**
 * @brief Starts the cheat menu and displays it on the screen.
 * 
 * This function checks if there are any cheats to display in the menu. If there are, it pauses the
 * Game Boy and displays the cheat menu as a sub-menu. The cheat menu is updated and redrawn on the
 * screen until the user closes the menu. If the Game Boy was not paused before the menu was opened,
 * it is unpaused when the menu is closed.
 * 
 * @return true if there are cheats to display in the menu, false otherwise.
 */
bool startCheatMenu()
{
    if (numCheats == 0)
        return false; // If there are no cheats to display, return false.

    cheatMenu_gameboyWasPaused = isGameboyPaused(); // Remember whether the Game Boy was paused before the menu was opened.
    pauseGameboy(); // Pause the Game Boy.
    displaySubMenu(updateCheatMenu); // Display the cheat menu as a sub-menu and set the update function.
    redrawCheatMenu(); // Draw the initial state of the cheat menu on the screen.

    return true; // Return true to indicate that the menu was successfully started.
}

/**
 * @brief Saves the current cheats to a file.
 * 
 * This function saves the current cheats to a file with the specified filename.
 * If there are no cheats to save, this function does nothing.
 * 
 * @param filename The filename to save the cheats to.
 * 
 * @return void
 */
void saveCheats(const char *filename)
{
    if (numCheats == 0)
        return; // If there are no cheats to save, do nothing.

    FILE *file = fopen(filename, "w"); // Open the file for writing.
    for (int i = 0; i < numCheats; i++)
    {
        // Write the cheat string, enabled flag, and name to the file.
        fiprintf(file, "%s %d%s\n", cheats[i].cheatString, !!(cheats[i].flags & FLAG_ENABLED), cheats[i].name);
    }
    fclose(file); // Close the file.
}
