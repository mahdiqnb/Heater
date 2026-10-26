/* Host test bench for Core/Src/control.c: the real controller, compiled for
   the PC, driving a first order thermal model. Used to check settling,
   overshoot, windup and the runaway detector without hardware. */
#include <stdio.h>
#include <stdint.h>

static uint32_t t_ms;
uint32_t HAL_GetTick(void) { return t_ms; }

static uint8_t duty;
void Dimmer_SetHeater(uint8_t p) { duty = p; }
void Dimmer_SetIron(uint8_t p) { (void)p; }
uint8_t Dimmer_GetIron(void) { return 0; }
uint8_t Dimmer_GetHeater(void) { return duty; }

#include "../../Core/Src/control.c"

#define TAU_S   70.0f   /* thermal time constant of the element + block */
#define K_C     260.0f  /* steady state rise at 100 % duty */

static float plant(float T, uint8_t d, float Tamb) {
    float target = Tamb + K_C * (float)d / 100.0f;
    return T + (target - T) * (0.1f / TAU_S);
}

static void run(const char *name, float sp, float stuck_at) {
    float T = 25.0f, Tamb = 25.0f;
    int i;
    float peak = 0.0f;
    int settle = -1;

    t_ms = 1000;
    Control_Init();
    Control_SetEnabled(1);
    duty = 0;
    printf("\n--- %s  (setpoint %.0f C) ---\n", name, sp);
    printf("   t[s]   T[C]   duty  fault\n");

    for (i = 0; i < 12000; i++) {           /* 1200 s, step = 100 ms */
        t_ms += 100;
        if (stuck_at < 0.0f) T = plant(T, duty, Tamb);
        else T = stuck_at;                   /* disconnected / frozen sensor */
        Control_Update(T, sp);
        if (T > peak) peak = T;
        if (settle < 0 && i > 300 && T > sp - 2.0f && T < sp + 2.0f) settle = i;
        if (i % 1000 == 0) printf("  %5.0f  %6.1f   %3u    %u\n",
                                  i * 0.1f, T, duty, Control_GetFault());
    }
    printf("  peak %.1f C  (overshoot %+.1f C)   settled at %.0f s   final duty %u %%\n",
           peak, peak - sp, settle * 0.1f, duty);
}

int main(void) {
    run("cold start", 200.0f, -1.0f);
    run("hot setpoint", 350.0f, -1.0f);
    run("sensor stuck / open", 200.0f, 25.0f);
    return 0;
}
