#include "printerIcon.h"
#include "inputhelper.h"
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <vector>
#include <nds.h>
#include <stdio.h>
#include <math.h>
#include "gbs.h"
#include "gbgfx.h"
#include "mmu.h"
#include "gbcpu.h"
#include "main.h"
#include "gameboy.h"
#include "sgb.h"
#include "console.h"
#include "filechooser.h"
#include "inputhelper.h"
#include "common.h"
#include "gbsnd.h"

#define BACKDROP_COLOUR RGB15(0, 0, 0)

const int map_base[] = {1, 2};
const int color0_map_base[] = {3, 4};
const int overlay_map_base[] = {5, 6};
const int off_map_base = 7;
const int border_map_base = 8;

const int OFF_MAP_PALETTE_INDEX = 15 * 16 + 1;

u16 *const map[2] = {BG_MAP_RAM(map_base[0]), BG_MAP_RAM(map_base[1])};
u16 *const color0Map[2] = {BG_MAP_RAM(color0_map_base[0]), BG_MAP_RAM(color0_map_base[1])};
u16 *const overlayMap[2] = {BG_MAP_RAM(overlay_map_base[0]), BG_MAP_RAM(overlay_map_base[1])};
u16 *const offMap = BG_MAP_RAM(off_map_base);
u16 *const borderMap = BG_MAP_RAM(border_map_base);

const int firstGbSprite = 128 - 40;

const int spr_priority = 2;
const int spr_priority_low = 3;

const int win_color0_priority = 3;
const int win_priority = 2;

const int bg_color0_priority = 3;
const int bg_priority = 2;

const int bg_overlay_priority = 1;
const int win_overlay_priority = 1;

const int bg_all_priority = 2;
const int win_all_priority = 2;

const int screenOffsX = 48;
const int screenOffsY = 24;

bool gbGraphicsDisabled = false;

// If the game is using the GBC's priority bit for tiles, the corresponding
// variable is set. This helps with layer management.
int usingTilePriority[2];

bool didVblank = false;
// Frame counter. Incremented each vblank.
volatile int dsFrameCounter = 0;

bool changedTile[2][0x180];
int changedTileQueueLength = 0;
u16 changedTileQueue[0x300];

bool changedTileInFrame[2][0x180];
int changedTileInFrameQueueLength = 0;
u16 changedTileInFrameQueue[0x300];

bool changedMap[2][0x400];
int changedMapQueueLength[2];
u16 changedMapQueue[2][0x400];

u8 bgPaletteData[0x40] ALIGN(4);
u8 sprPaletteData[0x40] ALIGN(4);

int winPosY = 0;

bool lineModified;
bool mapsModified;
bool bgPalettesModified;
bool sprPalettesModified;
bool spritesModified;

// Whether to wait for vblank if emulation is running behind schedule
int interruptWaitMode = 0;

bool windowDisabled = false;
bool hblankDisabled = false;

int scaleMode;
int scaleFilter = 1;
u8 gfxMask;

volatile int loadedBorderType; // This is read from hblank
bool customBorderExists = true;
bool sgbBorderLoaded;

int SCALE_BGX, SCALE_BGY;

bool lastScreenDisabled;
bool screenDisabled;

void drawSprites(u8 *data, int tallSprites);
void drawTile(int tile, int bank);
void updateTiles();
void updateTileMaps();
void updateTileMap(int map, int i);
void updateBgPalette(int paletteid, u8 *data, u8 dmgPal);
void updateBgPalette_GBC(int paletteid, u8 *data);
void updateSprPalette(int paletteid, u8 *data, u8 dmgPal);

bool lineCompleted[144];
typedef struct
{
    u8 bgPaletteData[0x40] ALIGN(4);
    u8 sprPaletteData[0x40] ALIGN(4);
    bool modified;
    bool mapsModified;
    u8 hofs;
    u8 vofs;
    u8 winX;
    u8 winY;
    u8 winPosY;
    u8 winOn;
    u16 bgColor0Cnt;
    u16 bgCnt;
    u16 bgOverlayCnt;
    u16 bgAllCnt;
    u16 winColor0Cnt;  // Contains color 0 only
    u16 winCnt;        // Contains colors 1-3
    u16 winOverlayCnt; // Contains colors 1-3, only for 'priority' tiles
    u16 winAllCnt;     // Contains colors 0-3. Used when available layers are limited.
    u8 bgMap, winMap;
    bool spritesOn;
    bool tileSigned;
    u8 bgPal;
    u8 sprPal[2];
    bool bgPalettesModified;
    bool sprPalettesModified;
    bool spritesModified;
    u8 spriteData[0xa0];
    int tallSprites;
    int bgHash;
} ScanlineStruct;

ScanlineStruct scanlineBuffers[2][144];

ScanlineStruct *drawingState = scanlineBuffers[0];
ScanlineStruct *renderingState = scanlineBuffers[1];

typedef struct
{
    u16 attr0;
    u16 attr1;
    u16 attr2;
    u16 affine_data;
} spriteEntry;

#define sprites ((spriteEntry *)OAM)

#define BG_CNT(x) (*((vu16 *)0x4000008 + (x)))
#define BG_HOFS(x) (*((vu16 *)0x4000010 + (x)*2))
#define BG_VOFS(x) (*((vu16 *)0x4000012 + (x)*2))

// The graphics are drawn with the DS's native hardware.
// Games tend to modify the graphics in the middle of being drawn.
// These changes are recorded and applied during DS hblank.

void drawLine(int gbLine) ITCM_CODE;

/**
 * @brief Draws a line on the screen, updating the state and palettes for each line.
 *
 * This function is responsible for updating the state of the screen on a line-by-line basis,
 * including managing screen/window layers and updating palettes for the background and sprites.
 * It also manages drawing the sprites for each line.
 *
 * @param gbLine The current line number to be drawn on the screen.
 */
void drawLine(int gbLine)
{
    ScanlineStruct *state = &drawingState[gbLine];

    if (screenDisabled)
    {
        REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE;
        REG_BG0CNT = BG_MAP_BASE(off_map_base);
        if (loadedBorderType)
            WIN_IN |= 7 << 8;
        else
            WIN_IN |= 15 << 8;
        return;
    }
    if (state->mapsModified)
    {
        if (state->spritesOn)
            REG_DISPCNT |= DISPLAY_SPR_ACTIVE;
        else
            REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE;

        bool useWin = state->winOn && !windowDisabled;
        int layer = 0;
        int bgLayers, winLayers;
        if (useWin)
        {
            if (state->winX > 7)
            {
                if (loadedBorderType == BORDER_NONE)
                {
                    winLayers = 2;
                    bgLayers = 2;
                }
                else
                {
                    if (usingTilePriority[state->bgMap])
                    {
                        winLayers = 1;
                        bgLayers = 2;
                    }
                    else if (usingTilePriority[state->winMap])
                    {
                        winLayers = 2;
                        bgLayers = 1;
                    }
                    else
                    {
                        winLayers = 1;
                        bgLayers = 2;
                    }
                }
            }
            else
            {
                winLayers = 3;
                bgLayers = 0;
            }
        }
        else
        {
            winLayers = 0;
            bgLayers = 3;
            WIN0_X0 = screenOffsX + 160;
            WIN0_Y0 = screenOffsY;
        }
        WIN_IN |= 15 << 8;

        if (winLayers != 0)
        {
            int whofs = -(state->winX - 7) - screenOffsX;
            int wvofs = -(gbLine - state->winPosY) - screenOffsY;
            if (gbMode == GB)
            {
                // In GB mode, BG and Window share the backdrop color.
                // So they don't need their own dedicated layers.
                BG_CNT(layer) = state->winCnt;
                BG_HOFS(layer) = whofs;
                BG_VOFS(layer++) = wvofs;
            }
            else if (winLayers == 1)
            {
                BG_CNT(layer) = state->winAllCnt;
                BG_HOFS(layer) = whofs;
                BG_VOFS(layer++) = wvofs;
            }
            else if (winLayers == 2)
            {
                if (usingTilePriority[state->winMap])
                {
                    // Apply tile priority bit.
                    BG_CNT(layer) = state->winAllCnt;
                    BG_HOFS(layer) = whofs;
                    BG_VOFS(layer++) = wvofs;
                    BG_CNT(layer) = state->winOverlayCnt;
                    BG_HOFS(layer) = whofs;
                    BG_VOFS(layer++) = wvofs;
                }
                else
                {
                    // Apply sprite priority bit.
                    BG_CNT(layer) = state->winColor0Cnt;
                    BG_HOFS(layer) = whofs;
                    BG_VOFS(layer++) = wvofs;
                    BG_CNT(layer) = state->winCnt;
                    BG_HOFS(layer) = whofs;
                    BG_VOFS(layer++) = wvofs;
                }
            }
            else
            { // winLayers == 3
                // Apply both tile and sprite priority bits.
                BG_CNT(layer) = state->winColor0Cnt;
                BG_HOFS(layer) = whofs;
                BG_VOFS(layer++) = wvofs;
                BG_CNT(layer) = state->winCnt;
                BG_HOFS(layer) = whofs;
                BG_VOFS(layer++) = wvofs;
                BG_CNT(layer) = state->winOverlayCnt;
                BG_HOFS(layer) = whofs;
                BG_VOFS(layer++) = wvofs;
            }

            if (state->winX <= 7)
                WIN0_X0 = screenOffsX;
            else
                WIN0_X0 = state->winX - 7 + screenOffsX;
            WIN_IN &= ~15;
            for (int i = 0; i < layer; i++)
            {
                WIN_IN &= ~(1 << (8 + i));
                WIN_IN |= 1 << i;
            }
        }
        if (bgLayers != 0)
        {
            int hofs = state->hofs - screenOffsX;
            int vofs = state->vofs - screenOffsY;
            if (gbMode == GB)
            {
                BG_CNT(layer) = state->bgCnt;
                BG_HOFS(layer) = hofs;
                BG_VOFS(layer++) = vofs;
            }
            else
            {
                if (bgLayers >= 3)
                {
                    BG_CNT(layer) = state->bgColor0Cnt;
                    BG_HOFS(layer) = hofs;
                    BG_VOFS(layer++) = vofs;
                    BG_CNT(layer) = state->bgCnt;
                    BG_HOFS(layer) = hofs;
                    BG_VOFS(layer++) = vofs;
                    BG_CNT(layer) = state->bgOverlayCnt;
                    BG_HOFS(layer) = hofs;
                    BG_VOFS(layer++) = vofs;
                }
                else if (bgLayers == 2)
                { // not enough for both priority bits.
                    if (usingTilePriority[state->bgMap])
                    {
                        // Apply tile priority bit.
                        BG_CNT(layer) = state->bgAllCnt;
                        BG_HOFS(layer) = hofs;
                        BG_VOFS(layer++) = vofs;
                        BG_CNT(layer) = state->bgOverlayCnt;
                        BG_HOFS(layer) = hofs;
                        BG_VOFS(layer++) = vofs;
                    }
                    else
                    {
                        // Apply sprite priority bit.
                        BG_CNT(layer) = state->bgColor0Cnt;
                        BG_HOFS(layer) = hofs;
                        BG_VOFS(layer++) = vofs;
                        BG_CNT(layer) = state->bgCnt;
                        BG_HOFS(layer) = hofs;
                        BG_VOFS(layer++) = vofs;
                    }
                }
                else
                { // bgLayers == 1
                    BG_CNT(layer) = state->bgAllCnt;
                    BG_HOFS(layer) = hofs;
                    BG_VOFS(layer++) = vofs;
                }
            }
        }
        if (gbMode == GB)
        {
            // This layer provides the backdrop for both BG and Window.
            WIN_IN |= (1 << layer); // Must be displayed over window
            BG_CNT(layer) = state->winColor0Cnt;
            BG_HOFS(layer) = 0;
            BG_VOFS(layer++) = 0;
        }
        while (layer < 4)
        {
            WIN_IN &= ~((1 << layer) << 8);
            WIN_IN &= ~(1 << layer++);
        }
    }

    if (state->bgPalettesModified)
    {
        if (gbMode == CGB)
        {
            for (int i = 0; i < 8; i++)
                updateBgPalette_GBC(i, state->bgPaletteData);
        }
        else
        {
            for (int i = 0; i < 4; i++)
            {
                updateBgPalette(i, state->bgPaletteData, state->bgPal);
            }
        }
    }
    if (state->sprPalettesModified)
    {
        if (gbMode == GB)
        {
            for (int i = 0; i < 4; i++)
            {
                updateSprPalette(i, state->sprPaletteData, state->sprPal[0]);
                updateSprPalette(i + 4, state->sprPaletteData, state->sprPal[1]);
            }
        }
        else
        {
            for (int i = 0; i < 8; i++)
            {
                updateSprPalette(i, state->sprPaletteData, 0);
            }
        }
    }

    if (state->spritesModified)
        drawSprites(state->spriteData, state->tallSprites);
}

void doHBlank(int line) ITCM_CODE;

/**
 * @brief Perform horizontal blank processing for a given line.
 * 
 * This function is called during the horizontal blank period of the Game Boy's screen refresh cycle. It checks if graphics
 * are disabled, the current line is outside the valid range, or if H-blank processing is disabled. If none of these
 * conditions apply, it calculates the Game Boy line number for the current line, checks if the previous line was modified
 * but not yet drawn, and draws the current line if it has been modified.
 * 
 * Additionally, if a file chooser or menu is currently active, this function will change the backdrop color for the current
 * row to highlight the selected option.
 * 
 * @param line The current line.
 */
void doHBlank(int line)
{
    if (line >= 192)
        return;

    // Highlight the selected row in the file chooser or menu.
    if ((isFileChooserOn() || isMenuOn()) && line % 8 == 0)
    {
        if (line / 8 == consoleSelectedRow)
            setBackdropColorSub(RGB15(0, 0, 31)); // Highlight the selected row.
        else
            setBackdropColorSub(RGB15(0, 0, 0));
    }

    if (gbGraphicsDisabled || hblankDisabled)
        return;

    int gbLine = line - screenOffsY;
    if (gbLine >= 144 || gbLine <= 0)
        return;

    // Draw the previous line if it was modified and not yet drawn.
    if (drawingState[gbLine - 1].modified && !lineCompleted[gbLine - 1])
        drawLine(gbLine - 1);

    // Return if the current line has not been modified.
    if (!drawingState[gbLine].modified)
        return;

    lineCompleted[gbLine] = true;
    drawLine(gbLine);
}

void hblankHandler() ITCM_CODE;

/**
 * @brief Handle horizontal blank interrupt.
 *
 * This function is called during the horizontal blank interrupt and retrieves the current line number using REG_VCOUNT.
 * If the screen is in a non-VBlank mode, it decrements the line number by one. It then calls the doHBlank function
 * to perform horizontal blank processing for the given line.
 *
 */
void hblankHandler()
{
    int line = REG_VCOUNT + 1;

    // Decrement line number if not in VBlank mode.
    if ((REG_DISPSTAT & 3) != 2)
        line--;

    // Call doHBlank function to perform horizontal blank processing for the given line.
    doHBlank(line);
}

/**
 * @brief Handle vcount interrupt, which is triggered on line 235 (middle of vblank).
 * This function is called during the vcount interrupt and sets the vram banks to the appropriate sub backgrounds
 * based on whether scaling is on or not. It then performs hblank processing for the very top line (physical line 0)
 * and draws the first Game Boy line early (physical line 24) if it has been modified. If graphics are disabled, the function
 * returns.
*/
void vcountHandler()
{
    // Set vram banks for sub backgrounds.
    vramSetBankC(VRAM_C_SUB_BG);
    if (sharedData->scalingOn)
        vramSetBankD(VRAM_D_LCD);

    // Perform hblank processing for physical line 0.
    doHBlank(0);

    // If graphics are disabled, return.
    if (gbGraphicsDisabled)
        return;

    // Draw the first Game Boy line early (physical line 24) if it has been modified.
    if (drawingState[0].modified)
        drawLine(0);

    // Mark the first Game Boy line as completed.
    lineCompleted[0] = true;
}

std::vector<void (*)()> vblankTasks;

/**
 * @brief Add a function to be called at VBlank time.
 *
 * This function adds a function pointer to the list of tasks to be executed during VBlank.
 * The function pointer is added to the end of the vblankTasks vector.
 *
 * @param func A pointer to the function to be executed at VBlank time.
 */
void doAtVBlank(void (*func)(void))
{
    vblankTasks.push_back(func);
}

bool filterFlip = false;
/**
 * @brief Handle vblank interrupt.
 * 
 * This function is called during the vblank interrupt and performs various tasks such as setting the appropriate
 * vram banks for scaling, capturing the main display into vram bank D, resetting the line completion status, and
 * executing any vblank tasks that were added to the vblankTasks vector using doAtVBlank. 
 * If scaling is enabled, DMA copying is left for arm7 to handle and the scaleTransferReady flag is set to true. 
 * The function also updates the frame counter and the filter position if a filter is being applied.
 */
void vblankHandler()
{
    if (sharedData->scalingOn)
    {
        // Set vram banks for scaling.
        vramSetBankD(VRAM_D_ARM7_0x06000000);
        vramSetBankC(VRAM_C_ARM7_0x06020000);
        sharedData->scaleTransferReady = true;

        // Capture the main display into vram bank D.
        REG_DISPCAPCNT = 15 | 3 << 16 | 0 << 18 | 3 << 20 | 0 << 29 | 1 << 31;
    }

    // Update vblank status and frame counter.
    didVblank = true;
    dsFrameCounter++;

    // Reset line completion status.
    memset(lineCompleted, 0, sizeof(lineCompleted));

    // Update filter position if a filter is being applied.
    if (scaleFilter == 1)
    {
        if (filterFlip)
        {
            REG_BG2X_SUB = SCALE_BGX - (1 << 5);
            REG_BG2Y_SUB = SCALE_BGY - (1 << 5);
            REG_BG3X_SUB = SCALE_BGX + (1 << 5);
            REG_BG3Y_SUB = SCALE_BGY + (1 << 5);
        }
        else
        {
            REG_BG2X_SUB = SCALE_BGX - (1 << 5);
            REG_BG2Y_SUB = SCALE_BGY + (1 << 5);
            REG_BG3X_SUB = SCALE_BGX + (1 << 5);
            REG_BG3Y_SUB = SCALE_BGY - (1 << 5);
        }
        filterFlip = !filterFlip;
    }

    // Copy the list of vblank tasks and execute them.
    std::vector<void (*)()> tasks = vblankTasks;
    vblankTasks.clear();
    for (uint i = 0; i < tasks.size(); i++)
    {
        tasks[i]();
    }
}

/**
 * @brief Load SGB border.
 *
 * This function sets up the background and loads the SGB border. It disables video background 3, sets the window out to video
 * background 3, and sets the display control to mode 0. It sets the background 3 control register to the border map base and
 * tile base 12, and sets the horizontal and vertical offsets to zero and -(screenOffsY - 40) respectively. It then enables
 * video background 3.
 */
void loadSGBBorder()
{
    loadedBorderType = BORDER_SGB;
    videoBgDisable(3);
    WIN_OUT = 1 << 3;
    REG_DISPCNT &= ~7; // Mode 0
    REG_BG3CNT = BG_MAP_BASE(border_map_base) | BG_TILE_BASE(12);
    REG_BG3HOFS = 0;
    REG_BG3VOFS = -(screenOffsY - 40);
    videoBgEnable(3);
}

/**

 * @brief Initialize the graphics subsystem
 * 
 * This function sets up the video memory banks for the backgrounds and sprites. It sets the vcount interrupt line to 235
 * and enables the vcount, hblank and vblank interrupts. It initializes the off map and the tile for "color0 maps". It also
 * initializes the window parameters and sets the video mode. Finally, it clears the scanline buffers and enables the graphics.
 * 
 */
void initGFX()
{
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_BG);
    vramSetBankE(VRAM_E_MAIN_SPRITE);

    REG_DISPSTAT &= 0xFF;
    REG_DISPSTAT |= 235 << 8; // Set line 235 for vcount

    gbGraphicsDisabled = true;

    irqEnable(IRQ_VCOUNT);
    irqEnable(IRQ_HBLANK);
    irqEnable(IRQ_VBLANK);
    irqSet(IRQ_VCOUNT, &vcountHandler);
    irqSet(IRQ_HBLANK, &hblankHandler);
    irqSet(IRQ_VBLANK, &vblankHandler);

    if (gbsMode)
    {
        gbGraphicsDisabled = true;
        return;
    }

    // Tile for "color0 maps", which contain only the gameboy's color 0.
    for (int i = 0; i < 16; i++)
    {
        BG_GFX[i] = 1 | (1 << 4) | (1 << 8) | (1 << 12);
    }

    // Initialize the "off map", for when the gameboy screen is disabled.
    // Uses the "color0 map" tile because, why not.
    for (int i = 0; i < 32 * 32; i++)
    {
        offMap[i] = 15 << 12;
    }
    // Off map palette
    BG_PALETTE[OFF_MAP_PALETTE_INDEX] = RGB15(31, 31, 31);

    for (int i = 0; i < 128; i++)
        sprites[i].attr0 = ATTR0_DISABLED;

    initGFXPalette();

    gbGraphicsDisabled = true;
    swiWaitForVBlank();

    WIN_IN = (1 << 4) | (1 << 12) | 1;
    WIN_OUT = 1 << 3;
    WIN1_X0 = screenOffsX;
    WIN1_X1 = screenOffsX + 160;
    WIN1_Y0 = screenOffsY;
    WIN1_Y1 = screenOffsY + 144;
    WIN0_X0 = screenOffsX;
    WIN0_Y0 = screenOffsY;
    WIN0_X1 = screenOffsX + 160;
    WIN0_Y1 = screenOffsY + 144;

    int mode = (loadedBorderType == BORDER_CUSTOM ? MODE_3_2D : MODE_0_2D);
    videoSetMode(mode | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE |
                 DISPLAY_WIN0_ON | DISPLAY_WIN1_ON | DISPLAY_SPR_1D);
    if (sharedData->scalingOn)
        REG_DISPCNT &= ~(3 << 16); // Main display is disabled when scaling is on

    memset(vram[0], 0, 0x2000);
    memset(vram[1], 0, 0x2000);
    for (int i = 0; i < 144; i++)
    {
        scanlineBuffers[0][i].modified = false;
        scanlineBuffers[1][i].modified = false;
    }
    gbGraphicsDisabled = false;

    gfxMask = 0;

    checkBorder();
    refreshGFX();
}

/**
 * @brief Initializes the palette data for the graphics.
 *
 * This function sets the palette data for the background and sprite palettes,
 * as well as prevents flickering when loading ROMs.
 */
void initGFXPalette()
{
    memset(bgPaletteData, 0xff, 0x40);

    if (gbMode == GB)
    {
        sprPaletteData[0] = 0xff;
        sprPaletteData[1] = 0xff;
        sprPaletteData[2] = 0x15 | ((0x15 & 7) << 5);
        sprPaletteData[3] = (0x15 >> 3) | (0x15 << 2);
        sprPaletteData[4] = 0xa | ((0xa & 7) << 5);
        sprPaletteData[5] = (0xa >> 3) | (0xa << 2);
        sprPaletteData[6] = 0;
        sprPaletteData[7] = 0;
        sprPaletteData[8] = 0xff;
        sprPaletteData[9] = 0xff;
        sprPaletteData[10] = 0x15 | ((0x15 & 7) << 5);
        sprPaletteData[11] = (0x15 >> 3) | (0x15 << 2);
        sprPaletteData[12] = 0xa | ((0xa & 7) << 5);
        sprPaletteData[13] = (0xa >> 3) | (0xa << 2);
        sprPaletteData[14] = 0;
        sprPaletteData[15] = 0;
        bgPaletteData[0] = 0xff;
        bgPaletteData[1] = 0xff;
        bgPaletteData[2] = 0x15 | ((0x15 & 7) << 5);
        bgPaletteData[3] = (0x15 >> 3) | (0x15 << 2);
        bgPaletteData[4] = 0xa | ((0xa & 7) << 5);
        bgPaletteData[5] = (0xa >> 3) | (0xa << 2);
        bgPaletteData[6] = 0;
        bgPaletteData[7] = 0;
    }

    // This prevents some flickering when loading roms
    for (int i = 0; i < 8; i++)
    {
        updateBgPalette_GBC(i, bgPaletteData + i * 8);
    }
}

/**
 * @brief Refreshes the graphics on the screen.
 *
 * This function updates all tiles and tile maps on both backgrounds,
 * as well as clears all queues of changes to tiles and tile maps.
 * It also resets various internal flags and counters used for graphics processing.
 */
void refreshGFX()
{
    for (int i = 0; i < 0x180; i++)
    {
        drawTile(i, 0);
        drawTile(i, 1);
    }
    for (int i = 0; i < 0x400; i++)
    {
        updateTileMap(0, i);
        updateTileMap(1, i);
    }
    changedMapQueueLength[0] = 0;
    changedMapQueueLength[1] = 0;
    changedTileQueueLength = 0;
    changedTileInFrameQueueLength = 0;
    memset(changedTile, 0, sizeof(changedTile));
    memset(changedTileInFrame, 0, sizeof(changedTileInFrame));

    lastScreenDisabled = !(ioRam[0x40] & 0x80);
    screenDisabled = lastScreenDisabled;
    winPosY = -1;

    usingTilePriority[0] = 0;
    usingTilePriority[1] = 0;
    for (int i = 0x1800; i < 0x1c00; i++)
    {
        if (vram[1][i] & 0x80)
            usingTilePriority[0]++;
    }
    for (int i = 0x1c00; i < 0x2000; i++)
    {
        if (vram[1][i] & 0x80)
            usingTilePriority[1]++;
    }
}

/**
 * @brief Disables graphics and blackens the screen or sets it to white if a border is loaded.
 *
 * This function disables all graphics and sets the background color to black or white depending
 * on whether a border is loaded. If a border is loaded, it sets the palette to white. Otherwise,
 * it sets the palette to black. It also disables all sprites.
 */
void clearGFX()
{
    gbGraphicsDisabled = true;
    for (int i = 0; i < (loadedBorderType ? 3 : 4); i++)
        videoBgDisable(i);
    REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE;

    // Blacken the screen.
    if (loadedBorderType)
    {
        // Make it white if there's a border loaded.
        // I could use the backdrop, but that messes with SGB borders.
        BG_PALETTE[OFF_MAP_PALETTE_INDEX] = RGB15(31, 31, 31);
    }
    else
    {
        // Otherwise make it black.
        BG_PALETTE[OFF_MAP_PALETTE_INDEX] = RGB15(0, 0, 0);
    }
    BG_CNT(0) = BG_MAP_BASE(off_map_base) | 3;
    videoBgEnable(0);
}

/**
 * @brief Refreshes the palettes for the Super Game Boy (SGB) mode.
 *
 * This function updates the palettes for the SGB mode, managing both background and window layers.
 * It takes into account the current state of the screen, scrolling, and window settings.
 * Note that SGB palettes might not be perfect due to the SGB not necessarily aligning
 * palettes with tiles, which is mostly problematic when scrolling around status bars.
 */
void refreshSgbPalette()
{
    int winMap = 0, bgMap = 0;
    bool winJustDisabled = false;
    bool winOn = 0;
    int hofs = 0, vofs = 0, winX = 0, winY = 0;

    for (int y = 0; y < 18; y++)
    {
        winJustDisabled = false;
        for (int yPix = y * 8 - 7; yPix <= y * 8; yPix++)
        {
            if (yPix >= 0 && drawingState[yPix].modified && drawingState[yPix].mapsModified)
            {
                if (!winJustDisabled)
                    winJustDisabled = winOn && (!drawingState[yPix].winOn);
                winOn = drawingState[yPix].winOn;
                winX = drawingState[yPix].winX;
                winY = drawingState[yPix].winY;
                hofs = drawingState[yPix].hofs;
                vofs = drawingState[yPix].vofs;
                winMap = drawingState[yPix].winMap;
                bgMap = drawingState[yPix].bgMap;
            }
        }
        for (int x = 0; x < 20; x++)
        {
            int palette = sgbMap[y * 20 + x] & 3;

            // BACKGROUND
            int yLoop = (y == 0 || winJustDisabled ? 2 : 1); // Give vertical tile -1 tile 0's palette as it's scrolling in
            int xLoop = (x == 19 ? 2 : 1);                   // Give horizontal tile 20 tile 19's palette, as it's scrolling in
            while (yLoop-- > 0)
            {
                for (int j = 0; j < xLoop; j++)
                {
                    int realx = (((x + j) * 8 + hofs) & 0xff) / 8;
                    int realy = (((y - yLoop) * 8 + vofs + 7) & 0xff) / 8;
                    int i = realy * 32 + realx;
                    map[bgMap][i] &= ~(7 << 12);
                    map[bgMap][i] |= (palette << 12);
                }
            }

            // WINDOW
            if (winOn)
            {
                yLoop = (y == 17 ? 2 : 1); // Give vertical tile 19 tile 18's palette as it's scrolling in
                xLoop = (x == 19 ? 2 : 1); // x is treated the same as the background

                while (yLoop-- > 0)
                {
                    for (int j = 0; j < xLoop; j++)
                    {
                        int realx = ((x + j) * 8 - (winX - 7)) / 8;
                        int realy = ((y + yLoop) * 8 - winY) / 8;

                        if (realx >= 0 && realy >= 0 && realx < 32 && realy < 32)
                        {
                            int i = realy * 32 + realx;
                            map[winMap][i] &= ~(7 << 12);
                            map[winMap][i] |= (palette << 12);
                        }
                    }
                }
            }
        }
    }
}

/**
 * @brief Displays an icon on the Game Boy screen.
 * 
 * @param iconid Identifier of the icon to be displayed.
 *        ICON_NULL for no icon, ICON_PRINTER for a printer icon.
 */
void displayIcon(int iconid)
{
    const u16 *gfx;
    const u16 *pal;
    int len = printerIconTilesLen; // Should always be the same

    switch (iconid)
    {
    case ICON_NULL:
        sprites[0].attr0 = ATTR0_DISABLED;
        return;
    case ICON_PRINTER:
        gfx = printerIconTiles;
        pal = printerIconPal;
        break;
    default:
        return;
    }

    dmaCopy(gfx, SPRITE_GFX + 0x200 * 16, len);
    dmaCopy(pal, SPRITE_PALETTE + 15 * 16, 16 * 2);

    sprites[0].attr0 = screenOffsY;
    sprites[0].attr1 = (screenOffsX + 160 - 32) | ATTR1_SIZE_32;
    sprites[0].attr2 = 0x200 | ATTR2_PALETTE(15);
}

/**
 * @brief Selects a custom border image file for the emulator display.
 * 
 * This function allows the user to select a custom border image file in BMP format
 * for the emulator display. The borderChooserState is loaded and the user is 
 * prompted to select a BMP file using the file chooser. If a file is selected, its
 * path is stored in borderPath, the BORDER_NONE is set to force a reload of the border,
 * and checkBorder is called to check if the border is valid.
 */
void selectBorder()
{
    muteSND(); // Mute the sound before opening file chooser

    // If the current directory is the root and a border is loaded, set the file chooser's
    // current directory to the directory containing the current border file.
    if (borderChooserState.directory == "/" && borderPath != NULL)
    {
        char dest[256];
        strcpy(dest, borderPath);
        setFileChooserMatchFile(strrchr(dest, '/') + 1);
        *(strrchr(dest, '/') + 1) = '\0';
        borderChooserState.directory = dest;
    }

    // Load the file chooser's state and prompt the user to select a BMP file.
    loadFileChooserState(&borderChooserState);
    const char *extensions[] = {"bmp"};
    int len = (sizeof(extensions) / sizeof(const char *));
    char *filename = startFileChooser(extensions, len, false, true);

    // If a file is selected, store its path in borderPath, free the filename buffer,
    // force a reload of the border, and check if the border is valid.
    if (filename != NULL)
    {
        char cwd[256];
        getcwd(cwd, 256);
        free(borderPath);
        borderPath = (char *)malloc(strlen(cwd) + strlen(filename) + 1);
        strcpy(borderPath, cwd);
        strcat(borderPath, filename);
        free(filename);
        loadedBorderType = BORDER_NONE; // Force reload
        checkBorder();
    }

    // Save the file chooser's state and reload the ROM file chooser state.
    saveFileChooserState(&borderChooserState);
    loadFileChooserState(&romChooserState);

    unmuteSND(); // Unmute the sound after closing the file chooser
}

/**
 * Loads a custom border image file into VRAM and displays it on the screen.
 *
 * @param filename The path and filename of the custom border image file to load.
 * @return 0 on success, 1 on failure.
 */
int loadBorder(const char *filename)
{
    // Attempt to open the file.
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
    {
        customBorderExists = false;
        disableMenuOption("Custom Border");
        printLog("Error opening border.\n");
        return 1;
    }

    // Enable the "Custom Border" menu option and set the custom border flag.
    enableMenuOption("Custom Border");
    customBorderExists = true;

    // Set the VRAM bank to use and start loading the image.
    vramSetBankD(VRAM_D_MAIN_BG_0x06040000);
    fseek(file, 0xe, SEEK_SET);
    u8 pixelStart = (u8)fgetc(file) + 0xe;
    fseek(file, pixelStart, SEEK_SET);

    // Load the image data into VRAM.
    for (int y = 191; y >= 168; y--)
    {
        u16 buffer[256];
        fread(buffer, 2, 0x100, file);
        u16 *src = buffer;

        for (int i = 0; i < 256; i++)
        {
            u16 val = *(src++);
            BG_GFX[0x20000 + y * 256 + i] = ((val >> 10) & 0x1f) | ((val) & (0x1f << 5)) | (val & 0x1f) << 10 | BIT(15);
        }
    }

    for (int y = 167; y >= 24; y--)
    {
        u16 buffer[256];
        fread(buffer, 2, 256, file);
        u16 *src = buffer;

        for (int i = 0; i < 48; i++)
        {
            u16 val = *(src++);
            BG_GFX[0x20000 + y * 256 + i] = ((val >> 10) & 0x1f) | ((val) & (0x1f << 5)) | (val & 0x1f) << 10 | BIT(15);
        }

        src += 160;

        for (int i = 208; i < 256; i++)
        {
            u16 val = *(src++);
            BG_GFX[0x20000 + y * 256 + i] = ((val >> 10) & 0x1f) | ((val) & (0x1f << 5)) | (val & 0x1f) << 10 | BIT(15);
        }
    }

    for (int y = 23; y >= 0; y--)
    {
        u16 buffer[256];
        fread(buffer, 2, 0x100, file);
        u16 *src = buffer;

        for (int i = 0; i < 256; i++)
        {
            u16 val = *(src++);
            BG_GFX[0x20000 + y * 256 + i] = ((val >> 10) & 0x1f) | ((val) & (0x1f << 5)) | (val & 0x1f) << 10 | BIT(15);
        }
    }

    fclose(file);

    return 0;
}

void checkBorder()
{
    if (gbGraphicsDisabled)
        return;

    int lastBorderType = loadedBorderType;
    int nextBorderType = BORDER_NONE;

    if (sgbBorderLoaded && sgbBordersEnabled)
    {
        nextBorderType = BORDER_SGB;
    }

    if (customBordersEnabled && scaleMode == 0 && nextBorderType == BORDER_NONE)
    {
        nextBorderType = BORDER_CUSTOM;
    }

end:
    /*
       if (loadedBorderType == nextBorderType)
       return;
       */
    loadedBorderType = nextBorderType;

    if (nextBorderType == BORDER_NONE)
    {
        WIN_OUT = 0;
        REG_DISPCNT &= ~7;               // Mode 0
        BG_PALETTE[0] = BACKDROP_COLOUR; // Reset backdrop (SGB borders use the backdrop)
    }
    else
    {
        WIN_OUT = 1 << 3;
        if (nextBorderType == BORDER_SGB)
        {
            loadSGBBorder();
        }
        else if (nextBorderType == BORDER_CUSTOM)
        {
            if (lastBorderType != BORDER_CUSTOM)
            { // Don't reload if it's already loaded
                videoBgDisable(3);
                // Set up background
                REG_DISPCNT &= ~7;
                REG_DISPCNT |= 3; // Mode 3
                REG_BG3CNT = BG_MAP_BASE(16) | BG_BMP16_256x256;
                REG_BG3X = 0;
                REG_BG3Y = 0;
                REG_BG3PA = 1 << 8;
                REG_BG3PB = 0;
                REG_BG3PC = 0;
                REG_BG3PD = 1 << 8;
                videoBgEnable(3);
                if (loadBorder(borderPath) == 1)
                {
                    nextBorderType = BORDER_NONE;
                    goto end;
                }
            }
        }
    }
}

/**
 * @brief Refreshes the scaling mode of the video.
 *
 * This function is used to refresh the scaling mode of the video depending on the current
 * scale mode and filter.
 *
 */
void refreshScaleMode()
{
    int BG2PA, BG2PB, BG2PC, BG2PD;

    checkBorder();
    if (scaleMode == 0)
    {
        return;
    }

    videoSetModeSub(MODE_5_2D);
    videoBgDisableSub(0);
    videoBgDisableSub(1);
    bgInitSub(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    switch (scaleMode)
    {
    case 1: // Aspect
    {
        const double scaleFactor = (double)191 / 144 + 0.125 / 144;
        BG2PA = (1 << 8) / scaleFactor;
        BG2PB = 0;
        BG2PC = 0;
        BG2PD = (1 << 8) / scaleFactor;
        int x = (256 - 160 * scaleFactor) / 2;
        SCALE_BGX = (1 << 8) * (screenOffsX - x / scaleFactor + 0.5);
        SCALE_BGY = 1 << 7; // 0.5
        break;
    }
    case 2: // Full
    {
        const double scaleFactorX = (double)255 / 160;
        const double scaleFactorY = (double)191 / 144 + 0.125 / 144;
        BG2PA = (1 << 8) / (scaleFactorX);
        BG2PB = 0;
        BG2PC = 0;
        BG2PD = (1 << 8) / (scaleFactorY);
        SCALE_BGX = (1 << 8) * (screenOffsX - (256 - 160 * scaleFactorX) / 2 / (scaleFactorX) + 0.5);
        SCALE_BGY = 1 << 7; // 0.5
    }
    break;
    default:
        return;
    }

    REG_BLDCNT_SUB = 1 << 2 | 1 << 11 | 1 << 6;
    REG_BLDALPHA_SUB = 8 | 8 << 8;

    REG_BG2X_SUB = SCALE_BGX;
    REG_BG2Y_SUB = SCALE_BGY;
    REG_BG2PA_SUB = BG2PA;
    REG_BG2PB_SUB = BG2PB;
    REG_BG2PC_SUB = BG2PC;
    REG_BG2PD_SUB = BG2PD;

    if (scaleFilter == 1)
    {
        REG_BG3X_SUB = SCALE_BGX;
        REG_BG3Y_SUB = SCALE_BGY;
    }
    else
    {
        REG_BG3X_SUB = SCALE_BGX;
        REG_BG3Y_SUB = SCALE_BGY;
    }
    REG_BG3PA_SUB = BG2PA;
    REG_BG3PB_SUB = BG2PB;
    REG_BG3PC_SUB = BG2PC;
    REG_BG3PD_SUB = BG2PD;
}

/**
 * Sets the graphics mask to the given value and performs necessary actions depending on the value.
 * 
 * @param mask an integer value representing the graphics mask to be set.
 */
void setGFXMask(int mask)
{
    // Set the graphics mask to the given value.
    gfxMask = mask;

    // Print a log message indicating the new mask value.
    printLog("Mask %d\n", gfxMask);

    // If the mask is 0, perform necessary actions.
    if (gfxMask == 0)
    {
        // If the loaded border type is not BORDER_NONE, enable the background of video and set the BG_PALETTE array.
        if (loadedBorderType != BORDER_NONE)
        {
            videoBgEnable(3);
            BG_PALETTE[0] = bgPaletteData[0] | bgPaletteData[1] << 8;
        }

        // Set the winPosY variable to -1.
        winPosY = -1;
    }
}

/**
 * Sets the SGB tiles using the given source and flags.
 *
 * @param src an unsigned 8-bit integer pointer to the source.
 * @param flags an unsigned 8-bit integer representing the flags.
 */
void setSgbTiles(u8 *src, u8 flags)
{
    // If the graphics mask is not 0 and the loaded border type is not BORDER_NONE, disable the video background and set the background palette to the backdrop color.
    if (gfxMask != 0 && loadedBorderType != BORDER_NONE)
    {
        videoBgDisable(3);
        BG_PALETTE[0] = BACKDROP_COLOUR;
    }

    // Initialize index and srcIndex to 0.
    int index = 0, srcIndex = 0;

    // If the first flag bit is set, increment index by 0x80 * 16.
    if (flags & 1)
        index += 0x80 * 16;

    // Iterate over 128 tiles.
    for (int i = 0; i < 128; i++)
    {
        // Iterate over 8 rows of pixels.
        for (int y = 0; y < 8; y++)
        {
            // Extract the bit values for each pixel in the row.
            int b1 = src[srcIndex];
            int b2 = src[srcIndex + 1] << 1;
            int b3 = src[srcIndex + 16] << 2;
            int b4 = src[srcIndex + 17] << 3;
            srcIndex += 2;

            // Initialize bb0 and bb1 to 0 and shift to 12.
            int bb0 = 0, bb1 = 0;
            int shift = 12;

            // Iterate over 4 pixels in the row to generate the data for bb1.
            for (int x = 0; x < 4; x++)
            {
                int colorid = b1 & 1;
                b1 >>= 1;
                colorid |= b2 & 2;
                b2 >>= 1;
                colorid |= b3 & 4;
                b3 >>= 1;
                colorid |= b4 & 8;
                b4 >>= 1;

                bb1 |= (colorid << shift);
                shift -= 4;
            }

            // Reset shift to 12.
            shift = 12;

            // Iterate over 4 pixels in the row to generate the data for bb0.
            for (int x = 0; x < 4; x++)
            {
                int colorid = b1 & 1;
                b1 >>= 1;
                colorid |= b2 & 2;
                b2 >>= 1;
                colorid |= b3 & 4;
                b3 >>= 1;
                colorid |= b4 & 8;
                b4 >>= 1;

                bb0 |= (colorid << shift);
                shift -= 4;
            }

            // Set the data for bb0 and bb1 in the BG_GFX array.
            BG_GFX[0x18000 + index++] = bb0;
            BG_GFX[0x18000 + index++] = bb1;
        }

        // Increment the source index by 16.
        srcIndex += 16;
    }
}

/**
 * Sets the SGB map using the given source.
 *
 * @param src an unsigned 8-bit integer pointer to the source.
 */
void setSgbMap(u8 *src)
{
    // Iterate over each tile in the 32x32 map.
    for (int i = 0; i < 32 * 32; i++)
    {
        // Extract the values for the tile ID, palette ID, flipX, and flipY.
        u16 val = ((u16 *)src)[i];
        int tile = val & 0xff;
        int paletteid = ((val >> 10) & 3) + 8;
        int flipX = (val >> 14) & 1;
        int flipY = (val >> 15) & 1;

        // Set the border map entry with the tile ID, palette ID, flipX, and flipY values.
        borderMap[i] = tile | (paletteid << 12) | (flipX << 10) | (flipY << 11);
    }

    // Flush the data cache for the source and copy the data to the background palette.
    DC_FlushRange(src + 0x800, 0x80);
    dmaCopy(src + 0x800, BG_PALETTE + 8 * 16, 0x80);

    // Set the SGB border loaded flag to true.
    sgbBorderLoaded = true;

    // If SGB borders are enabled, load the SGB border and set the background palette.
    if (sgbBordersEnabled)
    {
        loadSGBBorder();
        BG_PALETTE[0] = bgPaletteData[0] | bgPaletteData[1] << 8;

        // If probing for the border, set the probingForBorder flag to false and reset the Game Boy.
        if (probingForBorder)
        {
            probingForBorder = false;
            resetGameboy();
        }
    }
}

/**
 * Updates the tile maps.
 */
void updateTileMaps()
{
    // Iterate over the two tile maps.
    for (int m = 0; m < 2; m++)
    {
        // While there are changes in the map queue, update the tile map for the changed tile.
        while (changedMapQueueLength[m] != 0)
        {
            int tile = changedMapQueue[m][--changedMapQueueLength[m]];
            updateTileMap(m, tile);
        }
    }
}

/**
 * @brief Updates the tile map for the given map index and tile number.
 * 
 * The updateTileMap function updates the tile map for the given map index m and tile number i. It first sets the changed map flag for the tile to false. It then gets the map address and tile number for the tile.
 * If the Game Boy mode is CGB, it extracts the flipX, flipY, bank, paletteid, and priority values from the VRAM. If the priority value is set, it sets the overlay map entry with the tile number, bank, palette ID, flipX, and flipY values. Otherwise, it sets the overlay map entry to 0x300.
 * The function sets the map entry with the tile number, bank, palette ID, flipX, and flipY values. It also sets the color 0 map entry with the palette ID.
 *
 * @param m an integer value representing the map index.
 * @param i an integer value representing the tile number.
 */
void updateTileMap(int m, int i)
{
    // Set the changed map flag for the tile to false.
    changedMap[m][i] = false;

    // Get the map address and tile number for the tile.
    int mapAddr = (m ? 0x1c00 + i : 0x1800 + i);
    int tileNum = vram[0][mapAddr];

    // Initialize bank, flipX, flipY, paletteid, and priority to 0.
    int bank = 0;
    int flipX = 0, flipY = 0;
    int paletteid = 0;
    int priority = 0;

    // If the Game Boy mode is CGB, extract the flipX, flipY, bank, paletteid, and priority values from the VRAM.
    if (gbMode == CGB)
    {
        flipX = !!(vram[1][mapAddr] & 0x20);
        flipY = !!(vram[1][mapAddr] & 0x40);
        bank = !!(vram[1][mapAddr] & 0x8);
        paletteid = vram[1][mapAddr] & 0x7;
        priority = !!(vram[1][mapAddr] & 0x80);
    }

    // If the priority value is set, set the overlay map entry with the tile number, bank, palette ID, flipX, and flipY values.
    if (priority)
        overlayMap[m][i] = (tileNum + (bank * 0x100)) | (paletteid << 12) | (flipX << 10) | (flipY << 11);
    else
    {
        // Otherwise, set the overlay map entry to 0x300.
        overlayMap[m][i] = 0x300;
    }

    // Set the map entry with the tile number, bank, palette ID, flipX, and flipY values.
    map[m][i] = (tileNum + (bank * 0x100)) | (paletteid << 12) | (flipX << 10) | (flipY << 11);

    // Set the color 0 map entry with the palette ID.
    color0Map[m][i] = paletteid << 12;
}

/**
 * @brief Draws a tile with the given tile number and bank.
 * The drawTile function draws tiles on the Game Boy's background and sprite graphics planes based on the given tile number and bank.
 * The function takes in two integer arguments: tileNum, which represents the tile number, and bank, which represents the bank number. The tile number is used to calculate the index of the tile data and to determine whether the tile number is unsigned or signed. The bank number is used to determine which VRAM bank the tile data is stored in.
 * The function iterates over the 8 rows in the tile and over the 4 pixels in each row. It sets the foreground and background color values for each pixel based on the color ID values in the tile data. It then sets the graphics data for the tile in the appropriate location in the background or sprite graphics planes based on whether the tile number is unsigned or signed.
 *
 * @param tileNum an integer value representing the tile number.
 * @param bank an integer value representing the bank number.
 */
void drawTile(int tileNum, int bank)
{
    // Calculate the index for the tile and the signed index if the tile number is greater than or equal to 0x100.
    int index = (tileNum << 4) + (bank * 0x100 * 16);
    int signedIndex = index;
    if (tileNum >= 0x100)
        signedIndex -= (0x100 << 4);

    // Determine whether the tile number is unsigned or signed.
    bool unsign = tileNum < 0x100;
    bool sign = tileNum >= 0x80;

    // Get the source data for the tile.
    u8 *src = &vram[bank][tileNum << 4];

    // Iterate over the 8 rows in the tile.
    for (int y = 0; y < 8; y++)
    {
        int b1 = *(src++);
        int b2 = *(src++) << 1;
        int bb0 = 0, bb1 = 0;
        int fb0 = 0, fb1 = 0;
        int sb0 = 0, sb1 = 0;
        int shift = 12;

        // Iterate over the 4 pixels in each row of the tile.
        for (int x = 0; x < 4; x++)
        {
            int colorid = b1 & 1;
            b1 >>= 1;
            colorid |= b2 & 2;
            b2 >>= 1;

            // Set the foreground and background color values.
            fb1 |= (colorid + 1) << shift;
            if (colorid != 0)
                bb1 |= ((colorid + 1) << shift);
            if (unsign)
                sb1 |= (colorid << shift);
            shift -= 4;
        }
        shift = 12;
        for (int x = 0; x < 4; x++)
        {
            int colorid = b1 & 1;
            b1 >>= 1;
            colorid |= b2 & 2;
            b2 >>= 1;

            fb0 |= (colorid + 1) << shift;
            if (colorid != 0)
                bb0 |= ((colorid + 1) << shift);
            if (unsign)
                sb0 |= (colorid << shift);
            shift -= 4;
        }

        // Set the graphics data for the tile based on whether it is unsigned or signed.
        if (unsign)
        {
            BG_GFX[0x8000 + index] = bb0;
            BG_GFX[0x8000 + index + 1] = bb1;
            BG_GFX[0x10000 + index] = fb0;
            BG_GFX[0x10000 + index + 1] = fb1;
            SPRITE_GFX[index++] = sb0;
            SPRITE_GFX[index++] = sb1;
        }
        if (sign)
        {
            BG_GFX[0xc000 + signedIndex] = bb0;
            BG_GFX[0xc000 + signedIndex + 1] = bb1;
            BG_GFX[0x14000 + signedIndex++] = fb0;
            BG_GFX[0x14000 + signedIndex++] = fb1;
        }
    }
}

/**
 * @brief Copies a tile from source data to destination data.
 * The copyTile function copies a tile from the source data to the destination data.
 * The function takes in two arguments: src, a pointer to the source data for the tile, and dest, a pointer to the destination data for the tile.
 * The function iterates over the 8 rows in the tile and over the 8 pixels in each row. It gets the color ID for each pixel in the tile data and sets the appropriate color value in the destination data.
 *
 * @param src a pointer to the source data for the tile.
 * @param dest a pointer to the destination data for the tile.
 */
void copyTile(u8 *src, u16 *dest)
{
    // Iterate over the 8 rows in the tile.
    for (int y = 0; y < 8; y++)
    {
        dest[y * 2] = 0;
        dest[y * 2 + 1] = 0;
        int index = y * 2;

        // Iterate over the 8 pixels in each row of the tile.
        for (int x = 0; x < 8; x++)
        {
            // Get the color ID for the pixel.
            int colorid;
            colorid = !!(src[index] & (0x80 >> x));
            colorid |= !!(src[index + 1] & (0x80 >> x)) << 1;

            // Set the color value in the destination data.
            int orValue = (colorid << ((x % 4) * 4));
            dest[y * 2 + x / 4] |= orValue;
        }
    }
}

/**
 * Draws the screen using the current rendering state.
 *
 * If `probingForBorder` is true, the function immediately returns without performing any actions.
 * Otherwise, if fast forward mode is not enabled, the function waits for vblank and sets the
 * `didVblank` flag to false. If `interruptWaitMode` is 1, the function always waits for vblank,
 * otherwise it waits only if vblank has not yet occurred. The `frameFlip_Gameboy` and
 * `frameFlip_DS` fields of the `sharedData` struct are updated based on the current value of
 * `REG_VCOUNT`. If `gfxMask` is nonzero, the function immediately returns without performing any
 * further actions. The `renderingState` and `drawingState` pointers are swapped, and the
 * `screenDisabled` variable is updated based on the value of the LCD control register. The tile
 * data and tile maps are updated, and if SGB mode is active, the SGB palette is refreshed.
 */
void drawScreen()
{
    // If we are probing for a border, return early without performing any actions.
    if (probingForBorder)
        return;

    // If we are not in fast forward mode, wait for vblank and reset the `didVblank` flag.
    if (!(fastForwardMode || fastForwardKey))
    {
        if (interruptWaitMode == 1) // Always wait for Vblank.
            swiWaitForVBlank();
        else
        { // Continue if we've passed vblank.
            if (!didVblank)
                swiWaitForVBlank();
        }
    }
    didVblank = false;

    // Update the `frameFlip_Gameboy` and `frameFlip_DS` fields of the `sharedData` struct
    // based on the current value of `REG_VCOUNT`.
    sharedData->frameFlip_Gameboy = !sharedData->frameFlip_Gameboy;
    if (REG_VCOUNT == 192)
        sharedData->frameFlip_DS = sharedData->frameFlip_Gameboy;

    // If `gfxMask` is nonzero, return early without performing any further actions.
    if (gfxMask)
        return;

    // Swap the `renderingState` and `drawingState` pointers.
    ScanlineStruct *tmp = renderingState;
    renderingState = drawingState;
    drawingState = tmp;

    // Update the `screenDisabled` variable based on the value of the LCD control register.
    screenDisabled = lastScreenDisabled;
    if (!(ioRam[0x40] & 0x80))
        screenDisabled = true;
    lastScreenDisabled = !(ioRam[0x40] & 0x80);

    // Reset the window Y position.
    winPosY = -1;

    // Update the tile data and tile maps.
    updateTiles();
    updateTileMaps();

    // If SGB mode is active, refresh the SGB palette.
    if (sgbMode)
        refreshSgbPalette();
}

/**
 * @brief Draw sprites on the screen.
 * This function draws the sprites onto the screen using the sprite data provided.
 * It uses the sprite data to determine the position, size, tile number, bank,
 * and other attributes of each sprite.
 * @param data Pointer to an array containing the sprite data.
 * @param tall Whether the sprites are tall (2 tiles high) or not.
*/
void drawSprites(u8 *data, int tall)
{
    for (int i = 0; i < 40; i++)
    {
        int spriteIndex = i + firstGbSprite;
        int spriteNum = i * 4;
        if (data[spriteNum] == 0)
            sprites[spriteIndex].attr0 = ATTR0_DISABLED;
        else
        {
            int y = data[spriteNum] - 16;
            if (y == -16)
            {
                sprites[spriteIndex].attr0 = 0;
            }
            else
            {
                int tileNum = data[spriteNum + 2];
                if (tall)
                    tileNum &= ~1;
                int x = data[spriteNum + 1] - 8;
                int bank = 0;
                int flipX = !!(data[spriteNum + 3] & 0x20);
                int flipY = !!(data[spriteNum + 3] & 0x40);
                int priority = !!(data[spriteNum + 3] & 0x80);
                int paletteid;

                if (gbMode == CGB)
                {
                    bank = !!(data[spriteNum + 3] & 0x8);
                    paletteid = data[spriteNum + 3] & 0x7;
                }
                else
                {
                    if (sgbMode)
                    {
                        // Sprites are colored the same as the background. A bit
                        // of compromise is needed, since they don't line up perfectly.
                        int yPos, xPos;
                        xPos = (x < 0 ? 0 : x / 8);
                        if (tall)
                        {
                            if ((y >= 144 - 15 && !drawingState[143].spritesOn) || (y < 144 - 15 && !drawingState[y + 15].spritesOn))
                                yPos = y / 8; // Take the color at the top
                            else if ((y < 0 && !drawingState[0].spritesOn) || !drawingState[y].spritesOn)
                                yPos = (y + 15) / 8; // Take the color at the bottom
                            else
                                yPos = (y < -7 ? 0 : (y + 7) / 8); // Take the color around the middle
                        }
                        else
                        {
                            if (y > -8 && ((y >= 144 - 8 && !drawingState[143].spritesOn) || (y < 144 - 8 && !drawingState[y + 8].spritesOn)))
                                yPos = y / 8; // Take the color at the top
                            else if ((y < 0 && !drawingState[0].spritesOn) || !drawingState[y].spritesOn)
                                yPos = (y + 7) / 8; // Take the color at the bottom
                            else
                                yPos = (y < -3 ? 0 : (y + 3) / 8); // Take the color around the middle
                        }
                        if (yPos < 0)
                            yPos = 0;
                        else if (yPos >= 18)
                            yPos = 17;
                        int sgbPalette = sgbMap[yPos * 20 + xPos] & 3;
                        paletteid = sgbPalette + (!!(data[spriteNum + 3] & 0x10)) * 4;
                    }
                    else
                        paletteid = ((data[spriteNum + 3] & 0x10) ? 5 : 0);
                }

                int priorityVal = (priority ? spr_priority_low : spr_priority);
                sprites[spriteIndex].attr0 = (y + screenOffsY) | (tall << 15);
                sprites[spriteIndex].attr1 = ((x & 0x1ff) + screenOffsX) | (flipX << 12) | (flipY << 13);
                sprites[spriteIndex].attr2 = (tileNum + (bank * 0x100)) | (priorityVal << 10) | (paletteid << 12);
            }
        }
    }
}

void drawScanline(int scanline) ITCM_CODE;

int winX;
// Called after mode 2
/**
 * @brief Updates the window X position if it has changed and sets the lineModified
 * and mapsModified flags accordingly.
 * 
 * @param scanline Current scanline.
 */
void drawScanline(int scanline)
{
    // This japanese game, "Fushigi no Dungeon - Fuurai no Shiren..." expects
    // writes to winX during mode 3 to take effect next scanline - at least,
    // when winX is changed from >=167 to <167 or vice versa. Maybe this should
    // be investigated further.
    if (winX != ioRam[0x4b])
    {
        winX = ioRam[0x4b];
        lineModified = true;
        mapsModified = true;
    }
}

void drawScanline_P2(int scanline) ITCM_CODE;

/**
 * @brief Draw a scanline on the screen using palette data and background maps.
 * @param scanline The current scanline being drawn.
 * This function is called after video mode 3. It draws a scanline on the screen using the
 * palette data and background maps. If HBlank is disabled, the function returns immediately.
 * The function checks if the window position needs to be adjusted, updates the rendering state
 * for the scanline, and sets the modified flag if necessary. It then updates the sprite and
 *  background palettes, as well as the maps and tiles used in the rendering. If the scanline
 *  is 0, the palettes and maps are marked as modified to ensure they are drawn correctly.
 *  @return void
  */
void drawScanline_P2(int scanline)
{
    if (hblankDisabled)
        return;
    if (winPosY == -2)
        winPosY = ioRam[0x44] - ioRam[0x4a];
    else if (ioRam[0x40] & 0x20 && winX < 167 && ioRam[0x4a] <= scanline)
        winPosY++;

    // Always set this, since it's checked in the drawSprites function
    renderingState[scanline].spritesOn = ioRam[0x40] & 0x2;

    if (scanline == 0)
    {
        lineModified = true;
        bgPalettesModified = true;
        sprPalettesModified = true;
        spritesModified = true;
        mapsModified = true;
    }
    else if (scanline == ioRam[0x4a])
    { // First line of the window
        lineModified = true;
        mapsModified = true;
    }
    else if (!lineModified)
    {
        renderingState[scanline].modified = false;
        return;
    }

    renderingState[scanline].modified = true;

    renderingState[scanline].spritesModified = spritesModified;
    if (spritesModified)
    {
        for (int i = 0; i < 0xa0; i++)
            renderingState[scanline].spriteData[i] = hram[i];
        renderingState[scanline].tallSprites = !!(ioRam[0x40] & 4);
        spritesModified = false;
    }

    renderingState[scanline].mapsModified = mapsModified;
    if (mapsModified)
    {
        mapsModified = false;

        int tileSigned, winMap, bgMap;
        if (ioRam[0x40] & 0x10)
            tileSigned = 0;
        else
            tileSigned = 1;
        if (ioRam[0x40] & 0x40)
            winMap = 1;
        else
            winMap = 0;
        if (ioRam[0x40] & 0x8)
            bgMap = 1;
        else
            bgMap = 0;

        renderingState[scanline].bgMap = bgMap;
        renderingState[scanline].winMap = winMap;

        if (gbMode != CGB && !(ioRam[0x40] & 1))
        {
            renderingState[scanline].winColor0Cnt = BG_MAP_BASE(off_map_base) | 3;
            renderingState[scanline].winCnt = BG_MAP_BASE(off_map_base) | 3;
            renderingState[scanline].winOverlayCnt = BG_MAP_BASE(off_map_base) | 3;
            renderingState[scanline].bgColor0Cnt = BG_MAP_BASE(off_map_base) | 3;
            renderingState[scanline].bgCnt = BG_MAP_BASE(off_map_base) | 3;
            renderingState[scanline].bgOverlayCnt = BG_MAP_BASE(off_map_base) | 3;
        }
        else
        {
            bool priorityOn = gbMode == CGB && ioRam[0x40] & 1;

            int winMapBase = map_base[winMap];
            int bgMapBase = map_base[bgMap];

            renderingState[scanline].tileSigned = tileSigned;

            int tileBase = (tileSigned ? 6 : 4);

            renderingState[scanline].winColor0Cnt = (BG_MAP_BASE(color0_map_base[winMap]) | BG_TILE_BASE(0) | win_color0_priority);
            renderingState[scanline].winCnt = (BG_MAP_BASE(winMapBase) | BG_TILE_BASE(tileBase) | win_priority);
            renderingState[scanline].bgColor0Cnt = (BG_MAP_BASE(color0_map_base[bgMap]) | BG_TILE_BASE(0) | bg_color0_priority);
            renderingState[scanline].bgCnt = (BG_MAP_BASE(bgMapBase) | BG_TILE_BASE(tileBase) | bg_priority);

            renderingState[scanline].winAllCnt = (BG_MAP_BASE(winMapBase) | BG_TILE_BASE(tileBase + 4) | win_all_priority);
            renderingState[scanline].bgAllCnt = (BG_MAP_BASE(bgMapBase) | BG_TILE_BASE(tileBase + 4) | bg_all_priority);

            if (priorityOn)
            {
                renderingState[scanline].winOverlayCnt = (BG_MAP_BASE(overlay_map_base[winMap]) | BG_TILE_BASE(tileBase) | win_overlay_priority);
                renderingState[scanline].bgOverlayCnt = (BG_MAP_BASE(overlay_map_base[bgMap]) | BG_TILE_BASE(tileBase) | bg_overlay_priority);
            }
            else
            {
                // Give these layers the same priority as the regular layers.
                renderingState[scanline].winOverlayCnt = (BG_MAP_BASE(overlay_map_base[winMap]) | BG_TILE_BASE(tileBase) | win_priority);
                renderingState[scanline].bgOverlayCnt = (BG_MAP_BASE(overlay_map_base[bgMap]) | BG_TILE_BASE(tileBase) | bg_priority);
            }
        }

        bool winOn = (ioRam[0x40] & 0x20) && winX < 167 && ioRam[0x4a] < 144 && ioRam[0x4a] <= scanline;
        renderingState[scanline].winOn = winOn;
        renderingState[scanline].hofs = ioRam[0x43];
        renderingState[scanline].vofs = ioRam[0x42];
        renderingState[scanline].winX = winX;
        renderingState[scanline].winPosY = winPosY;
        renderingState[scanline].winY = ioRam[0x4a];
    }

    renderingState[scanline].bgPalettesModified = bgPalettesModified;
    if (bgPalettesModified)
    {
        bgPalettesModified = false;
        renderingState[scanline].bgPal = ioRam[0x47];

        // Hash is helpful for more-or-less static screens using tons of colours.
        int hash = 0;
        for (int i = 0; i < 0x10; i++)
        {
            hash = ((hash << 5) + hash) + ((int *)bgPaletteData)[i];
        }

        if (renderingState[scanline].bgHash != hash ||
            scanline == 0)
        { // Just to be safe in case of hash collision

            renderingState[scanline].bgHash = hash;
            for (int i = 0; i < 0x40; i++)
            {
                renderingState[scanline].bgPaletteData[i] = bgPaletteData[i];
            }
        }
    }

    renderingState[scanline].sprPalettesModified = sprPalettesModified;
    if (sprPalettesModified)
    {
        sprPalettesModified = false;
        renderingState[scanline].sprPal[0] = ioRam[0x48];
        renderingState[scanline].sprPal[1] = ioRam[0x49];
        for (int i = 0; i < 0x40; i++)
        {
            renderingState[scanline].sprPaletteData[i] = sprPaletteData[i];
        }
    }

    lineModified = false;
}

/**
 * @brief Writes a value to VRAM at the specified address.
 * This function writes a byte to VRAM at a specified address. It checks if the old value is the same as the new value, and if it is, the function returns without making any changes. If the value has changed, the new value is written to the specified address in VRAM.
 * If the address is less than 0x1800, the function determines which tile the address belongs to and checks the current scanline. If the current scanline is between 128 and 143, it marks the tile as changed in the current frame. Otherwise, it marks the tile as changed outside of the current frame.
 * If the address is greater than or equal to 0x1800, the function determines which map the address belongs to and which tile it represents. If the VRAM bank is 1 and the new value has bit 7 set but the old value didn't, it increments the number of tiles that use tile priority for that map. If the new value doesn't have bit 7 set but the old value did, it decrements the number of tiles that use tile priority for that map. The function then marks the map and tile as changed.
 * @param addr The address to write to.
 * @param val The value to write.
 */
void writeVram(u16 addr, u8 val)
{
    u8 old = vram[vramBank][addr];
    if (old == val)
        return;
    vram[vramBank][addr] = val;

    if (addr < 0x1800)
    {
        int tileNum = addr / 16;
        int scanline = ioRam[0x44];
        if (scanline >= 128 && scanline < 144)
        {
            if (!changedTileInFrame[vramBank][tileNum])
            {
                changedTileInFrame[vramBank][tileNum] = true;
                changedTileInFrameQueue[changedTileInFrameQueueLength++] = tileNum | (vramBank << 9);
            }
        }
        else
        {
            if (!changedTile[vramBank][tileNum])
            {
                changedTile[vramBank][tileNum] = true;
                changedTileQueue[changedTileQueueLength++] = tileNum | (vramBank << 9);
            }
        }
    }
    else
    {
        int map = (addr - 0x1800) / 0x400;
        int tile = addr & 0x3ff;
        if (vramBank == 1)
        {
            if ((val & 0x80) && !(old & 0x80))
                usingTilePriority[map]++;
            else if (!(val & 0x80) && (old & 0x80))
                usingTilePriority[map]--;
        }
        if (!changedMap[map][tile])
        {
            changedMap[map][tile] = true;
            changedMapQueue[map][changedMapQueueLength[map]++] = tile;
        }
    }
}

/**
 * @brief Writes a 16-bit value to VRAM.
 * The writeVram16 function is responsible for writing 16 bytes of data from the source address to the destination address in video RAM (VRAM). The function checks if the write operation is performed on map flags (in VRAM bank 1), and if so, it adjusts the tile priority accordingly.
 * After the write operation is performed, the function checks if the affected tiles or maps need to be redrawn, based on whether the operation occurs during the visible screen area or not. If necessary, the function adds the affected tiles or maps to a queue for later redrawing.
 * @param dest The destination address in VRAM.
 * @param src The source address in ROM or RAM.
 */
void writeVram16(u16 dest, u16 src)
{
    bool writingToMapFlags = (vramBank == 1 && dest >= 0x1800);
    bool changed = false;
    u8 *page = memory[src >> 12];
    int offset = src & 0xfff;

    for (int i = 0; i < 16; i++)
    {
        u8 val = page[offset++];
        if (vram[vramBank][dest] != val)
        {
            changed = true;
            if (writingToMapFlags)
            {
                u8 old = vram[vramBank][dest];
                int map = (dest - 0x1800) / 0x400;
                if ((val & 0x80) && !(old & 0x80))
                    usingTilePriority[map]++;
                else if (!(val & 0x80) && (old & 0x80))
                    usingTilePriority[map]--;
            }
            vram[vramBank][dest] = val;
        }
        dest++;
    }
    if (!changed)
        return;
    dest -= 16;

    if (dest < 0x1800)
    {
        int tileNum = dest / 16;
        if (ioRam[0x44] < 144)
        {
            if (!changedTileInFrame[vramBank][tileNum])
            {
                changedTileInFrame[vramBank][tileNum] = true;
                changedTileInFrameQueue[changedTileInFrameQueueLength++] = tileNum | (vramBank << 9);
            }
        }
        else
        {
            if (!changedTile[vramBank][tileNum])
            {
                changedTile[vramBank][tileNum] = true;
                changedTileQueue[changedTileQueueLength++] = tileNum | (vramBank << 9);
            }
        }
    }
    else
    {
        int map = (dest - 0x1800) / 0x400;
        for (int i = 0; i < 16; i++)
        {
            int tile = (dest + i) & 0x3ff;
            if (!changedMap[map][tile])
            {
                changedMap[map][tile] = true;
                changedMapQueue[map][changedMapQueueLength[map]++] = tile;
            }
        }
    }
}

/**
 * @brief Updates the tile data and queues the changed tiles to be redrawn.
 *
 * The function updates the tile data and queues the changed tiles to be redrawn.
 * It first processes all the tiles in the @p changedTileQueue, which holds the
 * tiles that have changed on the current frame. After redrawing these tiles,
 * it marks them as unmodified. Then, it copies the tiles in the
 * @p changedTileInFrameQueue, which holds the tiles that have changed in the
 * current frame but haven't been redrawn yet, to @p changedTileQueue so they
 * can be redrawn on the next frame. Finally, it marks the tiles in the
 * @p changedTileInFrame queue as unmodified.
 */
void updateTiles()
{
    while (changedTileQueueLength > 0)
    {
        int val = changedTileQueue[--changedTileQueueLength];
        int bank = val >> 9, tile = val & 0x1ff;
        drawTile(tile, bank);
        changedTile[bank][tile] = false;
    }
    // copy "changedTileInFrame" to "changedTile", where they'll be applied next
    // frame.
    while (changedTileInFrameQueueLength > 0)
    {
        int val = changedTileInFrameQueue[--changedTileInFrameQueueLength];
        int bank = val >> 9, tile = val & 0x1ff;
        changedTileInFrame[bank][tile] = false;
        changedTile[bank][tile] = true;
        changedTileQueue[changedTileQueueLength++] = val;
    }
}

/**
 * @brief Updates a background palette.
 * This function iterates through each color in the given data and updates the corresponding color in the background palette.
 * The @p paletteid parameter specifies which palette to update, and @p dmgPal is the palette data for DMG mode, which is used to map
 * the indices to the correct color values.
 * The background palette is stored as a linear array of 64 16-bit color values (one for each of the four colors in each of the 16 palette entries).
 * The first entry is always transparent, so it is skipped in the loop.
 * @param paletteid ID of the palette to update.
 * @param data Pointer to the data containing the new palette values.
 * @param dmgPal DMG palette value.
 */
void updateBgPalette(int paletteid, u8 *data, u8 dmgPal)
{
    for (int i = 0; i < 4; i++)
    {
        int id = (dmgPal >> (i * 2)) & 3;

        // Set the new color values for each palette index
        BG_PALETTE[((paletteid)*16) + i + 1] = data[(paletteid * 8) + (id * 2)] | data[(paletteid * 8) + (id * 2) + 1] << 8;
    }
}

/**
 * @brief Updates the Game Boy Color background palette with the specified data.
 *
 * @param paletteid The index of the palette to update.
 * @param data The data to update the palette with.
 *
 * @note This function is only used in Game Boy Color mode.
 */
void updateBgPalette_GBC(int paletteid, u8 *data)
{
    // Compute the destination and source pointers.
    u16 *dest = BG_PALETTE + paletteid * 16 + 1;
    u16 *src = ((u16 *)data) + paletteid * 4;

    // Update the palette with the specified data.
    for (int i = 0; i < 4; i++)
        *(dest++) = *(src++);
}

/**
 * @brief Updates the sprite palette in the given paletteid using the provided data and dmgPal
 * @param paletteid The palette ID to update
 * @param data The data containing the new color values
 * @param dmgPal The DMG palette
 */
void updateSprPalette(int paletteid, u8 *data, u8 dmgPal)
{
    int src = paletteid;
    if (gbMode == GB && paletteid >= 4) // Check if we're in SGB mode and adjust the source ID if necessary
        src -= 4;
    for (int i = 0; i < 4; i++)
    {
        int id;
        if (gbMode == GB) // Check if we're in DMG or CGB mode
            id = (dmgPal >> (i * 2)) & 3; // Get the DMG color ID
        else
            id = i; // Use the CGB color ID

        // Update the sprite palette with the new color values
        SPRITE_PALETTE[((paletteid)*16) + i] = data[(src * 8) + (id * 2)] | data[(src * 8) + (id * 2) + 1] << 8;
    }
}


/**
 * @brief Handles writes to video-related I/O registers.
 * This function is called whenever a write is made to one of the video-related I/O registers.
 * It handles the register writes by updating any state that is affected by the write.
 * For example, changes to the LCDC register can affect whether the background and sprites are
 * displayed, and changes to the DMA register initiate a DMA transfer of sprite data.
 * The handleVideoRegister function handles write accesses to various video-related IO registers. It takes two parameters, ioReg and val, which correspond to the register being written to and the value being written, respectively. The function contains a switch statement that handles each IO register separately.
 * For register 0x40, the LCD Control register (LCDC), the function updates the IO register value and sets flags indicating that the screen needs to be redrawn if the LCD is turned on or off, and if the background or sprites should be redrawn. If the LCD is turned off, it also sets the video mode to 0.
 * For register 0x46, the DMA Transfer register, the function performs a DMA transfer from the source address specified in the IO register multiplied by 0x100 to the HRAM. It also sets flags indicating that the sprites and screen need to be redrawn.
 * For registers 0x42 and 0x43, the Scroll Y and Scroll X registers, the function updates the IO register values and sets flags indicating that the background maps need to be redrawn.
 * For register 0x4B, the Window X Position register, the function updates the IO register value.
 * For register 0x4A, the Window Y Position register, the function updates the IO register value and sets a flag indicating that the window position needs to be updated.
 * For registers 0x47, 0x48, and 0x49, the Background Palette register, Sprite Palette 0 register, and Sprite Palette 1 register (GB classic only), respectively, the function updates the IO register values and sets flags indicating that the background or sprite palettes need to be redrawn.
 * For registers 0x69 and 0x6B, the Background Palette Data and Sprite Palette Data registers (GBC only), respectively, the function updates the IO register values, the corresponding palette data, and sets flags indicating that the background or sprite palettes need to be redrawn. These registers also update the palette index register, which is checked in the next write to determine which palette data should be read.
 * For all other registers, the function simply updates the IO register value.
 * @param ioReg The I/O register being written to.
 * @param val The value being written to the I/O register.
*/
void handleVideoRegister(u8 ioReg, u8 val)
{
    switch (ioReg)
    {
    case 0x40: // LCDC
        if ((val & 0x7B) != (ioRam[0x40] & 0x7B))
        {
            lineModified = true;
            mapsModified = true;
        }
        if ((val & 4) != (ioRam[0x40] & 4))
        {
            lineModified = true;
            spritesModified = true;
        }
        ioRam[0x40] = val;
        if (!(val & 0x80))
        {
            ioRam[0x44] = 0;
            ioRam[0x41] &= ~3; // Set video mode 0
        }
        return;
    case 0x46: // DMA
    {
        int src = val << 8;
        u8 *mem = memory[src >> 12];
        src &= 0xfff;
        for (int i = 0; i < 0xA0; i++)
        {
            u8 val = mem[src++];
            hram[i] = val;
        }
        lineModified = true;
        spritesModified = true;

        ioRam[ioReg] = val;
        // printLog("dma write %d\n", ioRam[0x44]);
        return;
    }
    case 0x42:
    case 0x43:
        if (val != ioRam[ioReg])
        {
            ioRam[ioReg] = val;
            lineModified = true;
            mapsModified = true;
        }
        break;
    case 0x4B: // winX
        if (val != ioRam[ioReg])
        {
            ioRam[ioReg] = val;
        }
        break;
    case 0x4A: // winY
        if (ioRam[0x44] >= 144 || val > ioRam[0x44])
            winPosY = -1;
        else
        {
            // Signal that winPosY must be reset according to winY
            winPosY = -2;
        }
        lineModified = true;
        mapsModified = true;
        ioRam[ioReg] = val;
        break;
    case 0x47: // BG Palette (GB classic only)
        if (gbMode == GB && ioRam[0x47] != val)
        {
            lineModified = true;
            bgPalettesModified = true;
        }
        ioRam[0x47] = val;
        return;
    case 0x48: // Spr Palette (GB classic only)
        if (gbMode == GB && ioRam[0x48] != val)
        {
            lineModified = true;
            sprPalettesModified = true;
        }
        ioRam[0x48] = val;
        return;
    case 0x49: // Spr Palette (GB classic only)
        if (gbMode == GB && ioRam[0x49] != val)
        {
            lineModified = true;
            sprPalettesModified = true;
        }
        ioRam[0x49] = val;
        return;
    case 0x69: // BG Palette Data (GBC only)
    {
        int index = ioRam[0x68] & 0x3F;
        if (bgPaletteData[index] != val)
        {
            bgPaletteData[index] = val;
            lineModified = true;
            bgPalettesModified = true;
        }

        if (ioRam[0x68] & 0x80)
            ioRam[0x68] = 0x80 | (ioRam[0x68] + 1);
        ioRam[0x69] = bgPaletteData[ioRam[0x68] & 0x3F];
        return;
    }
    case 0x6B: // Sprite Palette Data (GBC only)
    {
        int index = ioRam[0x6A] & 0x3F;
        if (sprPaletteData[index] != val)
        {
            sprPaletteData[index] = val;
            lineModified = true;
            sprPalettesModified = true;
        }

        if (ioRam[0x6A] & 0x80)
            ioRam[0x6A] = 0x80 | (ioRam[0x6A] + 1);
        ioRam[0x6B] = sprPaletteData[ioRam[0x6A] & 0x3F];
        return;
    }
    default:
        ioRam[ioReg] = val;
    }
}

/**
 * Writes a byte to the High RAM at the specified address.
 *
 * @param addr The address to write to.
 * @param val The value to write.
 */
void writeHram(u16 addr, u8 val)
{
    // Only use the lower 9 bits of the address, as that's all that exists in HRAM.
    hram[addr & 0x1ff] = val;

    // Indicate that the current line and sprites have been modified.
    lineModified = true;
    spritesModified = true;
}