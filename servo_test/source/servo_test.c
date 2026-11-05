/* FRDM-MCXC444 standalone servo test; no FreeRTOS, UART or plant logic.
 * Signal: PTB0, TPM1_CH0 (ALT3), same as the plant-care project.
 * Power the servo from a suitable 5 V supply and connect grounds together.
 * The PWM signal is 3.3 V logic; do not connect 5 V to PTB0.
 * Standard positional servo: 1000 -> 1500 us is approximately 90 degrees.
 * Actual travel depends on the servo. Test without the valve linkage first.
 */
#include "board.h"
#include "clock_config.h"

#define SERVO_CLOCK_HZ 48000000UL
#define SERVO_PRESCALER 16UL
#define SERVO_PERIOD_HZ 50UL
#define POSITION_A_US 500UL
#define POSITION_B_US 2500UL
#define HOLD_MS 2000UL

static void servo_position(uint32_t pulse_us)
{
    /* 48 MHz / 16 = 3 MHz: three counter ticks per microsecond. */
    TPM1->CONTROLS[0].CnV = (SERVO_CLOCK_HZ / SERVO_PRESCALER / 1000000UL) * pulse_us;
}

static void servo_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;
    SIM->SCGC6 |= SIM_SCGC6_TPM1_MASK;
    SIM->SOPT2 = (SIM->SOPT2 & ~SIM_SOPT2_TPMSRC_MASK) | SIM_SOPT2_TPMSRC(1U);
    PORTB->PCR[0] = PORT_PCR_MUX(3U);
    TPM1->SC = 0U;
    TPM1->CNT = 0U;
    TPM1->MOD = SERVO_CLOCK_HZ / SERVO_PRESCALER / SERVO_PERIOD_HZ - 1UL;
    TPM1->CONTROLS[0].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
    servo_position(POSITION_A_US);
    TPM1->SC = TPM_SC_PS(4U) | TPM_SC_CMOD(1U);
}

static void delay_ms(uint32_t ms)
{
    /* Poll the core timer: no SysTick interrupts or software timing loop. */
    SysTick->LOAD = SystemCoreClock / 1000UL - 1UL;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    while (ms--) {
        while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk)) { }
    }
    SysTick->CTRL = 0U;
}

int main(void)
{
    BOARD_InitBootClocks();
    servo_init();
    for (;;) {
        servo_position(POSITION_A_US);
        delay_ms(HOLD_MS);
        servo_position(POSITION_B_US);
        delay_ms(HOLD_MS);
    }
}
