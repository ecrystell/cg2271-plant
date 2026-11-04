/* Smart plant care: direct-register MCXC444 peripherals + FreeRTOS.
 * GPIO/PORT: LL-2/3; TPM: LL-4/5; ADC: LL-6; ISR/task/queue: LL-7/8.
 * The supplied board clock/debug-console bootstrap is kept as in the labs.
 * LPUART1 retains PTE0/PTE1 so the existing wiring need not change.
 * The six-day reminder and environmental multipliers are demonstration rules,
 * not a validated prediction of mosquito development or a safe storage time.
 *
 * UART, 9600 8N1, newline-terminated ASCII:
 * TX $STAT,seconds,state,level,light,humidity,soil_dry,watering,mode,sensors_ok
 *    (humidity=255 means unknown/stale)
 * TX $EVT,id,FULL|CLEARED|EXPIRED|RESET ; RX $EACK,id
 * RX $CMD,id,WATER_ON|WATER_OFF|AUTO|MUTE|UNMUTE|RESET ; TX $ACK,id,result
 * RX $HUM,0..100 or $HUM,NA. Event retries are RAM-only, not reboot persistence.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "board.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "plant_rules.h"

#define BREED_WINDOW_S       (6UL * 24UL * 3600UL)
#ifndef DEMO_TIME_SCALE
#define DEMO_TIME_SCALE      3600UL /* Set to 1 for real time; fractions are retained. */
#endif
#define SAMPLE_PERIOD_MS     500U
#define LEVEL_EMPTY_ADC      400U
#define LEVEL_WET_ADC        600U
#define LIGHT_DARK_ADC       800U
#define DARK_LIMIT_MS        10000U /* Demonstration setting; calibrate for your use. */
#define HUMIDITY_HIGH_PCT    85U
#define HUMIDITY_TTL_MS      120000U
#define SENSOR_TTL_MS        2000U
#define WATER_LONG_MS        5000U
#define WATER_SHORT_MS       2500U
#define WATER_COOLDOWN_MS    20000U
#define MANUAL_WATER_MAX_MS  10000U
#define SOIL_DRY_LEVEL       1U
#define VALVE_OPEN_PCT       10U
#define VALVE_CLOSED_PCT     5U
#define STAT_PERIOD_MS       5000U
#define EVENT_RETRY_MS       3000U
#define PERIPH_CLK_HZ        48000000U
#define ESP_UART_BAUD        9600U
#define SOIL_PIN             12U /* PTA12, LM393 digital output, either edge. */
#define SOIL_PORT            PORTA
#define SOIL_GPIO            GPIOA
#define LIGHT_ADC_CH         3U  /* PTE22, on-board light */
#define WATER_ADC_CH         0U  /* PTE20, water sensor */
#define SERVO_TPM            TPM1 /* PTB0, ALT3, channel 0 */
#define BUZZ_TPM             TPM2 /* PTB2, ALT3, channel 0 */
#define ESP_UART             LPUART1 /* PTE0 TX / PTE1 RX, ALT3 */
#define UART_ERRORS (LPUART_STAT_OR_MASK | LPUART_STAT_NF_MASK | \
                     LPUART_STAT_FE_MASK | LPUART_STAT_PF_MASK)

typedef enum { ST_OK, ST_FULL, ST_EXPIRED } plate_state_t;
enum { EVENT_FULL, EVENT_CLEARED, EVENT_EXPIRED, EVENT_RESET, EVENT_COUNT };
static const char *const event_name[EVENT_COUNT] = { "FULL", "CLEARED", "EXPIRED", "RESET" };
typedef struct {
    uint16_t light, level;
    TickType_t tick;
    bool valid;
} sensor_sample_t;
typedef struct {
    int32_t time_left_s;
    uint32_t fraction, dark_ms;
    plate_state_t state;
    uint16_t level, light;
    uint8_t humidity;
    bool humidity_valid, sensors_valid, water_present, soil_dry, watering, muted;
    TickType_t humidity_tick, sensor_tick;
    water_mode_t water_mode;
    uint8_t pending_evt;
    uint32_t event_generation[EVENT_COUNT];
} system_state_t;
static system_state_t g_state;
static SemaphoreHandle_t stateMutex, soilSem;
static QueueHandle_t sensorQ, rxQ;
static bool adc_ready;
static volatile uint32_t rx_errors;
/* Only tComms owns the command cache and in-flight event. */
static bool event_active, event_acknowledged, command_seen;
static uint32_t event_id, event_generation, command_id;
static unsigned event_index;
static char command_reply[64];

static bool sensors_fresh(const system_state_t *s, TickType_t now)
{
    return plant_fresh(s->sensors_valid, now, s->sensor_tick, pdMS_TO_TICKS(SENSOR_TTL_MS));
}
static bool humidity_fresh(const system_state_t *s, TickType_t now)
{
    return plant_fresh(s->humidity_valid, now, s->humidity_tick, pdMS_TO_TICKS(HUMIDITY_TTL_MS));
}
static void raise_event(unsigned index) /* stateMutex must be held */
{
    g_state.pending_evt |= (uint8_t)(1U << index);
    g_state.event_generation[index]++;
}
static void reset_countdown_locked(void)
{
    g_state.time_left_s = (int32_t)BREED_WINDOW_S;
    g_state.fraction = 0U;
    g_state.dark_ms = 0U;
    g_state.state = ST_OK;
    g_state.water_present = false;
    g_state.muted = false;
}

/* ISRs do only flag handling and FromISR hand-off. */
void PORTA_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    if (SOIL_PORT->ISFR & (1UL << SOIL_PIN)) {
        SOIL_PORT->ISFR = 1UL << SOIL_PIN; /* Write one to clear, not read/modify/write. */
        xSemaphoreGiveFromISR(soilSem, &woken);
    }
    portYIELD_FROM_ISR(woken);
}
void LPUART1_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t status = ESP_UART->STAT;
    if (status & (LPUART_STAT_RDRF_MASK | UART_ERRORS)) {
        uint8_t byte = (uint8_t)ESP_UART->DATA;
        if (status & UART_ERRORS) {
            ESP_UART->STAT = (status & UART_ERRORS); /* W1C error flags. */
            rx_errors++;
        } else if (xQueueSendFromISR(rxQ, &byte, &woken) != pdTRUE) {
            rx_errors++;
        }
    }
    portYIELD_FROM_ISR(woken);
}

static bool adc_read(uint32_t channel, uint16_t *result)
{
    if (!adc_ready) return false;
    ADC0->SC1[0] = ADC_SC1_ADCH(channel); /* Single-ended, software trigger, no ADC IRQ. */
    for (uint32_t n = 0; n < 100000U; n++) {
        if (ADC0->SC1[0] & ADC_SC1_COCO_MASK) {
            *result = (uint16_t)ADC0->R[0];
            return true;
        }
    }
    return false; /* A faulty ADC cannot trap the poller in an endless busy wait. */
}
static bool soil_is_dry(void)
{
    return ((SOIL_GPIO->PDIR >> SOIL_PIN) & 1U) == SOIL_DRY_LEVEL;
}
static void watering_led(bool on)
{
    GPIOE->PSOR = (1UL << 31) | (1UL << 29);
    GPIOD->PSOR = 1UL << 5;
    if (on) GPIOE->PCOR = 1UL << 29; /* Blue, active low. */
}
static void pwm_duty(TPM_Type *tpm, uint32_t percent)
{
    tpm->CONTROLS[0].CnV = ((tpm->MOD + 1U) * percent) / 100U;
}
static void pwm_init(TPM_Type *tpm, uint32_t frequency, uint32_t percent)
{
    tpm->SC = 0U;
    tpm->CNT = 0U;
    tpm->MOD = (PERIPH_CLK_HZ / 16U / frequency) - 1U;
    tpm->CONTROLS[0].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    pwm_duty(tpm, percent);
    tpm->SC = TPM_SC_PS(4U) | TPM_SC_CMOD(1U); /* Divide by 16, internal clock. */
}
static bool uart_send_str(const char *text)
{
    while (*text) {
        TickType_t start = xTaskGetTickCount();
        while (!(ESP_UART->STAT & LPUART_STAT_TDRE_MASK)) {
            if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(50)) return false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        ESP_UART->DATA = (uint8_t)*text++;
    }
    return true;
}
static void hw_init(void)
{
    NVIC_DisableIRQ(PORTA_IRQn);
    NVIC_DisableIRQ(LPUART1_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTB_MASK |
                  SIM_SCGC5_PORTD_MASK | SIM_SCGC5_PORTE_MASK | SIM_SCGC5_LPUART1_MASK;
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK | SIM_SCGC6_TPM1_MASK | SIM_SCGC6_TPM2_MASK;

    /* LL-2: select GPIO, establish the OFF value, then set output direction. */
    PORTE->PCR[31] = PORT_PCR_MUX(1U);
    PORTE->PCR[29] = PORT_PCR_MUX(1U);
    PORTD->PCR[5] = PORT_PCR_MUX(1U);
    GPIOE->PSOR = (1UL << 31) | (1UL << 29);
    GPIOD->PSOR = 1UL << 5;
    GPIOE->PDDR |= (1UL << 31) | (1UL << 29);
    GPIOD->PDDR |= 1UL << 5;
    SOIL_PORT->PCR[SOIL_PIN] = PORT_PCR_MUX(1U) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
    SOIL_GPIO->PDDR &= ~(1UL << SOIL_PIN);

    /* LL-6: 12-bit, bus/2 clock divided by 4 (3 MHz), alternate board reference. */
    PORTE->PCR[22] = 0U;
    PORTE->PCR[20] = 0U;
    ADC0->CFG1 = ADC_CFG1_ADICLK(1U) | ADC_CFG1_MODE(1U) |
                 ADC_CFG1_ADIV(2U) | ADC_CFG1_ADLSMP_MASK;
    ADC0->CFG2 = 0U;
    ADC0->SC2 = ADC_SC2_REFSEL(1U); /* ValtH/ValtL, as required in LL-6 slide 23. */
    ADC0->SC3 = ADC_SC3_CALF_MASK;
    ADC0->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(3U) | ADC_SC3_CAL_MASK;
    uint32_t limit = 1000000U;
    while ((ADC0->SC3 & ADC_SC3_CAL_MASK) && --limit) { }
    adc_ready = limit != 0U && !(ADC0->SC3 & ADC_SC3_CALF_MASK);
    if (adc_ready) {
        uint32_t sum = ADC0->CLP0 + ADC0->CLP1 + ADC0->CLP2 + ADC0->CLP3 + ADC0->CLP4 + ADC0->CLPS;
        ADC0->PG = (sum / 2U) | 0x8000U;
        sum = ADC0->CLM0 + ADC0->CLM1 + ADC0->CLM2 + ADC0->CLM3 + ADC0->CLM4 + ADC0->CLMS;
        ADC0->MG = (sum / 2U) | 0x8000U;
    }
    ADC0->SC3 = 0U;

    /* LL-4/5: independent TPM periods, 50 Hz servo and 2 kHz passive buzzer. */
    SIM->SOPT2 = (SIM->SOPT2 & ~(SIM_SOPT2_TPMSRC_MASK | SIM_SOPT2_LPUART1SRC_MASK)) |
                 SIM_SOPT2_TPMSRC(1U) | SIM_SOPT2_LPUART1SRC(1U);
    PORTB->PCR[0] = PORT_PCR_MUX(3U);
    PORTB->PCR[2] = PORT_PCR_MUX(3U);
    pwm_init(SERVO_TPM, 50U, VALVE_CLOSED_PCT);
    pwm_init(BUZZ_TPM, 2000U, 0U);

    /* LL-8 register sequence, using LPUART1 instead of changing the wired UART. */
    ESP_UART->CTRL = 0U; /* Disable TX/RX, parity off, eight data bits. */
    PORTE->PCR[0] = PORT_PCR_MUX(3U);
    PORTE->PCR[1] = PORT_PCR_MUX(3U);
    SIM->SOPT5 &= ~(SIM_SOPT5_LPUART1TXSRC_MASK | SIM_SOPT5_LPUART1RXSRC_MASK);
    uint32_t sbr = (PERIPH_CLK_HZ + ESP_UART_BAUD * 8U) / (ESP_UART_BAUD * 16U);
    ESP_UART->BAUD = LPUART_BAUD_OSR(15U) | LPUART_BAUD_SBR(sbr);
    ESP_UART->STAT = UART_ERRORS;
    ESP_UART->CTRL = LPUART_CTRL_TE_MASK | LPUART_CTRL_RE_MASK;
}
static void enable_sensor_irqs(void)
{
    /* CMSIS takes unshifted 0..3 priorities on this M0+, not raw IPR byte values. */
    NVIC_SetPriority(PORTA_IRQn, 2U);
    NVIC_SetPriority(LPUART1_IRQn, 2U);
    SOIL_PORT->ISFR = 1UL << SOIL_PIN;
    SOIL_PORT->PCR[SOIL_PIN] |= PORT_PCR_IRQC(11U); /* Either edge. */
    NVIC_ClearPendingIRQ(PORTA_IRQn);
    NVIC_ClearPendingIRQ(LPUART1_IRQn);
    ESP_UART->CTRL |= LPUART_CTRL_RIE_MASK;
    NVIC_EnableIRQ(PORTA_IRQn);
    NVIC_EnableIRQ(LPUART1_IRQn);
}

static void tSensorPoll(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        sensor_sample_t sample = {0};
        sample.valid = adc_read(LIGHT_ADC_CH, &sample.light) && adc_read(WATER_ADC_CH, &sample.level);
        sample.tick = xTaskGetTickCount();
        xQueueSend(sensorQ, &sample, 0);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}
static void tCountdown(void *arg)
{
    (void)arg;
    sensor_sample_t sample;
    TickType_t previous = xTaskGetTickCount();
    for (;;) {
        if (xQueueReceive(sensorQ, &sample, portMAX_DELAY) != pdTRUE) continue;
        uint32_t elapsed = (TickType_t)(sample.tick - previous);
        previous = sample.tick;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        g_state.sensors_valid = sample.valid;
        g_state.sensor_tick = sample.tick;
        if (sample.valid) {
            g_state.level = sample.level;
            g_state.light = sample.light;
            uint32_t elapsed_ms = (uint32_t)((uint64_t)elapsed * 1000U / configTICK_RATE_HZ);
            if (sample.light < LIGHT_DARK_ADC) {
                uint32_t room = UINT32_MAX - g_state.dark_ms;
                g_state.dark_ms += elapsed_ms < room ? elapsed_ms : room;
            } else g_state.dark_ms = 0U;
            bool was_present = g_state.water_present;
            if (!was_present && sample.level >= LEVEL_WET_ADC) g_state.water_present = true;
            else if (was_present && sample.level < LEVEL_EMPTY_ADC) {
                reset_countdown_locked();
                raise_event(EVENT_CLEARED);
            }
            if (was_present && g_state.water_present && g_state.time_left_s > 0) {
                uint32_t rate = 100U;
                if (g_state.dark_ms >= DARK_LIMIT_MS) rate += 50U;
                if (humidity_fresh(&g_state, sample.tick) && g_state.humidity >= HUMIDITY_HIGH_PCT) rate += 50U;
                plant_countdown(&g_state.time_left_s, &g_state.fraction, elapsed,
                                configTICK_RATE_HZ, DEMO_TIME_SCALE, rate);
            }
            plate_state_t old = g_state.state;
            if (plant_full(old == ST_FULL, sample.level)) g_state.state = ST_FULL;
            else if (g_state.water_present && g_state.time_left_s == 0) g_state.state = ST_EXPIRED;
            else g_state.state = ST_OK;
            if (g_state.state != old) {
                if (g_state.state == ST_FULL) { raise_event(EVENT_FULL); g_state.muted = false; }
                if (g_state.state == ST_EXPIRED) { raise_event(EVENT_EXPIRED); g_state.muted = false; }
            }
        }
        xSemaphoreGive(stateMutex);
    }
}
static void tIrrigation(void *arg)
{
    (void)arg;
    plant_irrigation_t irrigation = {0};
    const plant_water_times_t time = {
        pdMS_TO_TICKS(WATER_LONG_MS), pdMS_TO_TICKS(WATER_SHORT_MS),
        pdMS_TO_TICKS(WATER_COOLDOWN_MS), pdMS_TO_TICKS(MANUAL_WATER_MAX_MS)
    };
    enable_sensor_irqs(); /* Queues exist and the scheduler is running. */
    for (;;) {
        xSemaphoreTake(soilSem, pdMS_TO_TICKS(250));
        bool dry = soil_is_dry();
        TickType_t now = xTaskGetTickCount();
        /* Decision and state update are one short mutex-protected operation. */
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        bool allowed = plant_can_water(sensors_fresh(&g_state, now), g_state.state == ST_FULL);
        /* Unknown humidity uses the shorter bounded burst. */
        bool short_burst = !humidity_fresh(&g_state, now) || g_state.humidity >= HUMIDITY_HIGH_PCT;
        plant_irrigation_step(&irrigation, &g_state.water_mode, allowed, dry, short_burst, now, &time);
        g_state.soil_dry = dry;
        g_state.watering = irrigation.watering;
        xSemaphoreGive(stateMutex);
    }
}
static void tActuator(void *arg)
{
    (void)arg;
    uint8_t beep = 0, previous_duty = 255;
    for (;;) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        system_state_t s = g_state;
        xSemaphoreGive(stateMutex);
        bool on = s.watering && plant_can_water(sensors_fresh(&s, xTaskGetTickCount()), s.state == ST_FULL);
        uint8_t duty = on ? VALVE_OPEN_PCT : VALVE_CLOSED_PCT;
        if (duty != previous_duty) { pwm_duty(SERVO_TPM, duty); previous_duty = duty; }
        watering_led(on);
        beep = (uint8_t)((beep + 1U) % 6U);
        bool alarm = (s.state == ST_FULL || s.state == ST_EXPIRED) && !s.muted;
        pwm_duty(BUZZ_TPM, alarm && beep < 3U ? 50U : 0U);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void handle_line(char *line)
{
    char *field[4];
    unsigned count = plant_fields(line, field, 4);
    uint32_t value;
    if (count == 2 && !strcmp(field[0], "$EACK") && plant_uint(field[1], &value)) {
        if (event_active && value == event_id) event_acknowledged = true;
        return;
    }
    if (count == 2 && !strcmp(field[0], "$HUM")) {
        bool valid = plant_uint(field[1], &value) && value <= 100U;
        if (!valid && strcmp(field[1], "NA")) return;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        g_state.humidity_valid = valid;
        if (valid) { g_state.humidity = (uint8_t)value; g_state.humidity_tick = xTaskGetTickCount(); }
        xSemaphoreGive(stateMutex);
        return;
    }
    if (count != 3 || strcmp(field[0], "$CMD") || !plant_uint(field[1], &value) || value == 0U) return;
    if (command_seen && value == command_id) { uart_send_str(command_reply); return; }
    const char *result = "OK";
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (!strcmp(field[2], "WATER_ON")) {
        if (!sensors_fresh(&g_state, xTaskGetTickCount())) result = "DENIED_SENSOR";
        else if (g_state.state == ST_FULL) result = "DENIED_FULL";
        else g_state.water_mode = WATER_FORCE_ON;
    } else if (!strcmp(field[2], "WATER_OFF")) g_state.water_mode = WATER_FORCE_OFF;
    else if (!strcmp(field[2], "AUTO")) g_state.water_mode = WATER_AUTO;
    else if (!strcmp(field[2], "MUTE")) g_state.muted = true;
    else if (!strcmp(field[2], "UNMUTE")) g_state.muted = false;
    else if (!strcmp(field[2], "RESET")) {
        if (!sensors_fresh(&g_state, xTaskGetTickCount()) || g_state.level >= LEVEL_EMPTY_ADC) result = "DENIED_NOT_EMPTY";
        else { reset_countdown_locked(); raise_event(EVENT_RESET); }
    } else result = "BAD_COMMAND";
    xSemaphoreGive(stateMutex);
    command_id = value;
    command_seen = true;
    snprintf(command_reply, sizeof(command_reply), "$ACK,%lu,%s\n", (unsigned long)value, result);
    uart_send_str(command_reply); /* No serial I/O while holding stateMutex. */
}
static void tComms(void *arg)
{
    (void)arg;
    plant_line_t line = {0};
    TickType_t last_stat = xTaskGetTickCount() - pdMS_TO_TICKS(STAT_PERIOD_MS);
    TickType_t last_event = xTaskGetTickCount() - pdMS_TO_TICKS(EVENT_RETRY_MS);
    uint32_t observed_errors = rx_errors;
    uart_send_str("$BOOT\n");
    for (;;) {
        /* Bound each receive batch; a stream of input cannot starve transmission. */
        for (unsigned n = 0; n < 64; n++) {
            uint8_t c;
            if (rx_errors != observed_errors) {
                observed_errors = rx_errors;
                line.discard = true;
                line.length = 0;
            }
            if (xQueueReceive(rxQ, &c, 0) != pdTRUE) break;
            if (plant_line_byte(&line, (char)c)) handle_line(line.text);
        }
        TickType_t now = xTaskGetTickCount();
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        if (event_active && event_acknowledged) {
            if (g_state.event_generation[event_index] == event_generation)
                g_state.pending_evt &= (uint8_t)~(1U << event_index);
            event_active = false;
            event_acknowledged = false;
        }
        if (!event_active && g_state.pending_evt) {
            for (event_index = 0; event_index < EVENT_COUNT; event_index++)
                if (g_state.pending_evt & (1U << event_index)) break;
            event_generation = g_state.event_generation[event_index];
            event_id++;
            if (event_id == 0U) event_id++;
            event_active = true;
            last_event = now - pdMS_TO_TICKS(EVENT_RETRY_MS);
        }
        system_state_t s = g_state;
        xSemaphoreGive(stateMutex);
        char out[PLANT_LINE_CAP];
        if (event_active && (TickType_t)(now - last_event) >= pdMS_TO_TICKS(EVENT_RETRY_MS)) {
            snprintf(out, sizeof(out), "$EVT,%lu,%s\n", (unsigned long)event_id, event_name[event_index]);
            uart_send_str(out);
            last_event = now;
        }
        if ((TickType_t)(now - last_stat) >= pdMS_TO_TICKS(STAT_PERIOD_MS)) {
            bool valid = sensors_fresh(&s, now);
            bool watering = s.watering && plant_can_water(valid, s.state == ST_FULL);
            snprintf(out, sizeof(out), "$STAT,%ld,%u,%u,%u,%u,%u,%u,%u,%u\n",
                     (long)s.time_left_s, (unsigned)s.state, s.level, s.light,
                     humidity_fresh(&s, now) ? s.humidity : 255U, s.soil_dry ? 1U : 0U,
                     watering ? 1U : 0U, (unsigned)s.water_mode, valid ? 1U : 0U);
            uart_send_str(out);
            last_stat = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
int main(void)
{
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();
    stateMutex = xSemaphoreCreateMutex();
    soilSem = xSemaphoreCreateBinary();
    sensorQ = xQueueCreate(8, sizeof(sensor_sample_t));
    rxQ = xQueueCreate(128, sizeof(uint8_t));
    configASSERT(stateMutex && soilSem && sensorQ && rxQ);
    memset(&g_state, 0, sizeof(g_state));
    reset_countdown_locked();
    g_state.water_mode = WATER_AUTO;
    hw_init();
    xSemaphoreGive(soilSem);
    PRINTF("Plant care: register peripherals + FreeRTOS\r\n");
    BaseType_t created = xTaskCreate(tIrrigation, "tIrrigation", 320, NULL, 3, NULL);
    configASSERT(created == pdPASS);
    created = xTaskCreate(tCountdown, "tCountdown", 320, NULL, 2, NULL);
    configASSERT(created == pdPASS);
    created = xTaskCreate(tSensorPoll, "tSensorPoll", 192, NULL, 2, NULL);
    configASSERT(created == pdPASS);
    created = xTaskCreate(tActuator, "tActuator", 192, NULL, 2, NULL);
    configASSERT(created == pdPASS);
    created = xTaskCreate(tComms, "tComms", 512, NULL, 1, NULL);
    configASSERT(created == pdPASS);
    vTaskStartScheduler();
    configASSERT(0);
    for (;;) { }
}
