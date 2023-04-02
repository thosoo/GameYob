/**
 * @file timer.cpp
 * @brief Clock updater module for NDS (Nintendo DS) projects.
 *
 * This module provides a basic clock updater function to increase the raw time value by one.
 */

#include <nds.h>
#include <time.h>
#include "timer.h"

// Global variable to store raw time value.
extern time_t rawTime;

/**
 * @brief Updates the raw time value.
 *
 * This function is responsible for incrementing the raw time value (rawTime) by 1.
 * It is typically called by a timer interrupt or a main loop in the application.
 */
void clockUpdater()
{
    rawTime++; // Increment the raw time value by 1.
}
