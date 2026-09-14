/**
 * @file main.c
 * @brief Embedded smart-lock controller for the TM4C123GH6PM.
 *
 * Implements a keypad-controlled locking system with potentiometer-based
 * secondary validation, servo actuation, status LEDs, intrusion detection,
 * UART diagnostics, and an inactivity timeout.
 *
 * Hardware platform: TI TM4C123GH6PM / Tiva C Series
 * Dependencies: TI TivaWare Peripheral Driver Library
 *
 * Authors: Takeru Hiura, Shane Duffy
 */

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_memmap.h"
#include "inc/tm4c123gh6pm.h"
#include "driverlib/adc.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/pwm.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/watchdog.h"

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

#define PASSWORD_LENGTH                  4U
#define UART_BAUD_RATE                   115200U
#define POTENTIOMETER_UNLOCK_THRESHOLD   1241U

#define SERVO_PWM_PERIOD_TICKS           40000U
#define SERVO_LOCKED_PULSE_TICKS         5000U
#define SERVO_UNLOCKED_PULSE_TICKS       1000U
#define UNLOCK_DURATION_SECONDS          5U

#define WATCHDOG_TIMEOUT_SECONDS          10U
#define WATCHDOG_RELOAD_TICKS            (SysCtlClockGet() * WATCHDOG_TIMEOUT_SECONDS)

#define STATUS_LED_GREEN                 GPIO_PIN_2
#define STATUS_LED_RED                   GPIO_PIN_3
#define IR_SENSOR_PIN                    GPIO_PIN_0
#define SERVO_PIN                        GPIO_PIN_1

#define KEYPAD_ROW_MASK                  0x0FU   /* PE0-PE3 */
#define KEYPAD_COLUMN_MASK               0xF0U   /* PC4-PC7 */

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

typedef enum {
    LOCK_STATE_LOCKED = 0,
    LOCK_STATE_UNLOCKED,
    LOCK_STATE_INTRUDER
} LockState;

/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

static const int kPassword[PASSWORD_LENGTH] = {1, 2, 3, 4};
static int g_userInput[PASSWORD_LENGTH] = {0};
static volatile uint32_t g_inputIndex = 0U;
static volatile LockState g_lockState = LOCK_STATE_LOCKED;
static volatile bool g_keypadActivity = true;

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static void gpio_portf_input_init(uint8_t pins);
static void gpio_portf_output_init(uint8_t pins);
static void uart_init(void);
static void uart_write(const char *message);
static void uart_write_line(const char *message);
static void servo_init(void);
static void adc_init(void);
static uint32_t adc_read_potentiometer(void);
static void keypad_init(void);
static int keypad_read(void);
static void watchdog_init(void);
static void reset_user_input(void);
static bool password_matches(void);
static void update_lock_outputs(void);

void IntruderAlertHandler(void);
void WatchdogIntHandler(void);

/* -------------------------------------------------------------------------- */
/* Application entry point                                                    */
/* -------------------------------------------------------------------------- */

int main(void)
{
    gpio_portf_input_init(IR_SENSOR_PIN);
    gpio_portf_output_init(STATUS_LED_GREEN | STATUS_LED_RED);

    uart_init();
    keypad_init();
    servo_init();
    adc_init();
    watchdog_init();

    /* IR sensor is active-low and triggers when motion/entry is detected. */
    GPIOIntClear(GPIO_PORTF_BASE, IR_SENSOR_PIN);
    GPIOIntTypeSet(GPIO_PORTF_BASE, IR_SENSOR_PIN, GPIO_FALLING_EDGE);
    GPIOIntRegister(GPIO_PORTF_BASE, IntruderAlertHandler);
    GPIOIntEnable(GPIO_PORTF_BASE, IR_SENSOR_PIN);

    IntMasterEnable();

    uart_write_line("Smart-lock controller initialized.");

    while (1) {
        const uint32_t adcValue = adc_read_potentiometer();
        const int key = keypad_read();

        if (key != -1 && g_inputIndex < PASSWORD_LENGTH) {
            g_userInput[g_inputIndex] = key;
            g_inputIndex++;
            g_keypadActivity = true;

            uart_write("Key accepted (digit ");
            UARTCharPut(UART0_BASE, (char)('0' + g_inputIndex));
            uart_write_line(").");
        }

        if (g_inputIndex == PASSWORD_LENGTH) {
            const bool validPassword = password_matches();
            const bool validPotentiometer =
                (adcValue > POTENTIOMETER_UNLOCK_THRESHOLD);

            if (validPassword && validPotentiometer) {
                g_lockState = LOCK_STATE_UNLOCKED;
                uart_write_line("Access granted.");
            } else {
                uart_write_line("Access denied.");
            }

            reset_user_input();
        }

        update_lock_outputs();
    }
}

/* -------------------------------------------------------------------------- */
/* GPIO                                                                       */
/* -------------------------------------------------------------------------- */

/** Configure one or more Port F pins as digital inputs with pull-ups. */
static void gpio_portf_input_init(uint8_t pins)
{
    SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R5;
    while ((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R5) == 0U) {
        /* Wait for Port F to become ready. */
    }

    GPIO_PORTF_LOCK_R = GPIO_LOCK_KEY;
    GPIO_PORTF_CR_R |= pins;
    GPIO_PORTF_DIR_R &= ~pins;
    GPIO_PORTF_PUR_R |= pins;
    GPIO_PORTF_DEN_R |= pins;
}

/** Configure one or more Port F pins as digital outputs. */
static void gpio_portf_output_init(uint8_t pins)
{
    SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R5;
    while ((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R5) == 0U) {
        /* Wait for Port F to become ready. */
    }

    GPIO_PORTF_DIR_R |= pins;
    GPIO_PORTF_DEN_R |= pins;
}

/* -------------------------------------------------------------------------- */
/* UART                                                                       */
/* -------------------------------------------------------------------------- */

/** Initialize UART0 on PA0/PA1 for 115200 baud, 8-N-1 communication. */
static void uart_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA)) {
        /* Wait for UART0 and GPIOA to become ready. */
    }

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(
        UART0_BASE,
        SysCtlClockGet(),
        UART_BAUD_RATE,
        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
}

/** Write a null-terminated string to UART0. */
static void uart_write(const char *message)
{
    while (*message != '\0') {
        UARTCharPut(UART0_BASE, *message++);
    }
}

/** Write a string to UART0 followed by a CR/LF line ending. */
static void uart_write_line(const char *message)
{
    uart_write(message);
    uart_write("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Servo / PWM                                                                */
/* -------------------------------------------------------------------------- */

/** Initialize PWM1 output 5 on PF1 for servo positioning. */
static void servo_init(void)
{
    SysCtlPWMClockSet(SYSCTL_PWMDIV_8);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM1);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM1)) {
        /* Wait for PWM1 to become ready. */
    }

    GPIOPinConfigure(GPIO_PF1_M1PWM5);
    GPIOPinTypePWM(GPIO_PORTF_BASE, SERVO_PIN);

    PWMGenConfigure(
        PWM1_BASE,
        PWM_GEN_2,
        PWM_GEN_MODE_UP_DOWN | PWM_GEN_MODE_NO_SYNC);
    PWMGenPeriodSet(PWM1_BASE, PWM_GEN_2, SERVO_PWM_PERIOD_TICKS);
    PWMPulseWidthSet(PWM1_BASE, PWM_OUT_5, SERVO_LOCKED_PULSE_TICKS);
    PWMGenEnable(PWM1_BASE, PWM_GEN_2);
    PWMOutputState(PWM1_BASE, PWM_OUT_5_BIT, true);
}

/* -------------------------------------------------------------------------- */
/* ADC                                                                        */
/* -------------------------------------------------------------------------- */

/** Initialize ADC0 sequence 0 to sample AIN9 on PE4. */
static void adc_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOE) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_ADC0)) {
        /* Wait for GPIOE and ADC0 to become ready. */
    }

    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_4);

    ADCSequenceDisable(ADC0_BASE, 0);
    ADCSequenceConfigure(ADC0_BASE, 0, ADC_TRIGGER_PROCESSOR, 0);
    ADCSequenceStepConfigure(
        ADC0_BASE,
        0,
        0,
        ADC_CTL_CH9 | ADC_CTL_IE | ADC_CTL_END);
    ADCSequenceEnable(ADC0_BASE, 0);
    ADCIntClear(ADC0_BASE, 0);
}

/** Trigger and return a single potentiometer sample from ADC0. */
static uint32_t adc_read_potentiometer(void)
{
    uint32_t sample = 0U;

    ADCProcessorTrigger(ADC0_BASE, 0);
    while (!ADCIntStatus(ADC0_BASE, 0, false)) {
        /* Wait for conversion to complete. */
    }

    ADCSequenceDataGet(ADC0_BASE, 0, &sample);
    ADCIntClear(ADC0_BASE, 0);

    return sample;
}

/* -------------------------------------------------------------------------- */
/* Keypad                                                                     */
/* -------------------------------------------------------------------------- */

/** Initialize a 4x4 matrix keypad using PE0-PE3 rows and PC4-PC7 columns. */
static void keypad_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOE)) {
        /* Wait for keypad GPIO ports to become ready. */
    }

    /* PE0-PE3 drive the keypad rows. */
    GPIO_PORTE_DIR_R |= KEYPAD_ROW_MASK;
    GPIO_PORTE_DEN_R |= KEYPAD_ROW_MASK;

    /* PC4-PC7 read the keypad columns using internal pull-down resistors. */
    GPIO_PORTC_DIR_R &= ~KEYPAD_COLUMN_MASK;
    GPIO_PORTC_DEN_R |= KEYPAD_COLUMN_MASK;
    GPIO_PORTC_PDR_R |= KEYPAD_COLUMN_MASK;
}

/**
 * Scan the 4x4 keypad.
 *
 * @return Numeric key value or ASCII code for A-D/#/*; -1 if no key is pressed.
 */
static int keypad_read(void)
{
    static const int kKeyMap[4][4] = {
        {1,   2,   3,   'A'},
        {4,   5,   6,   'B'},
        {7,   8,   9,   'C'},
        {'#', 0,   '*', 'D'}
    };

    for (uint32_t row = 0U; row < 4U; row++) {
        GPIO_PORTE_DATA_R = (1U << row);

        for (uint32_t col = 0U; col < 4U; col++) {
            const uint32_t columnMask = (1U << (col + 4U));

            if ((GPIO_PORTC_DATA_R & columnMask) != 0U) {
                while ((GPIO_PORTC_DATA_R & KEYPAD_COLUMN_MASK) != 0U) {
                    /* Wait for key release to avoid repeated entries. */
                }

                return kKeyMap[row][col];
            }
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* Lock state                                                                 */
/* -------------------------------------------------------------------------- */

/** Clear the currently entered credential sequence. */
static void reset_user_input(void)
{
    for (uint32_t i = 0U; i < PASSWORD_LENGTH; i++) {
        g_userInput[i] = 0;
    }

    g_inputIndex = 0U;
}

/** Compare the entered keypad sequence against the configured password. */
static bool password_matches(void)
{
    for (uint32_t i = 0U; i < PASSWORD_LENGTH; i++) {
        if (g_userInput[i] != kPassword[i]) {
            return false;
        }
    }

    return true;
}

/** Apply LED and servo outputs for the active lock state. */
static void update_lock_outputs(void)
{
    switch (g_lockState) {
        case LOCK_STATE_LOCKED:
            GPIOPinWrite(GPIO_PORTF_BASE, STATUS_LED_GREEN, 0);
            GPIOPinWrite(GPIO_PORTF_BASE, STATUS_LED_RED, STATUS_LED_RED);
            PWMPulseWidthSet(
                PWM1_BASE, PWM_OUT_5, SERVO_LOCKED_PULSE_TICKS);
            break;

        case LOCK_STATE_UNLOCKED:
            GPIOPinWrite(GPIO_PORTF_BASE, STATUS_LED_RED, 0);
            GPIOPinWrite(
                GPIO_PORTF_BASE, STATUS_LED_GREEN, STATUS_LED_GREEN);
            PWMPulseWidthSet(
                PWM1_BASE, PWM_OUT_5, SERVO_UNLOCKED_PULSE_TICKS);

            SysCtlDelay(
                (SysCtlClockGet() / 3U) * UNLOCK_DURATION_SECONDS);

            g_lockState = LOCK_STATE_LOCKED;
            uart_write_line("Lock re-engaged.");
            break;

        case LOCK_STATE_INTRUDER:
        default:
            GPIOPinWrite(GPIO_PORTF_BASE, STATUS_LED_GREEN, 0);
            GPIOPinWrite(GPIO_PORTF_BASE, STATUS_LED_RED, STATUS_LED_RED);
            PWMPulseWidthSet(
                PWM1_BASE, PWM_OUT_5, SERVO_LOCKED_PULSE_TICKS);
            break;
    }
}

/* -------------------------------------------------------------------------- */
/* Interrupt handlers                                                         */
/* -------------------------------------------------------------------------- */

/** Handle an active-low intrusion event from the IR sensor on PF0. */
void IntruderAlertHandler(void)
{
    GPIOIntClear(GPIO_PORTF_BASE, IR_SENSOR_PIN);

    if (GPIOPinRead(GPIO_PORTF_BASE, IR_SENSOR_PIN) == 0U) {
        g_lockState = LOCK_STATE_INTRUDER;
        uart_write_line("INTRUDER ALERT");
    }
}

/**
 * Reset a partially entered credential after an inactivity interval.
 *
 * The watchdog interrupt is used as an inactivity timer rather than as a
 * system-reset watchdog. A recent keypress suppresses the current timeout;
 * otherwise, a partial entry is cleared.
 */
void WatchdogIntHandler(void)
{
    WatchdogIntClear(WATCHDOG0_BASE);

    if (!g_keypadActivity && g_inputIndex > 0U &&
        g_inputIndex < PASSWORD_LENGTH) {
        reset_user_input();
        uart_write_line("Input timeout. Please try again.");
        g_keypadActivity = true;
    } else {
        g_keypadActivity = false;
    }
}

/** Initialize Watchdog0 as a periodic inactivity timer. */
static void watchdog_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_WDOG0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_WDOG0)) {
        /* Wait for Watchdog0 to become ready. */
    }

    if (WatchdogLockState(WATCHDOG0_BASE)) {
        WatchdogUnlock(WATCHDOG0_BASE);
    }

    WatchdogIntRegister(WATCHDOG0_BASE, WatchdogIntHandler);
    WatchdogIntTypeSet(WATCHDOG0_BASE, WATCHDOG_INT_TYPE_INT);
    WatchdogReloadSet(WATCHDOG0_BASE, WATCHDOG_RELOAD_TICKS);
    WatchdogIntClear(WATCHDOG0_BASE);
    WatchdogIntEnable(WATCHDOG0_BASE);
    WatchdogEnable(WATCHDOG0_BASE);
}
