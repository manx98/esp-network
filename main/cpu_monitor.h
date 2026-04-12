#pragma once
#include <stdint.h>

/**
 * Initialize the CPU load monitor.
 * Registers a FreeRTOS idle hook and a 1-second sampling timer.
 * The first few seconds are used to calibrate the idle baseline.
 */
void cpu_monitor_init(void);

/**
 * Return the estimated CPU load as a percentage (0–100).
 * Based on idle hook counting with self-calibrating baseline.
 */
uint8_t cpu_monitor_get_load(void);
