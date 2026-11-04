#include "plant_rules.h"
#include <string.h>
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
int run_tests(void)
{
    int32_t seconds = 100;
    uint32_t remainder = 0;
    plant_countdown(&seconds, &remainder, 500, 1000, 1, 100);
    CHECK(seconds == 100 && remainder == 50000);
    plant_countdown(&seconds, &remainder, 500, 1000, 1, 100);
    CHECK(seconds == 99 && remainder == 0);
    seconds = 100; remainder = 0;
    for (unsigned i = 0; i < 4; i++) plant_countdown(&seconds, &remainder, 500, 1000, 1, 150);
    CHECK(seconds == 97 && remainder == 0);
    seconds = 100; remainder = 0;
    plant_countdown(&seconds, &remainder, 100, 200, 1, 100);
    plant_countdown(&seconds, &remainder, 100, 200, 1, 100);
    CHECK(seconds == 99 && remainder == 0);
    seconds = 518400; remainder = 0;
    plant_countdown(&seconds, &remainder, 500, 1000, 3600, 200);
    CHECK(seconds == 514800);
    plant_countdown(&seconds, &remainder, 1000000, 1000, 3600, 200);
    CHECK(seconds == 0 && remainder == 0);

    CHECK(plant_full(false, 3000));
    CHECK(plant_full(true, 2850));
    CHECK(!plant_full(true, 2849));
    CHECK(!plant_full(false, 2999));
    CHECK(!plant_can_water(false, false));
    CHECK(!plant_can_water(true, true));
    CHECK(plant_can_water(true, false));
    CHECK(plant_fresh(true, 10, UINT32_MAX - 20, 40));
    CHECK(!plant_fresh(true, 120001, 0, 120000));
    CHECK(!plant_fresh(false, 0, 0, 120000));

    plant_irrigation_t s = {0};
    plant_water_times_t time = {5000, 2500, 20000, 10000};
    water_mode_t mode = WATER_AUTO;
    plant_irrigation_step(&s, &mode, true, true, false, 0, &time);
    CHECK(s.watering && s.end == 5000);
    plant_irrigation_step(&s, &mode, true, false, false, 1000, &time);
    CHECK(!s.watering && s.cooling);
    plant_irrigation_step(&s, &mode, true, true, false, 20999, &time);
    CHECK(!s.watering);
    plant_irrigation_step(&s, &mode, true, true, true, 21000, &time);
    CHECK(s.watering && s.end == 23500);
    plant_irrigation_step(&s, &mode, false, true, false, 21500, &time);
    CHECK(!s.watering); /* full tray or stale/failed ADC stops an active burst */
    mode = WATER_FORCE_ON;
    plant_irrigation_step(&s, &mode, false, true, false, 22000, &time);
    CHECK(!s.watering && mode == WATER_AUTO);

    memset(&s, 0, sizeof(s)); mode = WATER_FORCE_ON;
    plant_irrigation_step(&s, &mode, true, false, false, 0, &time);
    CHECK(s.watering && s.end == 10000);
    plant_irrigation_step(&s, &mode, true, false, false, 9000, &time);
    CHECK(s.watering && s.end == 10000); /* repeated ON does not extend the cap */
    plant_irrigation_step(&s, &mode, true, false, false, 10000, &time);
    CHECK(!s.watering && s.cooling && mode == WATER_AUTO);
    mode = WATER_FORCE_OFF;
    plant_irrigation_step(&s, &mode, true, true, false, 100000, &time);
    CHECK(!s.watering);
    memset(&s, 0, sizeof(s)); mode = WATER_AUTO;
    plant_irrigation_step(&s, &mode, true, true, false, UINT32_MAX - 1000, &time);
    CHECK(s.watering && s.end == 3999);
    plant_irrigation_step(&s, &mode, true, true, false, 3998, &time);
    CHECK(s.watering);
    plant_irrigation_step(&s, &mode, true, true, false, 3999, &time);
    CHECK(!s.watering);

    uint32_t n;
    CHECK(plant_uint("100", &n) && n == 100);
    CHECK(plant_uint("4294967295", &n) && n == UINT32_MAX);
    CHECK(!plant_uint("4294967296", &n));
    CHECK(!plant_uint("garbage", &n));
    CHECK(!plant_uint("85garbage", &n));
    CHECK(!plant_uint("-1", &n));
    CHECK(!plant_uint("", &n));
    plant_line_t line = {0};
    for (unsigned i = 0; i < PLANT_LINE_CAP + 10; i++) CHECK(!plant_line_byte(&line, 'x'));
    const char *suffix = "$CMD,1,WATER_ON\n";
    for (const char *p = suffix; *p; p++) CHECK(!plant_line_byte(&line, *p));
    for (const char *p = suffix; *p; p++) {
        bool accepted = plant_line_byte(&line, *p);
        CHECK(accepted == (*p == '\n'));
    }
    char *fields[3];
    CHECK(plant_fields(line.text, fields, 3) == 3);
    CHECK(strcmp(fields[0], "$CMD") == 0 && strcmp(fields[2], "WATER_ON") == 0);
    char malformed[] = "$CMD,,WATER_ON";
    CHECK(plant_fields(malformed, fields, 3) == 0);
    char extra[] = "$CMD,1,WATER_ON,junk";
    CHECK(plant_fields(extra, fields, 3) == 0);
    return 0;
}
