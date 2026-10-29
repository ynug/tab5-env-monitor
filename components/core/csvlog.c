#include "csvlog.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "derived.h"
#include "record.h"

const char CSV_HEADER[] =
    "time,co2_ppm,temp_c,rh_pct,pressure_hpa,temp_sht_c,temp_scd_c,temp_bmp_c,temp_scd_raw_c,"
    "rh_sht_pct,rh_scd_pct,rh_scd_raw_pct,dew_point_c,discomfort,wbgt_c,crc\n";

/* days since 1970-01-01 <-> civil date (H. Hinnant's algorithms) */
static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

uint32_t csvlog_day_start(uint32_t unix_min, int tz_min)
{
    int64_t local = (int64_t)unix_min + tz_min;
    return (uint32_t)(local - local % 1440 - tz_min);
}

void csvlog_day_name(char *buf, size_t n, uint32_t unix_min, int tz_min)
{
    int32_t y, m, d;
    civil_from_days((int32_t)(((int64_t)unix_min + tz_min) / 1440), &y, &m, &d);
    snprintf(buf, n, "%04d%02d%02d.CSV", (int)y, (int)m, (int)d);
}

static int put_val(char *buf, size_t n, const stat_t *s, const char *fmt)
{
    return s->n ? snprintf(buf, n, fmt, s->avg) : snprintf(buf, n, ",");
}

static int put_f(char *buf, size_t n, float v, const char *fmt)
{
    return isnan(v) ? snprintf(buf, n, ",") : snprintf(buf, n, fmt, v);
}

int csvlog_format(char *buf, size_t n, uint32_t unix_min, int tz_min, const hist_entry_t *e)
{
    const int64_t local = (int64_t)unix_min + tz_min;
    int32_t y, m, d;
    civil_from_days((int32_t)(local / 1440), &y, &m, &d);
    const int mod = (int)(local % 1440);
    size_t o = (size_t)snprintf(buf, n, "%04d-%02d-%02d %02d:%02d", (int)y, (int)m, (int)d, mod / 60, mod % 60);

    const float t = history_primary_temp(e), rh = history_primary_rh(e);
    const bool both = !isnan(t) && !isnan(rh);
    o += put_val(buf + o, n - o, &e->s[S_CO2], ",%.0f");
    o += put_f(buf + o, n - o, t, ",%.2f");
    o += put_f(buf + o, n - o, rh, ",%.1f");
    o += put_val(buf + o, n - o, &e->s[S_P_BMP], ",%.2f");
    o += put_val(buf + o, n - o, &e->s[S_T_SHT], ",%.2f");
    o += put_val(buf + o, n - o, &e->s[S_T_SCD], ",%.2f");
    o += put_val(buf + o, n - o, &e->s[S_T_BMP], ",%.2f");
    o += put_val(buf + o, n - o, &e->s[S_T_SCD_RAW], ",%.2f");
    o += put_val(buf + o, n - o, &e->s[S_RH_SHT], ",%.1f");
    o += put_val(buf + o, n - o, &e->s[S_RH_SCD], ",%.1f");
    o += put_val(buf + o, n - o, &e->s[S_RH_SCD_RAW], ",%.1f");
    o += put_f(buf + o, n - o, both ? derived_dew_point(t, rh) : NAN, ",%.1f");
    o += put_f(buf + o, n - o, both ? derived_discomfort(t, rh) : NAN, ",%.1f");
    o += put_f(buf + o, n - o, both ? derived_wbgt(t, rh) : NAN, ",%.1f");
    const uint32_t crc = crc32_le(0, (const uint8_t *)buf, o);
    o += (size_t)snprintf(buf + o, n - o, ",%08lx\n", (unsigned long)crc);
    return (int)o;
}

static int digits(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

bool csvlog_parse_line(const char *line, size_t len, int tz_min, uint32_t *unix_min)
{
    /* "YYYY-MM-DD HH:MM" ... ",xxxxxxxx" */
    if (len < 16 + 9) return false;
    const char *c = line + len - 9;
    if (*c != ',') return false;
    uint32_t want = 0;
    for (int i = 1; i <= 8; i++) {
        char h = c[i];
        int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
        if (v < 0) return false;
        want = want << 4 | (uint32_t)v;
    }
    if (crc32_le(0, (const uint8_t *)line, (size_t)(c - line)) != want) return false;

    if (line[4] != '-' || line[7] != '-' || line[10] != ' ' || line[13] != ':') return false;
    int y = digits(line, 4), mo = digits(line + 5, 2), d = digits(line + 8, 2);
    int h = digits(line + 11, 2), mi = digits(line + 14, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    int64_t local = (int64_t)days_from_civil(y, mo, d) * 1440 + h * 60 + mi;
    *unix_min = (uint32_t)(local - tz_min);
    return true;
}

bool csvlog_last_minute(const char *tail, size_t len, int tz_min, uint32_t *unix_min, bool *ends_with_newline)
{
    *ends_with_newline = len == 0 || tail[len - 1] == '\n';
    size_t end = len;
    /* drop a trailing partial line */
    while (end > 0 && tail[end - 1] != '\n') end--;
    while (end > 0) {
        size_t line_end = end - 1;             /* index of the '\n' */
        size_t start = line_end;
        while (start > 0 && tail[start - 1] != '\n') start--;
        size_t l = line_end - start;
        if (l > 0 && tail[line_end - 1] == '\r') l--;
        /* a line cut at the start of the tail fails the CRC like a damaged one */
        if (csvlog_parse_line(tail + start, l, tz_min, unix_min)) {
            return true;
        }
        end = start;
    }
    return false;
}
