/* =========================================================
   Digital Function Generator - mbed NXP LPC1768
   =========================================================

   Keypad:
     1 = Sine        2 = Square      3 = Triangle    4 = Sawtooth
     A = Frequency UP      B = Frequency DOWN

   UART0 (USB virtual COM port, 9600 8N1) - type a command + Enter:
     1-4   select waveform          A / B   frequency up / down
     F500  set frequency to 500 Hz  (1 - 1000)
     ?     help + current status

   Amplitude : 10k potentiometer on ADC  (P0.23 = mbed p15)
   Output    : DAC on P0.26              (mbed p18)

   Keypad    : R1-R4 -> P0.4-P0.7  (p30, p29, p8, p7)
               C1-C4 -> P0.8-P0.11 (p6, p5, p28, p27)

   Single-digit 7-segment (shows waveform number 1-4):
               a -> P0.15 (p14)     e -> P0.1  (p10)
               b -> P0.16 (p13)     f -> P0.0  (p9)
               c -> P0.17 (p12)     g -> P0.24 (p16)
               d -> P0.18 (p11)     common -> GND (common cathode)

   4-digit TM1637(shows frequency in Hz):
               CLK -> P2.0 (p26)
               DIO -> P2.1 (p25)
               VCC -> 3.3 V (p40), GND -> GND

   UART0     : P0.2 (TXD0) / P0.3 (RXD0)
   ========================================================= */

#include "LPC17xx.h"
#include <stdint.h>
#include <math.h>

#define LUT_SIZE        256
#define PCLK_HZ         24000000UL      /* CCLK 96 MHz / 4 */

#define R1      (1 << 4)
#define R2      (1 << 5)
#define R3      (1 << 6)
#define R4      (1 << 7)

#define C1      (1 << 8)
#define C2      (1 << 9)
#define C3      (1 << 10)
#define C4      (1 << 11)

#define ROW_MASK    (R1 | R2 | R3 | R4)
#define COL_MASK    (C1 | C2 | C3 | C4)

#define NUM_FREQ    6
#define FREQ_MIN    1
#define FREQ_MAX    1000
#define AMP_MIN     10
#define AMP_MAX     100
#define POT_DEADBAND 100                /* ADC counts (0-4095) the pot must move before amplitude changes */

/* Single-digit 7-segment pins (all on Port 0) */
#define SEG_A       (1u << 15)
#define SEG_B       (1u << 16)
#define SEG_C       (1u << 17)
#define SEG_D       (1u << 18)
#define SEG_E       (1u << 1)
#define SEG_F       (1u << 0)
#define SEG_G       (1u << 24)
#define SEG_ALL     (SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G)

/* TM1637 pins (Port 2) */
#define TM_CLK      (1u << 0)
#define TM_DIO      (1u << 1)

/* UART */
#define UART_BAUD   9600
#define CMD_MAX     16


/* =========================================================
   GLOBALS
   ========================================================= */

volatile uint8_t  waveform   = 0;       /* 0=sine 1=square 2=tri 3=saw */
volatile uint16_t lut_index  = 0;
volatile uint8_t  amplitude  = 100;     /* percent */
volatile uint32_t ms_ticks   = 0;       /* incremented by SysTick */

static uint16_t current_freq = 100;     /* Hz */

/* Frequencies the keypad A / B keys step through */
static const uint16_t freq_list[NUM_FREQ] = {10, 50, 100, 200, 500, 1000};

static const char * const wave_names[4] =
    {"Sine", "Square", "Triangle", "Sawtooth"};

/* Flags set when something changes */
static uint8_t display_dirty = 0;
static uint8_t status_dirty  = 0;
static uint32_t status_stamp = 0;       /* time of the last change */

uint16_t sine_table[LUT_SIZE];
uint16_t square_table[LUT_SIZE];
uint16_t triangle_table[LUT_SIZE];
uint16_t sawtooth_table[LUT_SIZE];

static const char keymap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

/* Digit patterns: bit0 = a, bit1 = b, ... bit6 = g (same for both displays) */
static const uint8_t digit_pattern[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static const uint32_t seg_mask[7] = {
    SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G
};


/* =========================================================
   SYSTICK (1 ms time base)
   ========================================================= */

void SysTick_Handler(void)
{
    ms_ticks++;
}


/* =========================================================
   TIMER 1 - free running 1 MHz counter for microsecond delays
   (used by the TM1637 driver)
   ========================================================= */

void Timer1_Init(void)
{
    LPC_SC->PCONP |= (1 << 2);              /* power Timer 1 */
    LPC_SC->PCLKSEL0 &= ~(3 << 4);          /* PCLK = CCLK / 4 */

    LPC_TIM1->CTCR = 0;
    LPC_TIM1->PR   = (PCLK_HZ / 1000000UL) - 1;   /* 1 tick = 1 us */
    LPC_TIM1->TCR  = 2;                     /* reset */
    LPC_TIM1->TCR  = 1;                     /* start */
}

void delay_us(uint32_t us)
{
    uint32_t start = LPC_TIM1->TC;

    while ((LPC_TIM1->TC - start) < us)
    {
    }
}


/* =========================================================
   LUT INITIALIZATION
   ========================================================= */

void LUT_Init(void)
{
    uint16_t i;
    float angle;

    for (i = 0; i < LUT_SIZE; i++)
    {
        /* SINE */
        angle = (2.0f * 3.14159265f * i) / LUT_SIZE;
        sine_table[i] = (uint16_t)(511.5f + 511.5f * sinf(angle));

        /* SQUARE */
        square_table[i] = (i < 128) ? 1023 : 0;

        /* TRIANGLE */
        if (i < 128)
            triangle_table[i] = (uint16_t)((1023.0f * i) / 128.0f);
        else
            triangle_table[i] = (uint16_t)(1023.0f - (1023.0f * (i - 128)) / 128.0f);

        /* SAWTOOTH */
        sawtooth_table[i] = (uint16_t)((1023.0f * i) / 255.0f);
    }
}


/* =========================================================
   DAC
   ========================================================= */

void DAC_Init(void)
{
    /* P0.26 = AOUT (PINSEL1 bits 21:20 = 10) */
    LPC_PINCON->PINSEL1  &= ~(3 << 20);
    LPC_PINCON->PINSEL1  |=  (2 << 20);

    /* Neither pull-up nor pull-down (PINMODE1 bits 21:20 = 10) */
    LPC_PINCON->PINMODE1 &= ~(3 << 20);
    LPC_PINCON->PINMODE1 |=  (2 << 20);

    LPC_DAC->DACR = 0;
}

static inline void DAC_Write(uint16_t value)
{
    LPC_DAC->DACR = ((uint32_t)value & 0x3FF) << 6;
}


/* =========================================================
   TIMER 0  (sample rate generator)
   f_out = PCLK / ((MR0 + 1) * LUT_SIZE)
   ========================================================= */

void Set_Frequency(uint32_t hz)
{
    uint32_t div = hz * LUT_SIZE;
    uint32_t mr  = (PCLK_HZ + div / 2) / div;   /* rounded to nearest */

    if (mr < 2) mr = 2;

    LPC_TIM0->TC  = 0;          /* avoid missing the match if new MR0 < TC */
    LPC_TIM0->MR0 = mr - 1;
}

void Timer0_Init(void)
{
    LPC_SC->PCONP |= (1 << 1);              /* power Timer 0 */
    LPC_SC->PCLKSEL0 &= ~(3 << 2);          /* PCLK = CCLK / 4 */

    LPC_TIM0->CTCR = 0;                     /* timer mode */
    LPC_TIM0->PR   = 0;                     /* no prescale */

    LPC_TIM0->MCR = (1 << 0) | (1 << 1);    /* interrupt + reset on MR0 */

    Set_Frequency(current_freq);

    NVIC_SetPriority(TIMER0_IRQn, 0);       /* highest: keeps samples jitter-free */
    NVIC_EnableIRQ(TIMER0_IRQn);

    LPC_TIM0->TCR = 2;                      /* reset */
    LPC_TIM0->TCR = 1;                      /* start */
}

void TIMER0_IRQHandler(void)
{
    uint16_t value;
    int32_t  scaled;

    if (LPC_TIM0->IR & 1)
    {
        LPC_TIM0->IR = 1;                   /* clear interrupt,(w1c)writing 1 clears the register */

        switch (waveform)
        {
            case 0:  value = sine_table[lut_index];     break;
            case 1:  value = square_table[lut_index];   break;
            case 2:  value = triangle_table[lut_index]; break;
            default: value = sawtooth_table[lut_index]; break;
        }

        /* Amplitude scaling around mid-scale (512) */
        scaled = 512 + (((int32_t)value - 512) * (int32_t)amplitude) / 100;

        if (scaled < 0)    scaled = 0;
        if (scaled > 1023) scaled = 1023;

        DAC_Write((uint16_t)scaled);

        lut_index++;
        if (lut_index >= LUT_SIZE)
            lut_index = 0;
    }
}


/* =========================================================
   SINGLE-DIGIT 7-SEGMENT  (shows waveform number 1-4)
   ========================================================= */

void Seg_Init(void)
{
    /* P0.0, P0.1, P0.15 = GPIO */
    LPC_PINCON->PINSEL0 &= ~((3u << 0) | (3u << 2) | (3u << 30));

    /* P0.16, P0.17, P0.18, P0.24 = GPIO */
    LPC_PINCON->PINSEL1 &= ~((3u << 0) | (3u << 2) | (3u << 4) | (3u << 16));

    LPC_GPIO0->FIODIR |= SEG_ALL;
}

void Seg_Show(uint8_t digit)
{
    uint32_t on = 0;
    uint8_t  i;

    if (digit > 9) digit = 0;

    for (i = 0; i < 7; i++)
    {
        if (digit_pattern[digit] & (1 << i))
            on |= seg_mask[i];
    }

    /* common cathode: segment is ON when its pin is HIGH */
    LPC_GPIO0->FIOCLR = SEG_ALL & ~on;
    LPC_GPIO0->FIOSET = on;

}


/* =========================================================
   TM1637 4-DIGIT DISPLAY (HW-069)  - bit-banged, 2 wires
   ========================================================= */

#define TM_DELAY()      delay_us(50)    /* slow, reliable bus clock (~10 kHz) */

static void TM_Start(void)
{
    LPC_GPIO2->FIOSET = TM_CLK | TM_DIO;
    TM_DELAY();
    LPC_GPIO2->FIOCLR = TM_DIO;
    TM_DELAY();
    LPC_GPIO2->FIOCLR = TM_CLK;
}

static void TM_Stop(void)
{
    LPC_GPIO2->FIOCLR = TM_CLK;
    LPC_GPIO2->FIOCLR = TM_DIO;
    TM_DELAY();
    LPC_GPIO2->FIOSET = TM_CLK;
    TM_DELAY();
    LPC_GPIO2->FIOSET = TM_DIO;
    TM_DELAY();
}

static void TM_WriteByte(uint8_t b)
{
    uint8_t i;

    for (i = 0; i < 8; i++)                 /* LSB first */
    {
        LPC_GPIO2->FIOCLR = TM_CLK;

        if (b & 0x01)
            LPC_GPIO2->FIOSET = TM_DIO;
        else
            LPC_GPIO2->FIOCLR = TM_DIO;

        TM_DELAY();
        LPC_GPIO2->FIOSET = TM_CLK;
        TM_DELAY();

        b >>= 1;
    }

    /* Acknowledge clock: release DIO so the TM1637 can pull it low */
    LPC_GPIO2->FIOCLR = TM_CLK;
    LPC_GPIO2->FIODIR &= ~TM_DIO;           /* DIO = input */
    TM_DELAY();
    LPC_GPIO2->FIOSET = TM_CLK;
    TM_DELAY();
    LPC_GPIO2->FIOCLR = TM_CLK;
    LPC_GPIO2->FIOCLR = TM_DIO;
    LPC_GPIO2->FIODIR |= TM_DIO;            /* DIO = output (low) */
    TM_DELAY();
}

void TM1637_Init(void)
{
    /* P2.0, P2.1 = GPIO outputs */
    LPC_PINCON->PINSEL4 &= ~0x0000000F;
    LPC_GPIO2->FIODIR   |= (TM_CLK | TM_DIO);
    LPC_GPIO2->FIOSET    = (TM_CLK | TM_DIO);
}

/* Send 4 raw segment patterns (leftmost digit first) */
static void TM1637_Write4(const uint8_t *seg, uint8_t brightness)
{
    uint8_t i;

    TM_Start();
    TM_WriteByte(0x40);                     /* auto-increment address mode */
    TM_Stop();

    TM_Start();
    TM_WriteByte(0xC0);                     /* start at first digit */
    for (i = 0; i < 4; i++)
        TM_WriteByte(seg[i]);
    TM_Stop();

    TM_Start();
    TM_WriteByte(0x88 | (brightness & 0x07));   /* display ON + brightness */
    TM_Stop();
}

/* Show a number 0-9999, right aligned, no leading zeros */
void TM1637_ShowNumber(uint16_t n)
{
    uint8_t seg[4];

    if (n > 9999) n = 9999;

    seg[0] = (n >= 1000) ? digit_pattern[(n / 1000) % 10] : 0x00;
    seg[1] = (n >= 100)  ? digit_pattern[(n / 100)  % 10] : 0x00;
    seg[2] = (n >= 10)   ? digit_pattern[(n / 10)   % 10] : 0x00;
    seg[3] = digit_pattern[n % 10];

    TM1637_Write4(seg, 7);                  /* max brightness */
}


/* =========================================================
   DISPLAY UPDATE
   ========================================================= */

void Display_Update(void)
{
    uint8_t w = waveform;

    if (w > 3) w = 3;

    Seg_Show(w + 1);                        /* 1=sine 2=square 3=tri 4=saw */
    TM1637_ShowNumber(current_freq);
}


/* =========================================================
   UART0  (P0.2 = TXD0, P0.3 = RXD0 -> mbed USB virtual COM port)
   ========================================================= */

void UART0_Init(void)
{
    uint32_t divisor = PCLK_HZ / (16UL * UART_BAUD);    /* 156 for 9600 */

    LPC_SC->PCONP |= (1 << 3);              /* power UART0 */
    LPC_SC->PCLKSEL0 &= ~(3 << 6);          /* PCLK = CCLK / 4 */

    /* P0.2 = TXD0, P0.3 = RXD0 */
    LPC_PINCON->PINSEL0 &= ~((3u << 4) | (3u << 6));
    LPC_PINCON->PINSEL0 |=  ((1u << 4) | (1u << 6));

    LPC_UART0->LCR = 0x83;                  /* 8N1, DLAB = 1 */
    LPC_UART0->DLL = divisor & 0xFF;
    LPC_UART0->DLM = (divisor >> 8) & 0xFF;
    LPC_UART0->LCR = 0x03;                  /* DLAB = 0 */
    LPC_UART0->FCR = 0x07;                  /* enable and clear FIFOs */
}

void UART_PutChar(char c)
{
    while (!(LPC_UART0->LSR & (1 << 5)))    /* wait for THR empty */
    {
    }

    LPC_UART0->THR = (uint8_t)c;
}

void UART_PutString(const char *s)
{
    while (*s)
        UART_PutChar(*s++);
}

void UART_PutUint(uint32_t value)
{
    char tmp[10];
    uint8_t n = 0;

    if (value == 0)
        tmp[n++] = '0';

    while (value > 0 && n < 10)
    {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (n > 0)
        UART_PutChar(tmp[--n]);
}

/* Returns the received character, or -1 if none is waiting */
int UART_GetChar(void)
{
    if (LPC_UART0->LSR & 1)                 /* receive data ready */
        return LPC_UART0->RBR;

    return -1;
}


/* =========================================================
   CONTROL ACTIONS (shared by keypad and UART)
   ========================================================= */

static void Apply_Waveform(uint8_t w)
{
    if (w > 3) return;

    waveform  = w;
    lut_index = 0;

    display_dirty = 1;
    status_dirty  = 1;
    status_stamp  = ms_ticks;
}

static void Apply_Frequency(uint16_t hz)
{
    if (hz < FREQ_MIN || hz > FREQ_MAX) return;

    current_freq = hz;
    Set_Frequency(hz);

    display_dirty = 1;
    status_dirty  = 1;
    status_stamp  = ms_ticks;
}

/* Step to the next higher (+1) or lower (-1) frequency in freq_list */
static void Freq_Step(int8_t dir)
{
    uint8_t  i;
    uint16_t target = current_freq;

    if (dir > 0)
    {
        for (i = 0; i < NUM_FREQ; i++)
        {
            if (freq_list[i] > current_freq)
            {
                target = freq_list[i];
                break;
            }
        }
    }
    else
    {
        for (i = NUM_FREQ; i > 0; i--)
        {
            if (freq_list[i - 1] < current_freq)
            {
                target = freq_list[i - 1];
                break;
            }
        }
    }

    if (target != current_freq)
        Apply_Frequency(target);
}


/* =========================================================
   UART STATUS + COMMANDS
   ========================================================= */

static void Print_Status(void)
{
    uint8_t w = waveform;

    if (w > 3) w = 3;

    UART_PutString("Wave: ");
    UART_PutString(wave_names[w]);
    UART_PutString(" | Freq: ");
    UART_PutUint(current_freq);
    UART_PutString(" Hz | Amp: ");
    UART_PutUint(amplitude);
    UART_PutString("%\r\n");
}

static void Print_Help(void)
{
    UART_PutString("Commands (type, then press Enter):\r\n");
    UART_PutString("  1-4    select waveform (sine, square, triangle, sawtooth)\r\n");
    UART_PutString("  A / B  frequency up / down\r\n");
    UART_PutString("  F500   set frequency in Hz (1-1000)\r\n");
    UART_PutString("  ?      this help + current status\r\n");
}

/* Parse an unsigned number after optional spaces, '=' or ':' */
static int Parse_Number(const char *s, uint32_t *out)
{
    uint32_t n = 0;

    while (*s == ' ' || *s == '=' || *s == ':')
        s++;

    if (*s < '0' || *s > '9')
        return 0;

    while (*s >= '0' && *s <= '9')
    {
        n = n * 10 + (uint32_t)(*s - '0');
        if (n > 100000UL) n = 100000UL;
        s++;
    }

    *out = n;
    return 1;
}

static void Process_Command(const char *s)
{
    uint32_t n;
    char c;

    while (*s == ' ')
        s++;

    c = *s;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);                 /* upper case */

    switch (c)
    {
        case '1': case '2': case '3': case '4':
            Apply_Waveform((uint8_t)(c - '1'));
            break;

        case 'A': case '+':
            Freq_Step(1);
            break;

        case 'B': case '-':
            Freq_Step(-1);
            break;

        case 'F':
            if (Parse_Number(s + 1, &n) && n >= FREQ_MIN && n <= FREQ_MAX)
                Apply_Frequency((uint16_t)n);
            else
                UART_PutString("Error: frequency must be 1-1000 Hz (e.g. F500)\r\n");
            break;

        case '?': case 'H':
            Print_Help();
            Print_Status();
            break;

        default:
            UART_PutString("Unknown command. Type ? for help.\r\n");
            break;
    }
}

/* Read waiting characters, echo them, run a command on Enter */
static void UART_Poll(void)
{
    static char    line[CMD_MAX];
    static uint8_t len = 0;
    int c;

    while ((c = UART_GetChar()) >= 0)
    {
        if (c == '\r' || c == '\n')
        {
            if (len > 0)
            {
                line[len] = '\0';
                UART_PutString("\r\n");
                Process_Command(line);
                len = 0;
            }
        }
        else if (c == 0x08 || c == 0x7F)            /* backspace */
        {
            if (len > 0)
            {
                len--;
                UART_PutString("\b \b");
            }
        }
        else if (c >= 0x20 && c < 0x7F && len < CMD_MAX - 1)
        {
            line[len++] = (char)c;
            UART_PutChar((char)c);                  /* echo */
        }
    }
}


/* =========================================================
   KEYPAD
   ========================================================= */

static void Keypad_Settle(void)
{
    volatile uint32_t i;
    for (i = 0; i < 200; i++) { __NOP(); }  /* roughly 10 us */
}

void Keypad_Init(void)
{
    /* P0.4 - P0.11 as GPIO */
    LPC_PINCON->PINSEL0  &= ~0x00FFFF00;

    /* Pull-ups enabled (00) on all 8 pins */
    LPC_PINCON->PINMODE0 &= ~0x00FFFF00;

    /* Rows and columns all inputs to start */
    LPC_GPIO0->FIODIR &= ~(ROW_MASK | COL_MASK);
}

/* Raw scan: returns the key currently pressed, or 0 */
char Keypad_GetKey(void)
{
    uint8_t row, col, cols;

    for (row = 0; row < 4; row++)
    {
        /* Only the active row is an output, driven LOW */
        LPC_GPIO0->FIODIR = (LPC_GPIO0->FIODIR & ~ROW_MASK) | (1 << (4 + row));  /*selects one row to become the output*/
        LPC_GPIO0->FIOCLR = (1 << (4 + row));

        Keypad_Settle();

        cols = (LPC_GPIO0->FIOPIN >> 8) & 0x0F;

        if (cols != 0x0F)
        {
            for (col = 0; col < 4; col++)
            {
                if (!(cols & (1 << col)))
                {
                    LPC_GPIO0->FIODIR &= ~ROW_MASK;     /* release rows */
                    return keymap[row][col];
                }
            }
        }
    }

    LPC_GPIO0->FIODIR &= ~ROW_MASK;                     /* release rows */
    return 0;
}

/* Debounced, returns each key press ONCE. Call every ~5 ms. */
char Keypad_Read(void)
{
    static char    last     = 0;
    static uint8_t stable   = 0;
    static uint8_t reported = 0;

    char k = Keypad_GetKey();

    if (k == last)
    {
        if (stable < 255) stable++;
    }
    else
    {
        last     = k;
        stable   = 0;
        reported = 0;
    }

    if (k && stable >= 3 && !reported)      /* ~15-20 ms stable */
    {
        reported = 1;
        return k;
    }

    return 0;
}


/* =========================================================
   ADC (potentiometer on P0.23 = AD0.0 = mbed p15)
   ========================================================= */

void ADC_Init(void)
{
    LPC_SC->PCONP |= (1 << 12);                 /* power ADC */

    LPC_PINCON->PINSEL1  &= ~(3 << 14);
    LPC_PINCON->PINSEL1  |=  (1 << 14);         /* P0.23 = AD0.0 */
    LPC_PINCON->PINMODE1 &= ~(3 << 14);
    LPC_PINCON->PINMODE1 |=  (2 << 14);         /* no pull-up/down */

    /* channel 0, CLKDIV = 1 (PCLK 24/2 = 12 MHz, max 13), ADC on */
    LPC_ADC->ADCR = (1 << 0) | (1 << 8) | (1 << 21);
}

uint16_t ADC_Read(void)
{
    LPC_ADC->ADCR |= (1 << 24);                 /* start conversion now */
    while (!(LPC_ADC->ADGDR & (1u << 31))) { }  /* wait for DONE */
    LPC_ADC->ADCR &= ~(7 << 24);                /* clear start bits */
    return (LPC_ADC->ADGDR >> 4) & 0xFFF;       /* 12-bit result */
}

static uint8_t Pot_ToAmplitude(uint16_t adc)
{
    return (uint8_t)(AMP_MIN + ((uint32_t)adc * (AMP_MAX - AMP_MIN)) / 4095);
}


/* =========================================================
   MAIN
   ========================================================= */

int main(void)
{
    char     key;
    uint32_t last_scan   = 0;
    uint16_t pot_filt;
    uint16_t pot_last;

    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000);     /* 1 ms tick */

    Timer1_Init();                              /* microsecond delays */

    LUT_Init();
    DAC_Init();
    Keypad_Init();
    ADC_Init();
    Seg_Init();
    TM1637_Init();
    UART0_Init();

    pot_filt  = ADC_Read();                     /* start at the real pot position */
    pot_last  = pot_filt;
    amplitude = Pot_ToAmplitude(pot_last);

    Timer0_Init();                              /* wave output starts here */

    Display_Update();                           /* show initial waveform + frequency */

    UART_PutString("\r\nDigital Function Generator ready. Type ? for help.\r\n");
    Print_Status();

    while (1)
    {
        /* Handle any commands typed on the PC (checked on every pass) */
        UART_Poll();

        /* Refresh the displays only when something changed */
        if (display_dirty)
        {
            display_dirty = 0;
            Display_Update();
        }

        /* Send ONE status line once changes have settled (300 ms quiet) */
        if (status_dirty && (ms_ticks - status_stamp) >= 300)
        {
            status_dirty = 0;
            Print_Status();
        }

        /* Run the pot + keypad scan every 5 ms */
        if ((ms_ticks - last_scan) < 5)
            continue;

        last_scan = ms_ticks;

        /* Potentiometer -> amplitude (smoothed, with a deadband so
           ADC flicker does not make the wave "breathe") */
        pot_filt = (uint16_t)((pot_filt * 7 + ADC_Read()) / 8);

        if ((pot_filt > pot_last + POT_DEADBAND) || (pot_filt + POT_DEADBAND < pot_last))
        {
            pot_last     = pot_filt;
            amplitude    = Pot_ToAmplitude(pot_last);
            status_dirty = 1;
            status_stamp = ms_ticks;
        }

        /* Keypad */
        key = Keypad_Read();

        switch (key)
        {
            /* Waveform select */
            case '1': Apply_Waveform(0); break;
            case '2': Apply_Waveform(1); break;
            case '3': Apply_Waveform(2); break;
            case '4': Apply_Waveform(3); break;

            /* Frequency up / down */
            case 'A': Freq_Step(1);  break;
            case 'B': Freq_Step(-1); break;

            default:
                break;
        }
    }
}
