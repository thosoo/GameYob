#include <nds.h>
#include <fat.h>

#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include "libfat_fake.h"
#include "inputhelper.h"
#include "mmu.h"
#include "gameboy.h"
#include "main.h"
#include "console.h"
#include "gbcpu.h"
#include "nifi.h"
#include "gbgfx.h"
#include "gbsnd.h"
#include "cheats.h"
#include "sgb.h"
#include "gbs.h"
#include "common.h"
#include "filechooser.h"

#define FAT_CACHE_SIZE 16

FILE* romFile=NULL;
FILE* saveFile=NULL;
char filename[100];
char savename[100];
char basename[100];
char romTitle[20];

char* romPath = NULL;
char* biosPath = NULL;
char* borderPath = NULL;

// Values taken from the cartridge header
u8 ramSize;
u8 mapper;
u8 cgbFlag;
u8 romSize;

int keysPressed=0;
int lastKeysPressed=0;
int keysForceReleased=0;
int repeatStartTimer=0;
int repeatTimer=0;

bool advanceFrame=false;

u8* romSlot0;
u8* romSlot1;
int maxLoadedRomBanks;
int numLoadedRomBanks;
u8* romBankSlots = NULL; // Each 0x4000 bytes = one slot
int bankSlotIDs[MAX_ROM_BANKS]; // Keeps track of which bank occupies which slot
std::vector<int> lastBanksUsed;

bool suspendStateExists;

void initInput()
{
    fatInit(FAT_CACHE_SIZE, true);
    //fatInitDefault();

    if (__dsimode)
        maxLoadedRomBanks = 512; // 8 megabytes
    else
        maxLoadedRomBanks = 128; // 2 megabytes

    romBankSlots = (u8*)malloc(maxLoadedRomBanks*0x4000);
}

// This function is supposed to flush the cache so I don't have to fclose() and fopen()
// a file in order to save it.
// But I found I could not rely on it in the gameboySyncAutosave() function.
void flushFatCache() {
    // This involves things from libfat which aren't normally visible
    devoptab_t* devops = (devoptab_t*)GetDeviceOpTab ("sd");
    PARTITION* partition = (PARTITION*)devops->deviceData;
    _FAT_cache_flush(partition->cache); // Flush the cache manually
}

const char* gbKeyNames[] = {"-","A","B","Left","Right","Up","Down","Start","Select",
    "Menu","Menu/Pause","Save","Autofire A","Autofire B", "Fast Forward", "FF Toggle", "Scale","Reset","A+B+START+SELECT"};
const char* dsKeyNames[] = {"A","B","Select","Start","Right","Left","Up","Down",
    "R","L","X","Y","Touch"};

const int NUM_DS_KEYS = sizeof(dsKeyNames)/sizeof(char*);
const int NUM_GB_KEYS = sizeof(gbKeyNames)/sizeof(char*);

int keys[NUM_GB_KEYS];

struct KeyConfig {
    char name[32];
    int gbKeys[13];
};
KeyConfig defaultKeyConfig = {
    "Main",
    {KEY_GB_A,KEY_GB_B,KEY_GB_SELECT,KEY_GB_START,KEY_GB_RIGHT,KEY_GB_LEFT,KEY_GB_UP,KEY_GB_DOWN,
        KEY_MENU,KEY_FAST_FORWARD,KEY_SAVE,KEY_SCALE,KEY_MENU}
};

std::vector<KeyConfig> keyConfigs;
unsigned int selectedKeyConfig=0;

// Return false if there is no key assigned to opening the menu (including touch).
bool checkKeyAssignedToMenu(KeyConfig* config) {
    for (int i=0; i<NUM_DS_KEYS; i++) {
        if (config->gbKeys[i] == KEY_MENU || config->gbKeys[i] == KEY_MENU_PAUSE) {
            return true;
        }
    }

    return false;
}

void loadKeyConfig() {
    KeyConfig* keyConfig = &keyConfigs[selectedKeyConfig];
    for (int i=0; i<NUM_GB_KEYS; i++)
        keys[i] = 0;
    for (int i=0; i<NUM_DS_KEYS; i++) {
        keys[keyConfig->gbKeys[i]] |= BIT(i);
    }

    // Set "touch" to open the menu if nothing is assigned to the menu
    if (!checkKeyAssignedToMenu(keyConfig))
        keyConfig->gbKeys[12] = KEY_MENU;
}

void controlsParseConfig(const char* line2) {
    char line[100];
    strncpy(line, line2, 100);
    line[99] = '\0';
    while (strlen(line) > 0 && (line[strlen(line)-1] == '\n' || line[strlen(line)-1] == ' '))
        line[strlen(line)-1] = '\0';
    if (line[0] == '(') {
        char* bracketEnd;
        if ((bracketEnd = strrchr(line, ')')) != 0) {
            *bracketEnd = '\0';
            const char* name = line+1;

            keyConfigs.push_back(KeyConfig());
            KeyConfig* config = &keyConfigs.back();
            strncpy(config->name, name, 32);
            config->name[31] = '\0';
            for (int i=0; i<NUM_DS_KEYS; i++)
                config->gbKeys[i] = KEY_NONE;
        }
        return;
    }
    char* equalsPos;
    if ((equalsPos = strrchr(line, '=')) != 0 && equalsPos != line+strlen(line)-1) {
        *equalsPos = '\0';

        if (strcasecmp(line, "config") == 0) {
            selectedKeyConfig = atoi(equalsPos+1);
        }
        else {
            int dsKey = -1;
            for (int i=0; i<NUM_DS_KEYS; i++) {
                if (strcasecmp(line, dsKeyNames[i]) == 0) {
                    dsKey = i;
                    break;
                }
            }
            int gbKey = -1;
            for (int i=0; i<NUM_GB_KEYS; i++) {
                if (strcasecmp(equalsPos+1, gbKeyNames[i]) == 0) {
                    gbKey = i;
                    break;
                }
            }

            if (gbKey != -1 && dsKey != -1) {
                KeyConfig* config = &keyConfigs.back();
                config->gbKeys[dsKey] = gbKey;
            }
        }
    }
}
void controlsPrintConfig(FILE* file) {
    fiprintf(file, "config=%d\n", selectedKeyConfig);
    for (unsigned int i=0; i<keyConfigs.size(); i++) {
        fiprintf(file, "(%s)\n", keyConfigs[i].name);
        for (int j=0; j<NUM_DS_KEYS; j++) {
            fiprintf(file, "%s=%s\n", dsKeyNames[j], gbKeyNames[keyConfigs[i].gbKeys[j]]);
        }
    }
}

int keyConfigChooser_option;
bool keyConfigChooser_printMenuWarning = false;

void redrawKeyConfigChooser() {
    int& option = keyConfigChooser_option;
    KeyConfig* config = &keyConfigs[selectedKeyConfig];

    consoleClear();

    iprintf("Config: ");
    if (option == -1)
        iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, "* %s *\n\n", config->name);
    else
        iprintf("  %s  \n\n", config->name);

    iprintf("    Button   Function\n\n");

    for (int i=0; i<NUM_DS_KEYS; i++) {
        int len = 8-strlen(dsKeyNames[i]);
        while (len > 0) {
            iprintf(" ");
            len--;
        }
        if (option == i)
            iprintfColored(CONSOLE_COLOR_LIGHT_YELLOW, "* %s | %s *\n", dsKeyNames[i], gbKeyNames[config->gbKeys[i]]);
        else
            iprintf("  %s | %s  \n", dsKeyNames[i], gbKeyNames[config->gbKeys[i]]);
    }
    iprintf("\n\nPress X to make a new config.");
    if (selectedKeyConfig != 0) /* can't erase the default */ {
        iprintf("\n\nPress Y to delete this config.");
    }
    if (keyConfigChooser_printMenuWarning)
        iprintf("\n\nNo key is assigned to the menu!");
}

void updateKeyConfigChooser() {
    bool redraw = false;

    int& option = keyConfigChooser_option;
    KeyConfig* config = &keyConfigs[selectedKeyConfig];

    if (keyJustPressed(KEY_B)) {
        // Don't allow exiting if nothing is assigned to opening the menu

        if (!checkKeyAssignedToMenu(config)) {
            keyConfigChooser_printMenuWarning = true;
            redrawKeyConfigChooser();
        }
        else {
            loadKeyConfig();
            closeSubMenu();
        }
    }
    else if (keyJustPressed(KEY_X)) {
        keyConfigs.push_back(KeyConfig(*config));
        selectedKeyConfig = keyConfigs.size()-1;
        char name[32];
        siprintf(name, "Custom %d", keyConfigs.size()-1);
        strcpy(keyConfigs.back().name, name);
        option = -1;
        redraw = true;
    }
    else if (keyJustPressed(KEY_Y)) {
        if (selectedKeyConfig != 0) /* can't erase the default */ {
            keyConfigs.erase(keyConfigs.begin() + selectedKeyConfig);
            if (selectedKeyConfig >= keyConfigs.size())
                selectedKeyConfig = keyConfigs.size() - 1;
            redraw = true;
        }
    }
    else if (keyPressedAutoRepeat(KEY_DOWN)) {
        if (option == NUM_DS_KEYS-1)
            option = -1;
        else
            option++;
        redraw = true;
    }
    else if (keyPressedAutoRepeat(KEY_UP)) {
        if (option == -1)
            option = NUM_DS_KEYS-1;
        else
            option--;
        redraw = true;
    }
    else if (keyPressedAutoRepeat(KEY_LEFT)) {
        if (option == -1) {
            if (selectedKeyConfig == 0)
                selectedKeyConfig = keyConfigs.size()-1;
            else
                selectedKeyConfig--;
        }
        else {
            config->gbKeys[option]--;
            if (config->gbKeys[option] < 0)
                config->gbKeys[option] = NUM_GB_KEYS-1;
        }
        redraw = true;
    }
    else if (keyPressedAutoRepeat(KEY_RIGHT)) {
        if (option == -1) {
            selectedKeyConfig++;
            if (selectedKeyConfig >= keyConfigs.size())
                selectedKeyConfig = 0;
        }
        else {
            config->gbKeys[option]++;
            if (config->gbKeys[option] >= NUM_GB_KEYS)
                config->gbKeys[option] = 0;
        }
        redraw = true;
    }
    if (redraw)
        doAtVBlank(redrawKeyConfigChooser);
}

void startKeyConfigChooser() {
    keyConfigChooser_option = -1;
    displaySubMenu(updateKeyConfigChooser);
    redrawKeyConfigChooser();
}

void generalParseConfig(const char* line) {
    char* equalsPos;
    if ((equalsPos = strrchr(line, '=')) != 0 && equalsPos != line+strlen(line)-1) {
        *equalsPos = '\0';
        const char* parameter = line;
        const char* value = equalsPos+1;

        if (strcasecmp(parameter, "rompath") == 0) {
            if (romPath != 0)
                free(romPath);
            romPath = (char*)malloc(strlen(value)+1);
            strcpy(romPath, value);
            romChooserState.directory = romPath;
        }
        else if (strcasecmp(parameter, "biosfile") == 0) {
            if (biosPath != 0)
                free(biosPath);
            biosPath = (char*)malloc(strlen(value)+1);
            strcpy(biosPath, value);
            loadBios(biosPath);
        }
        else if (strcasecmp(parameter, "borderfile") == 0) {
            if (borderPath != 0)
                free(borderPath);
            borderPath = (char*)malloc(strlen(value)+1);
            strcpy(borderPath, value);
        }
    }
    if (borderPath == NULL || *borderPath == '\0') {
        free(borderPath);
        borderPath = (char*)malloc(strlen("/border.bmp")+1);
        strcpy(borderPath, "/border.bmp");
    }
}

void generalPrintConfig(FILE* file) {
    if (romPath == 0)
        fiprintf(file, "rompath=\n");
    else
        fiprintf(file, "rompath=%s\n", romPath);
    if (biosPath == 0)
        fiprintf(file, "biosfile=\n");
    else
        fiprintf(file, "biosfile=%s\n", biosPath);
    if (borderPath == 0)
        fiprintf(file, "borderfile=\n");
    else
        fiprintf(file, "borderfile=%s\n", borderPath);
}

bool readConfigFile() {
    FILE* file = fopen("/gameyobds.ini", "r");
    char line[100];
    void (*configParser)(const char*) = generalParseConfig;

    if (file == NULL)
        goto end;

    while (!feof(file)) {
        fgets(line, 100, file);
        char c=0;
        while (*line != '\0' && ((c = line[strlen(line)-1]) == ' ' || c == '\n' || c == '\r'))
            line[strlen(line)-1] = '\0';
        if (line[0] == '[') {
            char* endBrace;
            if ((endBrace = strrchr(line, ']')) != 0) {
                *endBrace = '\0';
                const char* section = line+1;
                if (strcasecmp(section, "general") == 0) {
                    configParser = generalParseConfig;
                }
                else if (strcasecmp(section, "console") == 0) {
                    configParser = menuParseConfig;
                }
                else if (strcasecmp(section, "controls") == 0) {
                    configParser = controlsParseConfig;
                }
            }
        }
        else
            configParser(line);
    }
    fclose(file);
end:
    if (keyConfigs.empty())
        keyConfigs.push_back(defaultKeyConfig);
    if (selectedKeyConfig >= keyConfigs.size())
        selectedKeyConfig = 0;
    loadKeyConfig();

    return file != NULL;
}

void writeConfigFile() {
    FILE* file = fopen("/gameyobds.ini", "w");
    fiprintf(file, "[general]\n");
    generalPrintConfig(file);
    fiprintf(file, "[console]\n");
    menuPrintConfig(file);
    fiprintf(file, "[controls]\n");
    controlsPrintConfig(file);
    fclose(file);

    char nameBuf[100];
    siprintf(nameBuf, "%s.cht", basename);
    saveCheats(nameBuf);
}


void loadBios(const char* filename) {
    FILE* file = fopen(filename, "rb");
    biosExists = file != NULL;
    if (biosExists)
        fread(bios, 1, 0x900, file);
}

int loadRom(char* f)
{
    if (romFile != NULL)
        fclose(romFile);
    strcpy(filename, f);

    // Check if this is a GBS file
    gbsMode = (strcasecmp(strrchr(filename, '.'), ".gbs") == 0);

    romFile = fopen(filename, "rb");
    if (romFile == NULL)
    {
        printLog("Error opening %s.\n", filename);
        return 1;
    }

    if (gbsMode) {
        fread(gbsHeader, 1, 0x70, romFile);
        gbsReadHeader();
        fseek(romFile, 0, SEEK_END);
        numRomBanks = (ftell(romFile)-0x70+0x3fff)/0x4000; // Get number of banks, rounded up
    }
    else {
        fseek(romFile, 0, SEEK_END);
        numRomBanks = (ftell(romFile)+0x3fff)/0x4000; // Get number of banks, rounded up
    }

    // Round numRomBanks to a power of 2
    int n=1;
    while (n < numRomBanks) n*=2;
    numRomBanks = n;

    //int rawRomSize = ftell(romFile);
    rewind(romFile);


    if (numRomBanks <= maxLoadedRomBanks)
        numLoadedRomBanks = numRomBanks;
    else
        numLoadedRomBanks = maxLoadedRomBanks;

    romSlot0 = romBankSlots;
    romSlot1 = romBankSlots + 0x4000;

    for (int i=0; i<numRomBanks; i++) {
        bankSlotIDs[i] = -1;
    }

    // Load rom banks and initialize all those "bank" arrays
    lastBanksUsed = std::vector<int>();
    // Read bank 0
    if (gbsMode) {
        bankSlotIDs[0] = 0;
        fseek(romFile, 0x70, SEEK_SET);
        fread(romBankSlots+gbsLoadAddress, 1, 0x4000-gbsLoadAddress, romFile);
    }
    else {
        bankSlotIDs[0] = 0;
        fseek(romFile, 0, SEEK_SET);
        fread(romBankSlots, 1, 0x4000, romFile);
    }
    // Read the rest of the banks
    for (int i=1; i<numLoadedRomBanks; i++) {
        bankSlotIDs[i] = i;
        fread(romBankSlots+0x4000*i, 1, 0x4000, romFile);
        lastBanksUsed.push_back(i);
    }

    strcpy(basename, filename);
    *(strrchr(basename, '.')) = '\0';
    strcpy(savename, basename);
    strcat(savename, ".sav");

    cgbFlag = romSlot0[0x143];
    romSize = romSlot0[0x148];
    ramSize = romSlot0[0x149];
    mapper  = romSlot0[0x147];

    int nameLength = 16;
    if (cgbFlag == 0x80 || cgbFlag == 0xc0)
        nameLength = 15;
    for (int i=0; i<nameLength; i++)
        romTitle[i] = (char)romSlot0[i+0x134];
    romTitle[nameLength] = '\0';

    hasRumble = false;

    if (gbsMode) {
        MBC = MBC5;
        loadCheats(""); // Unloads previous cheats
    }
    else {
        switch (mapper) {
            case 0: case 8: case 9:
                MBC = MBC0;
                break;
            case 1: case 2: case 3:
                MBC = MBC1;
                break;
            case 5: case 6:
                MBC = MBC2;
                break;
                //case 0xb: case 0xc: case 0xd:
                //MBC = MMM01;
                //break;
            case 0xf: case 0x10: case 0x11: case 0x12: case 0x13:
                MBC = MBC3;
                break;
                //case 0x15: case 0x16: case 0x17:
                //MBC = MBC4;
                //break;
            case 0x19: case 0x1a: case 0x1b:
                MBC = MBC5;
                break;
            case 0x1c: case 0x1d: case 0x1e:
                MBC = MBC5;
                hasRumble = true;
                break;
            case 0x22:
                MBC = MBC7;
                break;
            case 0xea: /* Hack for SONIC5 */
                MBC = MBC1;
                break;
            case 0xfe:
                MBC = HUC3;
                break;
            case 0xff:
                MBC = HUC1;
                break;
            default:
                printLog("Unsupported MBC %02x\n", mapper);
                return 1;
        }

        // Little hack to preserve "quickread" from gbcpu.cpp.
        if (biosExists) {
            for (int i=0x100; i<0x150; i++)
                bios[i] = romSlot0[i];
        }

        suspendStateExists = checkStateExists(-1);

        // Load cheats
        char nameBuf[100];
        siprintf(nameBuf, "%s.cht", basename);
        loadCheats(nameBuf);

    } // !gbsMode

    // If we've loaded everything, close the rom file
    if (numRomBanks <= numLoadedRomBanks) {
        fclose(romFile);
        romFile = NULL;
    }

    loadSave();

    return 0;
}

void loadRomBank() {
    if (bankSlotIDs[romBank] != -1 || numRomBanks <= numLoadedRomBanks || romBank == 0) {
        romSlot1 = romBankSlots+bankSlotIDs[romBank]*0x4000;
        return;
    }
    int bankToUnload = lastBanksUsed.back();
    lastBanksUsed.pop_back();
    int slot = bankSlotIDs[bankToUnload];
    bankSlotIDs[bankToUnload] = -1;
    bankSlotIDs[romBank] = slot;

    fseek(romFile, 0x4000*romBank, SEEK_SET);
    fread(romBankSlots+slot*0x4000, 1, 0x4000, romFile);

    lastBanksUsed.insert(lastBanksUsed.begin(), romBank);

    applyGGCheatsToBank(romBank);

    romSlot1 = romBankSlots+slot*0x4000;
}

bool isRomBankLoaded(int bank) {
    return bankSlotIDs[bank] != -1;
}
u8* getRomBank(int bank) {
    if (!isRomBankLoaded(bank))
        return 0;
    return romBankSlots+bankSlotIDs[bank]*0x4000;
}

const char* getRomBasename() {
    return basename;
}

void unloadRom() {
    doAtVBlank(clearGFX);

    gameboySyncAutosave();
    if (saveFile != NULL)
        fclose(saveFile);
    saveFile = NULL;
    // unload previous save
    if (externRam != NULL) {
        free(externRam);
        externRam = NULL;
    }
}

int loadSave()
{
    if (saveFile != NULL) {
        fclose(saveFile);
        saveFile = NULL;
    }
    if (externRam != NULL) {
        free(externRam);
        externRam = NULL;
    }

    if (gbsMode)
        numRamBanks = 1;
    else {
        // Get the game's external memory size and allocate the memory
        switch(ramSize)
        {
            case 0:
                numRamBanks = 0;
                break;
            case 1:
            case 2:
                numRamBanks = 1;
                break;
            case 3:
                numRamBanks = 4;
                break;
            case 4:
                numRamBanks = 16;
                break;
            default:
                printLog("Invalid RAM bank number: %x\nDefaulting to 4 banks\n", ramSize);
                numRamBanks = 4;
                break;
        }
        if (MBC == MBC2)
            numRamBanks = 1;
    }

    if (numRamBanks == 0)
        return 0;

    externRam = (u8*)malloc(numRamBanks*0x2000);

    if (gbsMode)
        return 0; // GBS files don't get to save.

    // Now load the data.
    saveFile = fopen(savename, "r+b");
    int neededFileSize = numRamBanks*0x2000;
    if (MBC == MBC3 || MBC == HUC3)
        neededFileSize += sizeof(clockStruct);

    int fileSize = 0;
    if (saveFile) {
        fseek(saveFile, 0, SEEK_END);
        fileSize = ftell(saveFile);
        fseek(saveFile, 0, SEEK_SET);
    }

    if (!saveFile || fileSize < neededFileSize) {
        fclose(saveFile);

        // Extend the size of the file, or create it
        if (!saveFile) {
            saveFile = fopen(savename, "wb");
            fseek(saveFile, neededFileSize-1, SEEK_SET);
            fputc(0, saveFile);
        }
        else {
            saveFile = fopen(savename, "ab");
            for (; fileSize<neededFileSize; fileSize++)
                fputc(0, saveFile);
        }
        fclose(saveFile);

        saveFile = fopen(savename, "r+b");
    }

    fread(externRam, 1, 0x2000*numRamBanks, saveFile);

    switch (MBC) {
        case MBC3:
        case HUC3:
            fread(&gbClock, 1, sizeof(gbClock), saveFile);
            break;
    }
    saveGame();
    return 0;
}

int saveGame()
{
    if (numRamBanks == 0 || saveFile == NULL)
        return 0;

    printLog("Full game save\n");

    fseek(saveFile, 0, SEEK_SET);

    fwrite(externRam, 1, 0x2000*numRamBanks, saveFile);

    switch (MBC) {
        case MBC3:
        case HUC3:
            fwrite(&gbClock, 1, sizeof(gbClock), saveFile);
            break;
    }

    flushFatCache();

    return 0;
}

bool wroteToSramThisFrame=false;
int framesSinceAutosaveStarted=0;

void gameboySyncAutosave() {
    if (!autosaveStarted)
        return;

    numSaveWrites = 0;
    wroteToSramThisFrame = false;

    int numSectors = 0;
    int lastWritten = -2;

    // iterate over each sector
    for (int i=0; i<numRamBanks*0x2000/AUTOSAVE_SECTOR_SIZE; i++) {
        if (dirtySectors[i]) {

            if (lastWritten+1 != i)
                fseek(saveFile, i*AUTOSAVE_SECTOR_SIZE, SEEK_SET);

            fwrite(externRam+i*AUTOSAVE_SECTOR_SIZE, AUTOSAVE_SECTOR_SIZE, 1, saveFile);

            lastWritten = i;
            dirtySectors[i] = false;
            numSectors++;
        }
    }
    printLog("SAVE %d sectors\n", numSectors);

    fclose(saveFile);
    saveFile = fopen(savename, "r+b");

    framesSinceAutosaveStarted = 0;
    autosaveStarted = false;
}

void updateAutosave() {
    if (autosaveStarted)
        framesSinceAutosaveStarted++;

    if (!(fastForwardMode || fastForwardKey)){
        if (framesSinceAutosaveStarted >= 120 ||     // Executes when sram is written to for 120 consecutive frames, or
            (!saveModified && wroteToSramThisFrame)) { // when a full frame has passed since sram was last written to.
            gameboySyncAutosave();
        }
    }
    else{
        if (framesSinceAutosaveStarted >= 960 ||     // Executes when sram is written to for 120 consecutive frames, or
            (!saveModified && wroteToSramThisFrame)) { // when a full frame has passed since sram was last written to.
            gameboySyncAutosave();
        }
    }
    
    if (saveModified) {
        wroteToSramThisFrame = true;
        autosaveStarted = true;
        saveModified = false;
    }
}


bool keyPressed(int key) {
    return keysPressed&key;
}
bool keyPressedAutoRepeat(int key) {
    if (keyJustPressed(key)) {
        repeatStartTimer = 14;
        return true;
    }
    if (keyPressed(key) && repeatStartTimer == 0 && repeatTimer == 0) {
        repeatTimer = 2;
        return true;
    }
    return false;
}
bool keyJustPressed(int key) {
    return ((keysPressed^lastKeysPressed)&keysPressed) & key;
}

int readKeysLastFrameCounter=0;
void readKeys() {
    scanKeys();

    lastKeysPressed = keysPressed;
    keysPressed = keysHeld();
    for (int i=0; i<16; i++) {
        if (keysForceReleased & (1<<i)) {
            if (!(keysPressed & (1<<i)))
                keysForceReleased &= ~(1<<i);
        }
    }
    keysPressed &= ~keysForceReleased;

    if (dsFrameCounter != readKeysLastFrameCounter) { // Double-check that it's been 1/60th of a second
        if (repeatStartTimer > 0)
            repeatStartTimer--;
        if (repeatTimer > 0)
            repeatTimer--;
        readKeysLastFrameCounter = dsFrameCounter;
    }
}

void forceReleaseKey(int key) {
    keysForceReleased |= key;
    keysPressed &= ~key;
}

int mapGbKey(int gbKey) {
    return keys[gbKey];
}

char* getRomTitle() {
    return romTitle;
}

const char *mbcName[] = {"ROM","MBC1","MBC2","MBC3","MBC4","MBC5","MBC7","HUC3","HUC1"};

void printRomInfo() {
    consoleClear();
    iprintf("ROM Title: \"%s\"\n", romTitle);
    iprintf("Cartridge type: %.2x (%s)\n", mapper, mbcName[MBC]);
    iprintf("ROM Size: %.2x (%d banks)\n", romSize, numRomBanks);
    iprintf("RAM Size: %.2x (%d banks)\n", ramSize, numRamBanks);
}

const int STATE_VERSION = 5;

struct StateStruct {
    // version
    // bg/sprite PaletteData
    // vram
    // wram
    // hram
    // sram
    Registers regs;
    int halt, ime;
    bool doubleSpeed, biosOn;
    int gbMode;
    int romBank, ramBank, wramBank, vramBank;
    int memoryModel;
    clockStruct clock;
    int scanlineCounter, timerCounter, phaseCounter, dividerCounter;
    // v2
    int serialCounter;
    // v3
    bool ramEnabled;
    // MBC-specific stuff
    // v4
    //  bool sgbMode;
    //  If sgbMode == true:
    //   int sgbPacketLength;
    //   int sgbPacketsTransferred;
    //   int sgbPacketBit;
    //   u8 sgbCommand;
    //   u8 gfxMask;
    //   u8[20*18] sgbMap;
};

void saveState(int stateNum) {
    FILE* outFile;
    StateStruct state;
    char statename[100];

    if (stateNum == -1)
        siprintf(statename, "%s.yss", basename);
    else
        siprintf(statename, "%s.ys%d", basename, stateNum);
    outFile = fopen(statename, "w");

    if (outFile == 0) {
        printMenuMessage("Error opening file for writing.");
        return;
    }

    state.regs = gbRegs;
    state.halt = halt;
    state.ime = ime;
    state.doubleSpeed = doubleSpeed;
    state.biosOn = biosOn;
    state.gbMode = gbMode;
    state.romBank = romBank;
    state.ramBank = currentRamBank;
    state.wramBank = wramBank;
    state.vramBank = vramBank;
    state.memoryModel = memoryModel;
    state.clock = gbClock;
    state.scanlineCounter = scanlineCounter;
    state.timerCounter = timerCounter;
    state.phaseCounter = phaseCounter;
    state.dividerCounter = dividerCounter;
    state.serialCounter = serialCounter;
    state.ramEnabled = ramEnabled;

    fwrite(&STATE_VERSION, sizeof(int), 1, outFile);
    fwrite((char*)bgPaletteData, 1, sizeof(bgPaletteData), outFile);
    fwrite((char*)sprPaletteData, 1, sizeof(sprPaletteData), outFile);
    fwrite((char*)vram, 1, sizeof(vram), outFile);
    fwrite((char*)wram, 1, sizeof(wram), outFile);
    fwrite((char*)hram, 1, 0x200, outFile);
    fwrite((char*)externRam, 1, 0x2000*numRamBanks, outFile);

    fwrite((char*)&state, 1, sizeof(StateStruct), outFile);

    switch (MBC) {
        case HUC3:
            fwrite(&HuC3Mode,  1, sizeof(u8), outFile);
            fwrite(&HuC3Value, 1, sizeof(u8), outFile);
            fwrite(&HuC3Shift, 1, sizeof(u8), outFile);
            break;
    }

    fwrite(&sgbMode, 1, sizeof(bool), outFile);
    if (sgbMode) {
        fwrite(&sgbPacketLength, 1, sizeof(int), outFile);
        fwrite(&sgbPacketsTransferred, 1, sizeof(int), outFile);
        fwrite(&sgbPacketBit, 1, sizeof(int), outFile);
        fwrite(&sgbCommand, 1, sizeof(u8), outFile);
        fwrite(&gfxMask, 1, sizeof(u8), outFile);
        fwrite(sgbMap, 1, sizeof(sgbMap), outFile);
    }

    fclose(outFile);
}

int loadState(int stateNum) {
    FILE *inFile;
    StateStruct state;
    char statename[100];
    int version;

    memset(&state, 0, sizeof(StateStruct));

    if (stateNum == -1)
        siprintf(statename, "%s.yss", basename);
    else
        siprintf(statename, "%s.ys%d", basename, stateNum);
    inFile = fopen(statename, "r");

    if (inFile == 0) {
        printMenuMessage("State doesn't exist.");
        return 1;
    }

    fread(&version, sizeof(int), 1, inFile);

    if (version == 0 || version > STATE_VERSION) {
        printMenuMessage("State is from an incompatible version.");
        return 1;
    }

    fread((char*)bgPaletteData, 1, sizeof(bgPaletteData), inFile);
    fread((char*)sprPaletteData, 1, sizeof(sprPaletteData), inFile);
    fread((char*)vram, 1, sizeof(vram), inFile);
    fread((char*)wram, 1, sizeof(wram), inFile);
    fread((char*)hram, 1, 0x200, inFile);

    if (version <= 4 && ramSize == 0x04)
        // Value "0x04" for ram size wasn't interpreted correctly before
        fread((char*)externRam, 1, 0x2000*4, inFile);
    else
        fread((char*)externRam, 1, 0x2000*numRamBanks, inFile);

    fread((char*)&state, 1, sizeof(StateStruct), inFile);

    /* MBC-specific values have been introduced in v3 */
    if (version >= 3) {
        switch (MBC) {
            case MBC3:
                if (version == 3) {
                    u8 rtcReg;
                    fread(&rtcReg, 1, sizeof(u8), inFile);
                    if (rtcReg != 0)
                        currentRamBank = rtcReg;
                }
                break;
            case HUC3:
                fread(&HuC3Mode,  1, sizeof(u8), inFile);
                fread(&HuC3Value, 1, sizeof(u8), inFile);
                fread(&HuC3Shift, 1, sizeof(u8), inFile);
                break;
        }

        fread(&sgbMode, 1, sizeof(bool), inFile);
        if (sgbMode) {
            fread(&sgbPacketLength, 1, sizeof(int), inFile);
            fread(&sgbPacketsTransferred, 1, sizeof(int), inFile);
            fread(&sgbPacketBit, 1, sizeof(int), inFile);
            fread(&sgbCommand, 1, sizeof(u8), inFile);
            fread(&gfxMask, 1, sizeof(u8), inFile);
            fread(sgbMap, 1, sizeof(sgbMap), inFile);
        }
    }
    else
        sgbMode = false;


    fclose(inFile);
    if (stateNum == -1) {
        unlink(statename);
        suspendStateExists = false;
    }

    gbRegs = state.regs;
    halt = state.halt;
    ime = state.ime;
    doubleSpeed = state.doubleSpeed;
    biosOn = state.biosOn;
    if (!biosExists)
        biosOn = false;
    gbMode = state.gbMode;
    romBank = state.romBank;
    currentRamBank = state.ramBank;
    wramBank = state.wramBank;
    vramBank = state.vramBank;
    memoryModel = state.memoryModel;
    gbClock = state.clock;
    scanlineCounter = state.scanlineCounter;
    timerCounter = state.timerCounter;
    phaseCounter = state.phaseCounter;
    dividerCounter = state.dividerCounter;
    serialCounter = state.serialCounter;
    ramEnabled = state.ramEnabled;
    if (version < 3)
        ramEnabled = true;

    transferReady = false;
    timerPeriod = periods[ioRam[0x07]&0x3];
    cyclesToEvent = 1;

    mapMemory();
    setDoubleSpeed(doubleSpeed);


    if (autoSavingEnabled && stateNum != -1)
        saveGame(); // Synchronize save file on sd with file in ram

    refreshGFX();
    refreshSND();

    return 0;
}

void deleteState(int stateNum) {
    if (!checkStateExists(stateNum))
        return;

    char statename[100];

    if (stateNum == -1)
        siprintf(statename, "%s.yss", basename);
    else
        siprintf(statename, "%s.ys%d", basename, stateNum);
    unlink(statename);
}

bool checkStateExists(int stateNum) {
    char statename[256];

    if (stateNum == -1)
        siprintf(statename, "%s.yss", basename);
    else
        siprintf(statename, "%s.ys%d", basename, stateNum);
    return access(statename, R_OK) == 0;
    /*
    file = fopen(statename, "r");

    if (file == 0) {
        return false;
    }
    fclose(file);
    return true;
    */
}


