#include "dimmer.h"
#include "main.h"
#include "stm32g0xx_hal.h"

/* ============================================================== timing ====
 * TIM3 ticks every 50 us. At 50 Hz one half-cycle is 10 ms = 200 ticks.
 *
 * The firing schedule is NOT rebuilt from every zero-cross edge. It runs on
 * its own half-cycle grid driven by TIM3, and a zero-cross only re-locks that
 * grid. That matters on this board: the H11AA1 sits on the same mains as two
 * triacs, and the burst when either triac fires can add an edge to PC7. With
 * the old code every such edge cancelled the pending gate pulse of the other
 * channel, so raising the iron duty switched the heater off. Now a bogus edge
 * can at worst shift one half-cycle by a few hundred microseconds; it can no
 * longer stop a channel from firing.
 */
#define HALF_PERIOD_TICKS     200UL

/* How far from the expected crossing a real zero-cross may arrive.
 * The H11AA1 output goes HIGH a little before the true zero (the mains has to
 * fall below the opto LED threshold), and the TIM3 grid is accurate to a
 * tick, so 8 ticks = 400 us is plenty. An edge outside this window is the
 * burst made when a triac fires: re-locking to it would shift every following
 * half-cycle by the firing delay, so it is ignored and the free-running grid
 * carries on. The window is wide enough for a 50 Hz +-5 % mains. */
#define ZC_WINDOW_TICKS       8UL

/* No zero-cross for 25 ms (2.5 mains periods) -> the mains is gone. Without a
 * phase reference a phase-angle dimmer must not fire at all. */
#define ZC_TIMEOUT_TICKS      500UL

/* No zero-cross for 100 ms -> tell the UI ("NO AC"). */
#define ZC_LOST_TICKS         2000UL

/* CH1 (heater) full-on threshold.
 * Above this percentage the gate pin is held HIGH continuously instead of
 * using phase-angle pulses.  A short gate pulse at very high duty (delay < 5
 * ticks) can end before the TRIAC latches reliably, so the measured voltage
 * drops paradoxically.  Holding the gate HIGH permanently is safe: the TRIAC
 * still extinguishes at each natural zero-cross and re-fires immediately on
 * the next half-cycle, giving true full conduction. */
#define HEATER_FULLON_PCT     80u

/* CH2 (iron) full-on threshold.
 * Same logic as heater, but iron works fine up to 94%, so threshold is 95. */
#define IRON_FULLON_PCT       95u

/* Below this duty CH1 is switched by whole half-cycles ("burst firing")
 * instead of phase-angle. At 2 % the phase-angle delay is 192 of 200 ticks, so
 * the conduction window is only 400 us and a one-tick jitter of the zero-cross
 * reference changes the delivered power by ~20 %. That is what made a constant
 * 2.00 % request wander over a 30 C band. A burst half-cycle is either fully on
 * or fully off, so its energy is exact; 100 half-cycles per second against a
 * ~20 s thermal time constant averages the bursts into a smooth temperature.
 * Above this duty the conduction window is wide enough for phase-angle. */
#define HEATER_BURST_MAX_DQ   2000u   /* 20.00 % */

/* Gate is held this long at the start of a burst half-cycle. The triac latches
 * in microseconds; 1 ms is plenty and stops well before the next zero-cross,
 * so the gate can never spill over and fire an extra half-cycle. */
#define BURST_GATE_TICKS      20u

/* Gate pulse width for phase-control range (0 .. HEATER_FULLON_PCT-1).
 * MOC3021 needs >=10 us; 3 ticks = 150 us is reliable and short enough not
 * to overlap the next ZC event. */
#define GATE_PULSE_TICKS      3u

/* ----------------------------------------------------------------- state -- */
static volatile uint32_t dim_tick     = 0;  /* free running 50 us counter     */
static volatile uint32_t half_start   = 0;  /* tick of the current half-cycle */
static volatile uint32_t last_zc_tick = 0;  /* last accepted zero-cross       */
static volatile uint8_t  synced       = 0;  /* 1 = mains phase is known       */

static volatile uint8_t  iron_pct     = 0;

/* CH1 duty in 0.01 % units (0..10000). Keeping it finer than one percent is
 * what lets the PID settle on a temperature: on this plant one percent of
 * duty is worth tens of degrees, so a 1 % grid forces a limit cycle of that
 * amplitude no matter how the gains are chosen. */
static volatile uint16_t heater_dq    = 0;
/* fractional part of the firing delay, carried from one half-cycle to the
 * next (ISR only) */
static uint16_t          heat_dither  = 0;
/* burst-firing accumulator: how much of a half-cycle is "owed" (ISR only) */
static uint16_t          heat_burst_acc = 0;

static volatile uint32_t fire1        = 0, fire2    = 0;
static volatile uint8_t  armed1       = 0, armed2   = 0;
static volatile uint32_t gate1_off    = 0, gate2_off = 0;

/* 1 while CH1/CH2 gates are held HIGH in full-on mode */
static volatile uint8_t  ch1_fullon   = 0;
static volatile uint8_t  ch2_fullon   = 0;

/* --------------------------------------------------------- RMS delay table -
 * Maps 0..100 % request to firing-angle delay in 50 us ticks.
 * Inverse of the phase-control RMS equation:
 *   (Vout/Vin)^2 = (pi - a + sin(2a)/2) / pi
 *
 *   0 %  -> 200 ticks  (full half-period, no conduction -> 0 V)
 *   5 %  -> 185 ticks  (~9.25 ms delay  -> ~11 V RMS)
 *  50 %  -> 126 ticks  (~6.3 ms delay   -> ~110 V RMS)
 *  79 %  ->  47 ticks  (last phase-ctrl entry before full-on)
 * >=80 %  -> full-on (gate held HIGH, see HEATER_FULLON_PCT)
 */
static const uint8_t rms_delay_ticks[101] = {
    200, 195, 192, 190, 187, 185, 184, 182, 180, 178,
    177, 175, 174, 172, 171, 169, 168, 167, 165, 164,
    163, 161, 160, 159, 158, 156, 155, 154, 153, 151,
    150, 149, 148, 147, 145, 144, 143, 142, 141, 139,
    138, 137, 136, 135, 134, 132, 131, 130, 129, 128,
    126, 125, 124, 123, 122, 120, 119, 118, 117, 115,
    114, 113, 112, 110, 109, 108, 106, 105, 104, 102,
    101, 100,  98,  97,  95,  94,  92,  91,  89,  87,
     86,  84,  82,  81,  79,  77,  75,  73,  71,  68,
     66,  63,  61,  58,  55,  51,  47,  43,  37,  29,
      0
};

/* Returns firing delay in ticks (used only for phase-control range). */
static uint32_t RmsDelayTicks(uint8_t percent) {
    if (percent >= 100) return 0;
    return (uint32_t)rms_delay_ticks[percent];
}

/* Fine-duty version of RmsDelayTicks() for CH1.
 *
 * The table only has an entry per whole percent, and one percent of duty is
 * worth tens of degrees here, so using it directly leaves the PID with a
 * coarse quantised actuator. Instead the delay is linearly interpolated
 * between the two neighbouring entries and the leftover fraction of a tick is
 * carried in a delta-sigma accumulator: a request of, say, 1.37 % fires at
 * 193 ticks on 63 % of the half-cycles and at 194 on the other 37 %. The
 * average firing angle - and therefore the average power - then follows the
 * request continuously, while each individual half-cycle is still a normal
 * phase-angle burst. 100 half-cycles per second is far faster than the
 * thermal time constant, so the modulation is invisible on the tip.
 *
 * The delay is limited to HALF_PERIOD_TICKS-1 so a gate pulse can never spill
 * into the next half-cycle and fire the triac at full conduction. */
static uint32_t HeaterDelayTicks(void) {
    uint16_t dq = heater_dq;
    if (dq == 0) return HALF_PERIOD_TICKS;

    uint32_t pct = dq / 100u;
    uint32_t rem = dq % 100u;
    uint32_t d0  = rms_delay_ticks[pct];
    uint32_t d1  = rms_delay_ticks[pct + 1u];

    /* delay in 1/100 tick, then split into whole ticks and a remainder */
    uint32_t q     = d0 * 100u - (d0 - d1) * rem;
    uint32_t whole = q / 100u;
    uint32_t frac  = q % 100u;

    heat_dither += (uint16_t)frac;
    if (heat_dither >= 100u) {
        heat_dither -= 100u;
        whole++;
    }
    if (whole >= HALF_PERIOD_TICKS) whole = HALF_PERIOD_TICKS - 1u;
    return whole;
}

/* End of a gate pulse, clipped so it stops at the half-cycle boundary. */
static uint32_t gate_end(uint32_t now) {
    uint32_t end = now + GATE_PULSE_TICKS;
    uint32_t limit = half_start + HALF_PERIOD_TICKS;
    if (end > limit) end = limit;
    return end;
}

/* --------------------------------------------------------- half scheduling -
 * Arm both channels for the half-cycle that starts at dim_tick.
 * Called from the timer ISR and from the zero-cross ISR, which share one
 * NVIC priority, so the two can never nest. */
static void schedule_half(void) {
    /* The zero-cross handler and the tick ISR can both reach here for the same
     * crossing. Scheduling twice would advance the fine-duty dither twice and
     * bias the average power, so remember which half-cycle was already set up. */
    static uint32_t sched_start = 0;
    static uint8_t  sched_valid = 0;
    if (sched_valid && sched_start == half_start) return;
    sched_start = half_start;
    sched_valid = 1;

    /* ---- CH2 (iron) ---- */
    if (ch2_fullon) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
        armed2    = 0;
        gate2_off = 0;
        if (iron_pct) {
            uint32_t delay = RmsDelayTicks(iron_pct);
            if (delay == 0) {
                HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
                gate2_off = dim_tick + GATE_PULSE_TICKS;
            } else {
                fire2  = dim_tick + delay;
                armed2 = 1;
            }
        }
    }

    /* ---- CH1 (heater) ---- */
    if (ch1_fullon) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
        armed1    = 0;
        gate1_off = 0;
        if (heater_dq) {
            if (heater_dq < HEATER_BURST_MAX_DQ) {
                /* burst mode: fire whole half-cycles at the zero-cross */
                heat_burst_acc += heater_dq;
                if (heat_burst_acc >= 10000u) {
                    heat_burst_acc -= 10000u;
                    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
                    gate1_off = dim_tick + BURST_GATE_TICKS;
                }
            } else {
                uint32_t delay = HeaterDelayTicks();
                if (delay == 0) {
                    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
                    gate1_off = gate_end(dim_tick);
                } else {
                    fire1  = dim_tick + delay;
                    armed1 = 1;
                }
            }
        }
    }
}

/* ================================================================== API === */

void Dimmer_Init(void) {
    HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
    dim_tick     = 0;
    half_start   = 0;
    last_zc_tick = 0;
    synced       = 0;
    heater_dq    = 0;
    iron_pct     = 0;
    heat_dither  = 0;
    fire1 = fire2 = 0;
    armed1 = armed2 = 0;
    gate1_off = gate2_off = 0;
    ch1_fullon = 0;
    ch2_fullon = 0;
}

void Dimmer_SetHeater(uint8_t percent) {
    if (percent > 100) percent = 100;
    Dimmer_SetHeaterFine((float)percent);
}

void Dimmer_SetHeaterFine(float percent) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    uint16_t dq = (uint16_t)(percent * 100.0f + 0.5f);
    if (dq > 10000u) dq = 10000u;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    heater_dq = dq;
    if (dq == 0) heat_burst_acc = 0;

    /* Enter or leave full-on mode immediately so the gate state is correct
     * even between zero-cross events. */
    if (dq >= HEATER_FULLON_PCT * 100u) {
        if (!ch1_fullon) {
            ch1_fullon = 1;
            armed1    = 0;
            gate1_off = 0;
            HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        }
    } else if (ch1_fullon) {
        ch1_fullon = 0;
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
    }
    __set_PRIMASK(primask);
}

void Dimmer_SetIron(uint8_t percent) {
    if (percent > 100) percent = 100;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    iron_pct = percent;

    if (percent >= IRON_FULLON_PCT) {
        if (!ch2_fullon) {
            ch2_fullon = 1;
            armed2    = 0;
            gate2_off = 0;
            HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
        }
    } else if (ch2_fullon) {
        ch2_fullon = 0;
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
    }
    __set_PRIMASK(primask);
}

uint8_t Dimmer_GetHeater(void) { return (uint8_t)((heater_dq + 50u) / 100u); }
uint8_t Dimmer_GetIron(void)   { return iron_pct;   }

uint8_t Dimmer_ZcOk(void) {
    return ((dim_tick - last_zc_tick) < ZC_LOST_TICKS) ? 1 : 0;
}

/* ============================================================= ISR handlers */

void Dimmer_TickISR(void) {
    dim_tick++;

    /* No mains reference: nothing may fire (full-on needs no phase, though). */
    if (!synced || (dim_tick - last_zc_tick) > ZC_TIMEOUT_TICKS) {
        synced = 0;
        if (!ch1_fullon) {
            HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
            armed1    = 0;
            gate1_off = 0;
        }
        if (!ch2_fullon) {
            HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
            armed2    = 0;
            gate2_off = 0;
        }
        return;
    }

    /* Free-running half-cycle grid. A zero-cross re-locks it; if an edge is
     * missed the schedule simply continues from here. */
    if ((dim_tick - half_start) >= HALF_PERIOD_TICKS) {
        half_start += HALF_PERIOD_TICKS;
        /* If the ISR was held off for more than a whole half-cycle (a Flash
         * write, for instance), resynchronise instead of calling
         * schedule_half() several ticks in a row and firing repeatedly. */
        if ((dim_tick - half_start) >= HALF_PERIOD_TICKS)
            half_start = dim_tick;
        schedule_half();
    }

    /* CH1 phase-control: fire when delay expires */
    if (armed1 && dim_tick >= fire1) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_SET);
        gate1_off = gate_end(dim_tick);
        armed1 = 0;
    }
    /* CH2 phase-control: fire when delay expires */
    if (armed2 && dim_tick >= fire2) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_SET);
        gate2_off = dim_tick + GATE_PULSE_TICKS;
        armed2 = 0;
    }

    /* End gate pulses (phase-control only, not full-on) */
    if (!ch1_fullon && gate1_off != 0 && dim_tick >= gate1_off) {
        HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
        gate1_off = 0;
    }
    if (!ch2_fullon && gate2_off != 0 && dim_tick >= gate2_off) {
        HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
        gate2_off = 0;
    }
}

void Dimmer_ZeroCross(void) {
    uint32_t now = dim_tick;

    if (!synced) {
        /* first edge after power-up or after a mains dropout: lock on */
        synced       = 1;
        last_zc_tick = now;
        half_start   = now;
        schedule_half();
        return;
    }

    /* The grid free-runs at exactly one half-period, so the next real crossing
     * is expected HALF_PERIOD_TICKS after half_start. Accept an edge only in a
     * small window around that point (just before it, or just after the tick
     * ISR has already advanced the grid). Anything in the middle of the
     * half-cycle is the burst from a triac firing and must not re-lock the
     * phase, otherwise a low dimmer duty would pull every following
     * half-cycle early. */
    uint32_t since_half = now - half_start;
    if (since_half > ZC_WINDOW_TICKS &&
        since_half < (HALF_PERIOD_TICKS - ZC_WINDOW_TICKS))
        return;

    last_zc_tick = now;
    half_start   = now;
    schedule_half();
}
