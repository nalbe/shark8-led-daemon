/*
 * aw2033.h - userspace controller for the AW2033 3-channel LED driver.
 *
 * Drives the chip directly through the reg register map exported by the
 * out-of-tree aw2033_led kernel node (/sys/class/leds/<c>/reg). Every
 * feature of the datasheet is reachable here: manual PWM, per-channel
 * current (CUR) and global Imax, pattern breathing with T0..T4 timing,
 * REPEAT, exponential/linear ramp, PWM frequency, fade in/out, sync mode
 * and the recommended synchronized channel start procedure.
 *
 * The controller talks to the chip through ONE sysfs node (any of the
 * three leds; they are aliases to the same i2c device). It never uses
 * brightness/led_time/blink sysfs nodes on purpose - those force
 * manual/pattern modes implicitly and cause the channel state to drift
 * (the green-stuck-in-manual bug).
 */

#ifndef AW2033_H
#define AW2033_H

#include <stdint.h>

/* ---- register map (datasheet V1.4) ---- */
#define AW_REG_RSTR   0x00  /* read = chip id 0x09; write 0x55 = soft reset */
#define AW_REG_GCR1   0x01
#define AW_REG_ISR    0x02  /* read-only, cleared on read */
#define AW_REG_PATST  0x03  /* read-only: ST2|ST1|ST0 pattern running */
#define AW_REG_GCR2   0x04
#define AW_REG_LCTR   0x30
#define AW_REG_LCFG0  0x31
#define AW_REG_LCFG1  0x32
#define AW_REG_LCFG2  0x33
#define AW_REG_PWM0   0x34
#define AW_REG_PWM1   0x35
#define AW_REG_PWM2   0x36
#define AW_REG_LED0T0 0x37  /* T1[7:4] T2[3:0] */
#define AW_REG_LED0T1 0x38  /* T3[7:4] T4[3:0] */
#define AW_REG_LED0T2 0x39  /* T0[7:4] REPEAT[3:0] */
#define AW_REG_LED1T0 0x3A
#define AW_REG_LED1T1 0x3B
#define AW_REG_LED1T2 0x3C
#define AW_REG_LED2T0 0x3D
#define AW_REG_LED2T1 0x3E
#define AW_REG_LED2T2 0x3F

/* GCR1 bit fields */
#define AW_CHGDIS     0x02  /* disable hardware auto charge indication */
#define AW_CHIPEN     0x01  /* device operating enable */
#define AW_CHARGEDIS_CHIPEN (AW_CHGDIS | AW_CHIPEN)

/* GCR2 bit fields */
#define AW_GCR2_IMAX_MASK 0x03
#define AW_IMAX_15MA  0x00
#define AW_IMAX_30MA  0x01
#define AW_IMAX_5MA   0x02
#define AW_IMAX_10MA  0x03

/* LCFGx bit fields */
#define AW_LCFG_SYNC  0x80  /* LCFG0 only */
#define AW_LCFG_FO    0x40  /* fade-out, manual mode only */
#define AW_LCFG_FI    0x20  /* fade-in, manual mode only */
#define AW_LCFG_MD    0x10  /* 0 = manual, 1 = pattern */
#define AW_LCFG_CUR_MASK 0x0F

/* LCTR bit fields */
#define AW_LCTR_FREQ  0x20  /* 1 = 125Hz, 0 = 250Hz */
#define AW_LCTR_EXP   0x08  /* 1 = linear, 0 = exponential */
#define AW_LCTR_LE2   0x04
#define AW_LCTR_LE1   0x02
#define AW_LCTR_LE0   0x01
#define AW_LCTR_LE_ALL (AW_LCTR_LE2 | AW_LCTR_LE1 | AW_LCTR_LE0)

/* pattern timing codepoints (datasheet: 0000..1111) in milliseconds.
 * Four-bit fields select one of these discrete times.  NOT linear. */
#define AW_TIME_CODES 16
extern const unsigned aw_time_ms[AW_TIME_CODES];

#define AW_MAX_DELAY_MS  10000

/* ---- opaque handle ---- */
typedef struct aw_chip aw_chip;

/* ---- lifecycle / low level ---- */
aw_chip *aw_open(const char *led_name);
void      aw_close(aw_chip *c);
int       aw_probe(aw_chip *c);            /* read chip id (0x09), reset state */
int       aw_rst(aw_chip *c);              /* soft reset (0x55), 5ms wait */
int       aw_pwr(aw_chip *c, int imax);    /* GCR1: CHIPEN+CHGDIS on; GCR2: imax */
int       aw_reg_get(aw_chip *c, uint8_t addr, uint8_t *val);
int       aw_reg_set(aw_chip *c, uint8_t addr, uint8_t val);
void      aw_dump(aw_chip *c);             /* hexdump every readable reg */
int       aw_patst(aw_chip *c, uint8_t *st);

/* ---- helpers ---- */
int       aw_ms_to_code(long ms);          /* nearest discrete time code */
long      aw_code_to_ms(int code);

/* ---- global controls ---- */
int aw_lein(aw_chip *c, int le);           /* set LCTR LE bits (keep FREQ/EXP) */
int aw_lctr(aw_chip *c, int freq125, int linear);  /* FREQ + EXP ramp shape */
int aw_cur(aw_chip *c, int c0, int c1, int c2);     /* per-channel current 0..15 */
int aw_pwm(aw_chip *c, int p0, int p1, int p2);     /* manual duty 0..255 */

/* ---- mode ----
 * Every mode takes the per-channel current levels 0..15 explicitly.
 * CUR is what the chip actually scales: PWMx (or the pattern controller)
 * picks the duty, CUR nibble picks the sink current level under it.  The
 * mode helpers write LCFGx.CUR themselves, so a separate aw_cur() call
 * before them would be clobbered - do NOT do that. */
int aw_all_off(aw_chip *c);
int aw_solid(aw_chip *c, int r, int g, int b, int cur_r, int cur_g, int cur_b);
                                        /* manual, MD=0, PWM=rgb, LCFG=CUR */
int aw_breathe(aw_chip *c, int r, int g, int b,
               long rise_ms, long hold_ms, long fall_ms, long off_ms,
               long t0_ms, int repeat, int sync3,
               int cur_r, int cur_g, int cur_b);
int aw_breathe_ex(aw_chip *c, int r, int g, int b,
                  const long rise_ms[3], const long hold_ms[3],
                  const long fall_ms[3], const long off_ms[3],
                  const long t0_ms[3], int repeat, int sync3,
                  const int cur[3]);
int aw_fade(aw_chip *c, int r, int g, int b,
            long in_ms, long out_ms, int cur_r, int cur_g, int cur_b);
                                        /* manual + FI/FO smooth dim */
int aw_sync_mode(aw_chip *c, int on);        /* LCFG0.SYNC master control */

/* ---- live state readback (what the chip is ACTUALLY doing) ----
 * The /sys/class/leds/<c>/brightness nodes are framework stubs (always
 * whatever Android last set, often stale 255) - they do NOT reflect the
 * chip.  This reads the reg file itself:
 *   chip_on  - GCR1.CHIPEN, 0 = hard off
 *   le       - LCTR channel enables (bit0..2 = LED0..2)
 *   md       - per-channel mode: 0 manual, 1 pattern
 *   cie      - per-channel CUR level 0..15
 *   pwm      - per-channel manual duty 0..255 (manual mode = real output;
 *              in pattern mode holds the pattern amplitude peak)
 *   patst    - PATST pattern-running status bits (bit0..2)
 *   pat_t0   - per-channel T0 phase amount from LEDxT2[7:4] (breath)
 */
typedef struct aw_live {
    int   chip_on;
    int   le;              /* LCTR LE bits 0..7 */
    int   md[3];           /* manual(0) / pattern(1) */
    int   cie[3];          /* current level 0..15 */
    int   pwm[3];          /* manual duty / pattern peak 0..255 */
    int   patst;           /* 0..7 */
    int   pat_t0[3];       /* phase offset code 0..15 */
} aw_live;
int aw_live_get(aw_chip *c, aw_live *st);

/* ---- diagnostics exported for CLI ---- */
extern const char *g_reg_path;   /* full path of chosen reg file */

#endif