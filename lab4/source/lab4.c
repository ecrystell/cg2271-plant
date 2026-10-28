/*
 * dengue_mcxc444.c — Smart Anti-Dengue Plant Care System (MVP)
 * Target : FRDM-MCXC444, MCUXpresso SDK + FreeRTOS
 *
 * Role of this board
 *   - Soil moisture sensor (interrupt): decides watering (with humidity)
 *   - Water level sensor (polled): water collecting in the plate under the pot —
 *     used only for the mosquito breeding countdown and "empty the plate" alerts
 *   - On-board light sensor (polled): long darkness speeds up mosquito breeding
 *   - Humidity arrives from the ESP32 over UART: decides watering length and
 *     speeds up the breeding countdown
 *   - Servo: turns the sprinkler valve on / off
 *   - On-board RGB LED: lights blue while watering
 *   - Buzzer: tells the user to empty the plate (full, or 6-day window used up)
 *
 * RTOS objects
 *   ISRs      : PORTA_IRQHandler (soil moisture DO edge), LPUART1_IRQHandler (UART RX)
 *   Tasks     : tIrrigation (prio 3), tCountdown (prio 2), tSensorPoll (prio 2),
 *               tActuator (prio 2), tComms (prio 1)
 *               -> three equal-priority tasks share the CPU by time-slicing
 *   Semaphore : soilSem    (binary, soil ISR -> tIrrigation)
 *   Mutex     : stateMutex (guards g_state)
 *   Queues    : sensorQ    (tSensorPoll -> tCountdown), rxQ (UART ISR -> tComms)
 *
 * Required FreeRTOSConfig.h settings
 *   #define configUSE_PREEMPTION                1
 *   #define configUSE_TIME_SLICING              1
 *   #define configUSE_MUTEXES                   1
 *   #define configUSE_TIMERS                    0   // not used
 *   #define configUSE_CO_ROUTINES               0   // not used
 *   #define configUSE_TRACE_FACILITY            0   // not used
 *   #define configGENERATE_RUN_TIME_STATS       0   // not used
 *   #define configUSE_IDLE_HOOK                 0
 *   #define configUSE_TICK_HOOK                 0
 *   #define configTICK_RATE_HZ                  1000
 *
 * UART protocol (9600 8N1, ASCII lines ending in '\n') — unchanged, so the ESP32
 * sketch works as-is:
 *   MCXC444 -> ESP32 : $STAT,<time_left_s>,<state>,<level>,<light>,<humidity>,<soil_dry>,<watering>
 *                      (ESP32 reads the first 5 fields; the last two are extra info)
 *                      $EVT,FULL | $EVT,CLEARED | $EVT,EXPIRED
 *   ESP32 -> MCXC444 : $HUM,<%RH integer>
 *                      $CMD,LID_OPEN   -> sprinkler ON  (manual, auto-stops after 10 s)
 *                      $CMD,LID_CLOSE  -> sprinkler OFF (manual hold)
 *                      $CMD,LID_AUTO   -> back to automatic watering
 *                      $CMD,MUTE       -> silence the buzzer until the plate is emptied
 *                      $CMD,RESET      -> reset the breeding countdown
 *
 * !! PIN NOTE: pins marked VERIFY are placeholders. Check them against the FRDM-MCXC444
 *    schematic / your lab pinout. Each is a #define, so a change is one line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "board.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "fsl_clock.h"
#include "fsl_port.h"
#include "fsl_gpio.h"
#include "fsl_adc16.h"
#include "fsl_tpm.h"
#include "fsl_lpuart.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ------------------------------------------------------------------ */
/* Demo / tuning parameters                                            */
/* ------------------------------------------------------------------ */
#define BREED_WINDOW_S        (6UL * 24UL * 3600UL)   /* 6 days in seconds              */
#define DEMO_TIME_SCALE       3600UL   /* 1 real second = 1 simulated hour (6 days ~ 2.4 min).
                                          Set to 1 for real-time.                          */
#define SAMPLE_PERIOD_MS      500U

/* Plate water level (12-bit ADC)                                                  calibrate */
#define LEVEL_EMPTY_ADC       400U     /* below this = plate empty                          */
#define LEVEL_WET_ADC         600U     /* above this = water present, countdown runs        */
#define LEVEL_FULL_ADC        3000U    /* at/above this = plate full                        */
#define LEVEL_HYST_ADC        150U

/* Light (on-board sensor)                                                         calibrate */
#define LIGHT_DARK_ADC        800U     /* below this = dark                                 */
#define DARK_LIMIT_MS         10000U   /* dark longer than this -> countdown speeds up      */

/* Humidity (from ESP32) */
#define HUMIDITY_HIGH_PCT     85U      /* at/above: shorter watering, faster countdown      */
#define DARK_PENALTY_PCT      50U      /* +50% countdown speed while dark too long          */
#define HUMID_PENALTY_PCT     50U      /* +50% countdown speed while humid                  */

/* Watering (demo values) */
#define WATER_LONG_MS         5000U    /* soil dry, normal humidity                         */
#define WATER_SHORT_MS        2500U    /* soil dry, high humidity -> less water needed      */
#define WATER_COOLDOWN_MS     20000U   /* let water soak in before watering again           */
#define MANUAL_WATER_MAX_MS   10000U   /* safety cap on a manual "on" command               */
#define SOIL_DRY_LEVEL        1U       /* DO pin level meaning "dry" (most LM393 modules: 1) */

/* Actuators */
#define VALVE_OPEN_PCT        10U      /* servo 2.0 ms pulse @ 50 Hz -> sprinkler on        */
#define VALVE_CLOSED_PCT      5U       /* servo 1.0 ms pulse @ 50 Hz -> sprinkler off       */
#define BUZZER_FREQ_HZ        2000U
#define STAT_PERIOD_MS        5000U

/* ------------------------------------------------------------------ */
/* Pin map                                                             */
/* ------------------------------------------------------------------ */
/* On-board RGB LED (active low) — per FRDM-MCXC444 board docs */
#define LED_R_PORT  PORTE
#define LED_R_GPIO  GPIOE
#define LED_R_PIN   31U
#define LED_G_PORT  PORTD
#define LED_G_GPIO  GPIOD
#define LED_G_PIN   5U
#define LED_B_PORT  PORTE
#define LED_B_GPIO  GPIOE
#define LED_B_PIN   29U

/* Soil moisture module digital output (LM393 comparator, threshold set by its pot)  VERIFY */
#define SOIL_PORT   PORTA
#define SOIL_GPIO   GPIOA
#define SOIL_PIN    12U
#define SOIL_IRQn   PORTA_IRQn

/* ADC channels                                                       VERIFY */
#define LIGHT_ADC_CH   3U    /* on-board light sensor, PTE22 / ADC0_SE3 */
#define LIGHT_PORT     PORTE
#define LIGHT_PIN      22U
#define WATER_ADC_CH   0U    /* plate water level sensor, PTE20 / ADC0_SE0 */
#define WATER_PORT     PORTE
#define WATER_PIN      20U

/* Servo (sprinkler valve): TPM1 CH0 on PTB0 (ALT3)                   VERIFY */
#define SERVO_TPM      TPM1
#define SERVO_CH       kTPM_Chnl_0
#define SERVO_PORT     PORTB
#define SERVO_PIN      0U
#define SERVO_MUX      kPORT_MuxAlt3

/* Buzzer: TPM2 CH0 on PTB2 (ALT3)                                    VERIFY */
#define BUZZ_TPM       TPM2
#define BUZZ_CH        kTPM_Chnl_0
#define BUZZ_PORT      PORTB
#define BUZZ_PIN       2U
#define BUZZ_MUX       kPORT_MuxAlt3

/* UART link to ESP32: LPUART1, TX PTE0 / RX PTE1 (ALT3)              VERIFY */
#define ESP_UART       LPUART1
#define ESP_UART_IRQn  LPUART1_IRQn
#define ESP_UART_PORT  PORTE
#define ESP_UART_TX    0U
#define ESP_UART_RX    1U
#define ESP_UART_MUX   kPORT_MuxAlt3
#define ESP_UART_BAUD  9600U

/* Peripheral clock: IRC48M selected for TPM and LPUART below */
#define PERIPH_CLK_HZ  48000000U

/* ------------------------------------------------------------------ */
/* Types and shared state                                              */
/* ------------------------------------------------------------------ */
/* Plate state — numbering kept so the ESP32 shows the right text */
typedef enum { ST_OK = 0, ST_FULL = 1, ST_EXPIRED = 2 } plate_state_t;

typedef enum { WATER_AUTO = 0, WATER_FORCE_ON, WATER_FORCE_OFF } water_mode_t;

#define EVT_FULL     (1U << 0)
#define EVT_CLEARED  (1U << 1)
#define EVT_EXPIRED  (1U << 2)

typedef struct {
    uint16_t light;
    uint16_t level;
} sensor_sample_t;

typedef struct {
    int32_t       time_left_s;   /* breeding countdown, simulated seconds */
    plate_state_t state;
    uint16_t      level;
    uint16_t      light;
    uint8_t       humidity;      /* %RH, from ESP32                        */
    bool          soil_dry;
    bool          watering;      /* sprinkler currently on                  */
    water_mode_t  water_mode;    /* auto, or manual override from Telegram  */
    bool          muted;
    uint8_t       pending_evt;   /* EVT_* bits waiting to be sent           */
} system_state_t;

static system_state_t    g_state;
static SemaphoreHandle_t stateMutex;
static SemaphoreHandle_t soilSem;
static QueueHandle_t     sensorQ;
static QueueHandle_t     rxQ;

/* ------------------------------------------------------------------ */
/* ISRs — kept short: clear flag, hand off to a task, yield if needed  */
/* ------------------------------------------------------------------ */
void PORTA_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t flags = PORT_GetPinsInterruptFlags(SOIL_PORT);

    if (flags & (1UL << SOIL_PIN)) {
        xSemaphoreGiveFromISR(soilSem, &woken);   /* "soil changed wet <-> dry" */
    }
    PORT_ClearPinsInterruptFlags(SOIL_PORT, flags);
    portYIELD_FROM_ISR(woken);
}

void LPUART1_IRQHandler(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t flags = LPUART_GetStatusFlags(ESP_UART);

    if (flags & kLPUART_RxOverrunFlag) {
        LPUART_ClearStatusFlags(ESP_UART, kLPUART_RxOverrunFlag);
    }
    if (flags & kLPUART_RxDataRegFullFlag) {
        uint8_t c = LPUART_ReadByte(ESP_UART);
        xQueueSendFromISR(rxQ, &c, &woken);       /* drop byte if queue full */
    }
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ */
/* Hardware helpers                                                    */
/* ------------------------------------------------------------------ */
static uint16_t adc_read(uint32_t channel)
{
    adc16_channel_config_t ch = {
        .channelNumber = channel,
        .enableInterruptOnConversionCompleted = false,
        .enableDifferentialConversion = false,
    };
    ADC16_SetChannelConfig(ADC0, 0U, &ch);
    while (!(ADC16_GetChannelStatusFlags(ADC0, 0U) & kADC16_ChannelConversionDoneFlag)) { }
    return (uint16_t)ADC16_GetChannelConversionValue(ADC0, 0U);
}

static void rgb_set(bool r, bool g, bool b)
{
    /* active low */
    GPIO_PinWrite(LED_R_GPIO, LED_R_PIN, r ? 0U : 1U);
    GPIO_PinWrite(LED_G_GPIO, LED_G_PIN, g ? 0U : 1U);
    GPIO_PinWrite(LED_B_GPIO, LED_B_PIN, b ? 0U : 1U);
}

static bool soil_is_dry(void)
{
    return GPIO_PinRead(SOIL_GPIO, SOIL_PIN) == SOIL_DRY_LEVEL;
}

static void uart_send_str(const char *s)
{
    LPUART_WriteBlocking(ESP_UART, (const uint8_t *)s, strlen(s));
}

static void hw_init(void)
{
    CLOCK_EnableClock(kCLOCK_PortA);
    CLOCK_EnableClock(kCLOCK_PortB);
    CLOCK_EnableClock(kCLOCK_PortD);
    CLOCK_EnableClock(kCLOCK_PortE);

    /* --- RGB LED --- */
    gpio_pin_config_t out_hi = { kGPIO_DigitalOutput, 1U };
    PORT_SetPinMux(LED_R_PORT, LED_R_PIN, kPORT_MuxAsGpio);
    PORT_SetPinMux(LED_G_PORT, LED_G_PIN, kPORT_MuxAsGpio);
    PORT_SetPinMux(LED_B_PORT, LED_B_PIN, kPORT_MuxAsGpio);
    GPIO_PinInit(LED_R_GPIO, LED_R_PIN, &out_hi);
    GPIO_PinInit(LED_G_GPIO, LED_G_PIN, &out_hi);
    GPIO_PinInit(LED_B_GPIO, LED_B_PIN, &out_hi);

    /* --- Soil moisture DO: GPIO input, interrupt on both edges (wet->dry and dry->wet) --- */
    gpio_pin_config_t in = { kGPIO_DigitalInput, 0U };
    PORT_SetPinMux(SOIL_PORT, SOIL_PIN, kPORT_MuxAsGpio);
    GPIO_PinInit(SOIL_GPIO, SOIL_PIN, &in);
    PORT_SetPinInterruptConfig(SOIL_PORT, SOIL_PIN, kPORT_InterruptEitherEdge);
    NVIC_SetPriority(SOIL_IRQn, 2);
    EnableIRQ(SOIL_IRQn);

    /* --- ADC0 (light + water level) --- */
    PORT_SetPinMux(LIGHT_PORT, LIGHT_PIN, kPORT_PinDisabledOrAnalog);
    PORT_SetPinMux(WATER_PORT, WATER_PIN, kPORT_PinDisabledOrAnalog);
    adc16_config_t adc_cfg;
    ADC16_GetDefaultConfig(&adc_cfg);
    adc_cfg.resolution = kADC16_ResolutionSE12Bit;
    ADC16_Init(ADC0, &adc_cfg);
    ADC16_EnableHardwareTrigger(ADC0, false);
    (void)ADC16_DoAutoCalibration(ADC0);

    /* --- TPM: servo (50 Hz) and buzzer (2 kHz) --- */
    CLOCK_SetTpmClock(1U);   /* TPM clock source = IRC48M */
    PORT_SetPinMux(SERVO_PORT, SERVO_PIN, SERVO_MUX);
    PORT_SetPinMux(BUZZ_PORT, BUZZ_PIN, BUZZ_MUX);

    tpm_config_t tpm_cfg;
    TPM_GetDefaultConfig(&tpm_cfg);
    tpm_cfg.prescale = kTPM_Prescale_Divide_16;   /* 48 MHz / 16 = 3 MHz */

    tpm_chnl_pwm_signal_param_t servo_ch = {
        .chnlNumber = SERVO_CH, .level = kTPM_HighTrue, .dutyCyclePercent = VALVE_CLOSED_PCT };
    TPM_Init(SERVO_TPM, &tpm_cfg);
    TPM_SetupPwm(SERVO_TPM, &servo_ch, 1U, kTPM_EdgeAlignedPwm, 50U, PERIPH_CLK_HZ);
    TPM_StartTimer(SERVO_TPM, kTPM_SystemClock);

    tpm_chnl_pwm_signal_param_t buzz_ch = {
        .chnlNumber = BUZZ_CH, .level = kTPM_HighTrue, .dutyCyclePercent = 0U };
    TPM_Init(BUZZ_TPM, &tpm_cfg);
    TPM_SetupPwm(BUZZ_TPM, &buzz_ch, 1U, kTPM_EdgeAlignedPwm, BUZZER_FREQ_HZ, PERIPH_CLK_HZ);
    TPM_StartTimer(BUZZ_TPM, kTPM_SystemClock);

    /* --- LPUART1 to ESP32, RX interrupt --- */
    CLOCK_SetLpuart1Clock(1U);   /* LPUART1 clock source = IRC48M */
    PORT_SetPinMux(ESP_UART_PORT, ESP_UART_TX, ESP_UART_MUX);
    PORT_SetPinMux(ESP_UART_PORT, ESP_UART_RX, ESP_UART_MUX);
    lpuart_config_t u_cfg;
    LPUART_GetDefaultConfig(&u_cfg);
    u_cfg.baudRate_Bps = ESP_UART_BAUD;
    u_cfg.enableTx = true;
    u_cfg.enableRx = true;
    LPUART_Init(ESP_UART, &u_cfg, PERIPH_CLK_HZ);
    LPUART_EnableInterrupts(ESP_UART, kLPUART_RxDataRegFullInterruptEnable);
    NVIC_SetPriority(ESP_UART_IRQn, 2);
    EnableIRQ(ESP_UART_IRQn);
}

/* ------------------------------------------------------------------ */
/* Task 1: tSensorPoll — fixed-rate polling of light + plate water level */
/* ------------------------------------------------------------------ */
static void tSensorPoll(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    sensor_sample_t s;

    for (;;) {
        s.light = adc_read(LIGHT_ADC_CH);
        s.level = adc_read(WATER_ADC_CH);
        xQueueSend(sensorQ, &s, 0);               /* never block the poller */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* Task 2: tCountdown — breeding countdown for water in the plate       */
/* ------------------------------------------------------------------ */
static void reset_countdown_locked(void)
{
    g_state.time_left_s = (int32_t)BREED_WINDOW_S;
    g_state.state       = ST_OK;
    g_state.muted       = false;
}

static void tCountdown(void *arg)
{
    (void)arg;
    sensor_sample_t s;
    uint32_t dark_ms = 0;
    bool water_present = false;

    for (;;) {
        if (xQueueReceive(sensorQ, &s, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        dark_ms = (s.light < LIGHT_DARK_ADC) ? dark_ms + SAMPLE_PERIOD_MS : 0U;

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        g_state.level = s.level;
        g_state.light = s.light;

        /* 1. Water appears / plate emptied (hysteresis between EMPTY and WET) */
        if (!water_present && s.level >= LEVEL_WET_ADC) {
            water_present = true;                 /* countdown starts running */
        } else if (water_present && s.level < LEVEL_EMPTY_ADC) {
            water_present = false;
            reset_countdown_locked();             /* emptied -> fresh 6 days */
            g_state.pending_evt |= EVT_CLEARED;
            PRINTF("[cnt] plate emptied, timer reset\r\n");
        }

        /* 2. Countdown only while water is standing in the plate */
        if (water_present && g_state.time_left_s > 0) {
            uint32_t rate_pct = 100U;
            if (dark_ms >= DARK_LIMIT_MS)              rate_pct += DARK_PENALTY_PCT;
            if (g_state.humidity >= HUMIDITY_HIGH_PCT) rate_pct += HUMID_PENALTY_PCT;

            uint32_t elapsed_sim_s = (SAMPLE_PERIOD_MS * DEMO_TIME_SCALE) / 1000U;
            g_state.time_left_s -= (int32_t)((elapsed_sim_s * rate_pct) / 100U);
            if (g_state.time_left_s < 0) g_state.time_left_s = 0;
        }

        /* 3. Plate state: FULL has priority, then EXPIRED */
        plate_state_t prev = g_state.state;
        if (s.level >= LEVEL_FULL_ADC ||
            (prev == ST_FULL && s.level >= (LEVEL_FULL_ADC - LEVEL_HYST_ADC))) {
            g_state.state = ST_FULL;
        } else if (water_present && g_state.time_left_s == 0) {
            g_state.state = ST_EXPIRED;
        } else {
            g_state.state = ST_OK;
        }
        if (g_state.state != prev) {
            if (g_state.state == ST_FULL)    g_state.pending_evt |= EVT_FULL;
            if (g_state.state == ST_EXPIRED) g_state.pending_evt |= EVT_EXPIRED;
        }
        xSemaphoreGive(stateMutex);
    }
}

/* ------------------------------------------------------------------ */
/* Task 3: tIrrigation — decides when the sprinkler runs                */
/*   Woken by soilSem (soil changed) or every 250 ms to time a burst.   */
/* ------------------------------------------------------------------ */
static void tIrrigation(void *arg)
{
    (void)arg;
    bool watering = false;
    TickType_t water_end = 0;
    TickType_t cooldown_end = 0;

    for (;;) {
        bool soil_changed = (xSemaphoreTake(soilSem, pdMS_TO_TICKS(250)) == pdTRUE);
        bool dry = soil_is_dry();
        TickType_t now = xTaskGetTickCount();

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        water_mode_t mode = g_state.water_mode;
        uint8_t humidity  = g_state.humidity;
        xSemaphoreGive(stateMutex);

        if (soil_changed) {
            PRINTF("[irr] soil now %s\r\n", dry ? "DRY" : "wet");
        }

        if (mode == WATER_FORCE_ON) {
            if (!watering) {                      /* manual on, capped for safety */
                watering = true;
                water_end = now + pdMS_TO_TICKS(MANUAL_WATER_MAX_MS);
            } else if ((int32_t)(now - water_end) >= 0) {
                watering = false;
                mode = WATER_AUTO;                /* manual burst done -> back to auto */
                cooldown_end = now + pdMS_TO_TICKS(WATER_COOLDOWN_MS);
            }
        } else if (mode == WATER_FORCE_OFF) {
            watering = false;
        } else if (watering) {
            /* stop when the soil is wet again or the burst is over */
            if (!dry || (int32_t)(now - water_end) >= 0) {
                watering = false;
                cooldown_end = now + pdMS_TO_TICKS(WATER_COOLDOWN_MS);
            }
        } else if (dry && (int32_t)(now - cooldown_end) >= 0) {
            /* humid air -> plant loses less water -> shorter burst */
            uint32_t burst = (humidity >= HUMIDITY_HIGH_PCT) ? WATER_SHORT_MS : WATER_LONG_MS;
            watering = true;
            water_end = now + pdMS_TO_TICKS(burst);
            PRINTF("[irr] watering %lu ms (humidity %u%%)\r\n", (unsigned long)burst, humidity);
        }

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        g_state.soil_dry = dry;
        g_state.watering = watering;
        if (g_state.water_mode == WATER_FORCE_ON && mode == WATER_AUTO) {
            g_state.water_mode = WATER_AUTO;
        }
        xSemaphoreGive(stateMutex);
    }
}

/* ------------------------------------------------------------------ */
/* Task 4: tActuator — servo valve, RGB LED, buzzer                     */
/* ------------------------------------------------------------------ */
static void tActuator(void *arg)
{
    (void)arg;
    system_state_t snap;
    uint8_t beep = 0;
    uint8_t last_servo = 0xFF;

    for (;;) {
        /* short critical section: copy and release */
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        snap = g_state;
        xSemaphoreGive(stateMutex);

        /* Servo: sprinkler valve follows the watering decision */
        uint8_t servo_pct = snap.watering ? VALVE_OPEN_PCT : VALVE_CLOSED_PCT;
        if (servo_pct != last_servo) {
            TPM_UpdatePwmDutycycle(SERVO_TPM, SERVO_CH, kTPM_EdgeAlignedPwm, servo_pct);
            last_servo = servo_pct;
        }

        /* LED: blue while watering, off otherwise */
        rgb_set(false, false, snap.watering);

        /* Buzzer: chirp (300 ms on / 300 ms off) to ask the user to empty the plate */
        bool alarm = (snap.state == ST_FULL || snap.state == ST_EXPIRED) && !snap.muted;
        beep = (uint8_t)((beep + 1U) % 6U);
        TPM_UpdatePwmDutycycle(BUZZ_TPM, BUZZ_CH, kTPM_EdgeAlignedPwm,
                               (alarm && beep < 3U) ? 50U : 0U);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------------ */
/* Task 5: tComms — parse ESP32 frames, send status + events            */
/* ------------------------------------------------------------------ */
static void handle_line(const char *line)
{
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (strncmp(line, "$HUM,", 5) == 0) {
        int h = atoi(line + 5);
        if (h >= 0 && h <= 100) g_state.humidity = (uint8_t)h;
    } else if (strcmp(line, "$CMD,LID_OPEN") == 0) {        /* Telegram /open  */
        g_state.water_mode = WATER_FORCE_ON;
    } else if (strcmp(line, "$CMD,LID_CLOSE") == 0) {       /* Telegram /close */
        g_state.water_mode = WATER_FORCE_OFF;
    } else if (strcmp(line, "$CMD,LID_AUTO") == 0) {        /* Telegram /auto  */
        g_state.water_mode = WATER_AUTO;
    } else if (strcmp(line, "$CMD,MUTE") == 0) {
        g_state.muted = true;
    } else if (strcmp(line, "$CMD,RESET") == 0) {
        reset_countdown_locked();
        g_state.pending_evt |= EVT_CLEARED;
    }
    xSemaphoreGive(stateMutex);
    PRINTF("[rx] %s\r\n", line);
}

static void tComms(void *arg)
{
    (void)arg;
    char line[48];
    uint8_t len = 0;
    char out[96];
    TickType_t last_stat = xTaskGetTickCount();

    for (;;) {
        uint8_t c;
        /* 1. Drain incoming bytes (blocks up to 50 ms so status still goes out on time) */
        while (xQueueReceive(rxQ, &c, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                if (len > 0 && line[0] == '$') handle_line(line);
                len = 0;
            } else if (len < sizeof(line) - 1U) {
                line[len++] = (char)c;
            } else {
                len = 0;   /* overlong line: discard */
            }
        }

        /* 2. Snapshot state, take pending events */
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        system_state_t snap = g_state;
        g_state.pending_evt = 0;
        xSemaphoreGive(stateMutex);

        if (snap.pending_evt & EVT_FULL)    uart_send_str("$EVT,FULL\n");
        if (snap.pending_evt & EVT_CLEARED) uart_send_str("$EVT,CLEARED\n");
        if (snap.pending_evt & EVT_EXPIRED) uart_send_str("$EVT,EXPIRED\n");

        /* 3. Periodic status frame */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(STAT_PERIOD_MS)) {
            last_stat = xTaskGetTickCount();
            snprintf(out, sizeof(out), "$STAT,%ld,%d,%u,%u,%u,%d,%d\n",
                     (long)snap.time_left_s, (int)snap.state,
                     (unsigned)snap.level, (unsigned)snap.light, (unsigned)snap.humidity,
                     snap.soil_dry ? 1 : 0, snap.watering ? 1 : 0);
            uart_send_str(out);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    /* RTOS objects must exist before the ISRs are enabled in hw_init() */
    stateMutex = xSemaphoreCreateMutex();
    soilSem    = xSemaphoreCreateBinary();
    sensorQ    = xQueueCreate(8, sizeof(sensor_sample_t));
    rxQ        = xQueueCreate(64, sizeof(uint8_t));

    memset(&g_state, 0, sizeof(g_state));
    g_state.time_left_s = (int32_t)BREED_WINDOW_S;
    g_state.state       = ST_OK;
    g_state.water_mode  = WATER_AUTO;

    hw_init();
    xSemaphoreGive(soilSem);   /* make tIrrigation check the soil once at start-up */
    PRINTF("Plant care / anti-dengue MCXC444 MVP starting\r\n");

    xTaskCreate(tIrrigation, "tIrrigation", 256, NULL, 3, NULL);
    xTaskCreate(tCountdown,  "tCountdown",  256, NULL, 2, NULL);
    xTaskCreate(tSensorPoll, "tSensorPoll", 192, NULL, 2, NULL);
    xTaskCreate(tActuator,   "tActuator",   192, NULL, 2, NULL);
    xTaskCreate(tComms,      "tComms",      320, NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;) { }
}
