#ifndef PLANT_RULES_H
#define PLANT_RULES_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Pure C helpers shared with the test harness. No peripheral or RTOS drivers. */
#define PLANT_LINE_CAP 128U
typedef struct {
    char text[PLANT_LINE_CAP];
    size_t length;
    bool discard;
} plant_line_t;

static inline bool plant_line_byte(plant_line_t *line, char c)
{
    if (c == '\n') {
        bool complete = !line->discard && line->length > 0U && line->text[0] == '$';
        line->text[line->length] = '\0';
        line->length = 0U;
        line->discard = false;
        return complete;
    }
    if (line->discard || c == '\r') return false;
    if (line->length == PLANT_LINE_CAP - 1U || c == '\0') {
        line->length = 0U;
        line->discard = true;  /* Ignore the whole remainder, including any '$'. */
        return false;
    }
    line->text[line->length++] = c;
    return false;
}

static inline bool plant_uint(const char *text, uint32_t *value)
{
    uint32_t result = 0U;
    if (text == NULL || *text == '\0') return false;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') return false;
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (result > (UINT32_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

/* Split in place; an extra/empty field invalidates the entire message. */
static inline unsigned plant_fields(char *text, char **field, unsigned capacity)
{
    unsigned count = 0U;
    if (*text == '\0') return 0U;
    field[count++] = text;
    for (; *text != '\0'; text++) {
        if (*text != ',') continue;
        *text = '\0';
        if (count == capacity || text[1] == '\0' || text[1] == ',') return 0U;
        field[count++] = text + 1;
    }
    return count;
}

/* Retain fractions instead of truncating each 500 ms sample to zero seconds.
 * Units are (ticks * speed multiplier * percentage); remainder < tick_hz*100.
 * uint64_t prevents multiplication overflow in accelerated demonstrations. */
static inline void plant_countdown(int32_t *seconds, uint32_t *remainder,
    uint32_t elapsed_ticks, uint32_t tick_hz, uint32_t scale, uint32_t rate_pct)
{
    uint64_t denominator = (uint64_t)tick_hz * 100U;
    uint64_t units = (uint64_t)elapsed_ticks * scale * rate_pct + *remainder;
    uint64_t decrement = units / denominator;
    *remainder = (uint32_t)(units % denominator);
    if (decrement >= (uint32_t)*seconds) {
        *seconds = 0;
        *remainder = 0;
    } else {
        *seconds -= (int32_t)decrement;
    }
}

static inline bool plant_fresh(bool valid, uint32_t now, uint32_t last, uint32_t ttl)
{
    return valid && (uint32_t)(now - last) <= ttl;
}

static inline bool plant_full(bool was_full, uint16_t level)
{
    return level >= 3000U || (was_full && level >= 2850U);
}

static inline bool plant_can_water(bool sensor_fresh, bool full)
{
    return sensor_fresh && !full;
}

typedef enum { WATER_AUTO, WATER_FORCE_ON, WATER_FORCE_OFF } water_mode_t;
typedef struct {
    bool watering, cooling;
    uint32_t end, cooldown_end;
    water_mode_t previous_mode;
} plant_irrigation_t;
typedef struct {
    uint32_t normal_ticks, short_ticks, cooldown_ticks, manual_ticks;
} plant_water_times_t;

static inline void plant_irrigation_step(plant_irrigation_t *s, water_mode_t *mode,
    bool allowed, bool dry, bool short_burst, uint32_t now, const plant_water_times_t *time)
{
    if (s->cooling && (int32_t)(now - s->cooldown_end) >= 0) s->cooling = false;
    bool stopped = false;
    if (!allowed || *mode == WATER_FORCE_OFF) {
        stopped = s->watering;
        s->watering = false;
        if (!allowed && *mode == WATER_FORCE_ON) *mode = WATER_AUTO;
    } else if (*mode == WATER_FORCE_ON) {
        if (s->previous_mode != WATER_FORCE_ON || !s->watering) {
            s->watering = true;
            s->end = now + time->manual_ticks;
        } else if ((int32_t)(now - s->end) >= 0) {
            s->watering = false;
            *mode = WATER_AUTO;
            stopped = true;
        }
    } else if (s->watering) {
        if (!dry || (int32_t)(now - s->end) >= 0) {
            s->watering = false;
            stopped = true;
        }
    } else if (dry && !s->cooling) {
        s->watering = true;
        s->end = now + (short_burst ? time->short_ticks : time->normal_ticks);
    }
    if (stopped) {
        s->cooling = true;
        s->cooldown_end = now + time->cooldown_ticks;
    }
    s->previous_mode = *mode;
}
#endif
