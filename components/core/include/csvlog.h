#pragma once

/*
 * CSV lines for the SD card export: one file per local day, one line per minute.
 *
 *   time,co2_ppm,temp_c,rh_pct,pressure_hpa,...,wbgt_c,crc
 *   2026-10-17 14:05,412,23.45,45.2,1008.32,...,20.1,1a2b3c4d
 *
 * temp_c / rh_pct are the primary values (SHT35, else the fallbacks). Missing values are
 * empty fields. crc is the CRC-32 of the line up to (not including) the last comma, so a line
 * cut off by a power loss is recognised and skipped. Files are only ever appended to; the
 * resume point is the last valid line of the file itself.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "history.h"

#define CSV_LINE_MAX    192

extern const char CSV_HEADER[];     /* with the trailing newline */

/* "20261017.CSV" for the local day of unix_min (tz_min: local offset, e.g. 540 for JST) */
void csvlog_day_name(char *buf, size_t n, uint32_t unix_min, int tz_min);

/* First unix minute of the local day containing unix_min */
uint32_t csvlog_day_start(uint32_t unix_min, int tz_min);

/* One line with the trailing newline; returns its length */
int csvlog_format(char *buf, size_t n, uint32_t unix_min, int tz_min, const hist_entry_t *e);

/* Check one line (without its newline) and return its unix minute */
bool csvlog_parse_line(const char *line, size_t len, int tz_min, uint32_t *unix_min);

/*
 * The unix minute of the last valid line in the tail of a file. A trailing partial line is
 * ignored. *ends_with_newline tells whether an append can start right away.
 */
bool csvlog_last_minute(const char *tail, size_t len, int tz_min, uint32_t *unix_min, bool *ends_with_newline);
