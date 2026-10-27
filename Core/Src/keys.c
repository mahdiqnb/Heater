#include "keys.h"
#include "stm32g0xx_hal.h"

/* Wiring, as measured on the board: four keys on PC0..PC3, active low.
 *   SW1 = PC3 -> UP
 *   SW2 = PC2 -> LEFT
 *   SW3 = PC1 -> RIGHT
 *   SW4 = PC0 -> DOWN
 * The previous version had DOWN on PC2, LEFT on PC1 and RIGHT on PC0, so the
 * arrow keys did something different from what the silkscreen says.         */
#define SW4_PIN  GPIO_PIN_0
#define SW4_PORT GPIOC

#define DEBOUNCE_MS   10u

/* Hold-to-repeat. A normal tap always gives exactly one step; if the key stays
 * down for REP_HOLD_MS it starts repeating, and the period shrinks from
 * REP_SLOW_MS to REP_FAST_MS over REP_RAMP_MS of holding. After REP_RAMP_MS
 * the key also reports Key_*_Fast(), which the menu uses to take bigger steps
 * instead of merely ticking sooner -- turning 200 C into 400 C then takes a
 * couple of seconds instead of 200 taps.
 * (The main loop runs at ~25 ms, so the fastest step is bounded by that.)    */
#define REP_HOLD_MS   700u
#define REP_SLOW_MS   110u
#define REP_FAST_MS   35u
#define REP_RAMP_MS   3000u

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       raw;      /* last sample                     */
    uint8_t       deb;      /* debounced state, 1 = pressed    */
    uint8_t       edge;     /* press or repeat tick to consume */
    uint8_t       new_edge; /* the physical press only; a repeat never sets it */
    uint8_t       fast;     /* down for >= REP_RAMP_MS: take the big step */
    uint32_t      held_ms;  /* how long this key has been down, 0 if up    */
    uint32_t      t_raw;    /* last raw change (bounce timer)  */
    uint32_t      t_press;  /* when the press was confirmed    */
    uint32_t      t_rep;    /* when the next repeat is due     */
    uint32_t      t_release;/* when the key was last seen up   */
} key_t;

static key_t k_up    = {SW1_GPIO_Port, SW1_Pin,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static key_t k_down  = {SW4_PORT,      SW4_PIN,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static key_t k_left  = {SW2_GPIO_Port, SW2_Pin,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static key_t k_right = {SW3_GPIO_Port, SW3_Pin,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static uint8_t read_key(const key_t *k) {
    return (HAL_GPIO_ReadPin(k->port, k->pin) == GPIO_PIN_RESET) ? 1 : 0;
}

static void service(key_t *k) {
    uint32_t now = HAL_GetTick();
    uint8_t  r = read_key(k);

    if (r != k->raw) {          /* still bouncing: restart the timer */
        k->raw = r;
        k->t_raw = now;
    }
    if ((now - k->t_raw) >= DEBOUNCE_MS) {
        if (r && !k->deb) {                 /* new press */
            k->edge = 1;
            k->new_edge = 1;
            /* A switch can drop its contact for a few ms and look like a new
               press. Only a real release (> 250 ms) restarts the hold ramp,
               otherwise the acceleration would restart over and over and the
               value would crawl one step at a time. */
            if (k->t_press == 0u || (uint32_t)(now - k->t_release) > 250u)
                k->t_press = now;
            k->t_rep = now + REP_HOLD_MS;
        } else if (!r && k->deb) {          /* released */
            k->t_rep = 0;
            k->t_release = now;
        }
        k->deb = r;
    }

    /* the fast phase starts once the key has been down for REP_RAMP_MS; the
       caller uses it to grow the step instead of shrinking the period further */
    k->fast = (k->deb && (uint32_t)(now - k->t_press) >= REP_RAMP_MS) ? 1 : 0;
    k->held_ms = k->deb ? (uint32_t)(now - k->t_press) : 0;

    if (k->deb && k->t_rep && (int32_t)(now - k->t_rep) >= 0) {
        k->edge = 1;
        uint32_t held = now - k->t_press;
        uint32_t span = (held > REP_RAMP_MS) ? REP_RAMP_MS : held;
        uint32_t period = REP_FAST_MS +
                          (REP_RAMP_MS - span) * (REP_SLOW_MS - REP_FAST_MS) / REP_RAMP_MS;
        k->t_rep = now + period;
    }
}

void Keys_Init(void) {
    k_up.raw = k_up.deb = k_up.edge = k_up.new_edge = k_up.fast = 0;
    k_down.raw = k_down.deb = k_down.edge = k_down.new_edge = k_down.fast = 0;
    k_left.raw = k_left.deb = k_left.edge = k_left.new_edge = k_left.fast = 0;
    k_right.raw = k_right.deb = k_right.edge = k_right.new_edge = k_right.fast = 0;
    k_up.t_rep = k_down.t_rep = k_left.t_rep = k_right.t_rep = 0;
}

void Keys_Update(void) {
    service(&k_up);
    service(&k_down);
    service(&k_left);
    service(&k_right);
}

uint8_t Key_Up(void)    { return k_up.deb; }
uint8_t Key_Down(void)  { return k_down.deb; }
uint8_t Key_Left(void)  { return k_left.deb; }
uint8_t Key_Right(void) { return k_right.deb; }

/* One step per call: the press itself and, while the key is held, every
 * repeat tick. Callers that only want the physical press cannot tell the
 * difference, which is why the repeat lives here and not in the menu code. */
uint8_t Key_Up_Pressed(void)    { uint8_t e = k_up.edge;    k_up.edge = 0;    return e; }
uint8_t Key_Down_Pressed(void)  { uint8_t e = k_down.edge;  k_down.edge = 0;  return e; }
uint8_t Key_Left_Pressed(void)  { uint8_t e = k_left.edge;  k_left.edge = 0;  return e; }
uint8_t Key_Right_Pressed(void) { uint8_t e = k_right.edge; k_right.edge = 0; return e; }

/* The physical press only, never a repeat tick: switches (HEATER, FAN MODE,
 * BOX FAN) and page changes must fire once per press, or holding the key
 * would flip them at the repeat rate. Always read these so no press is left
 * pending across passes. */
uint8_t Key_Left_New(void)  { uint8_t e = k_left.new_edge;  k_left.new_edge = 0;  return e; }
uint8_t Key_Right_New(void) { uint8_t e = k_right.new_edge; k_right.new_edge = 0; return e; }

/* 1 while the key has been held for REP_RAMP_MS or more: the caller then
   changes values in bigger steps (that is the "much faster after 3 s"). */
uint8_t Key_Left_Fast(void)  { return k_left.fast; }
uint8_t Key_Right_Fast(void) { return k_right.fast; }

/* Milliseconds the key has been held (0 while released). The menu turns this
   into a step size, so a value speeds up progressively instead of waiting
   for one single threshold. */
uint32_t Key_Left_HoldMs(void)  { return k_left.held_ms; }
uint32_t Key_Right_HoldMs(void) { return k_right.held_ms; }
