"""Line by line transcription of Core/Src/control.c, run against a first order
thermal plant. There is no host C compiler on this machine, so this model is
the only way to look at the closed loop behaviour before flashing.

Keep it in sync with control.c: every constant and every branch matters."""

KP_DEF, KI_DEF, KD_DEF = 2.0, 0.05, 1.0
OUT_SLEW_PCT_S = 60.0
DERIV_TAU_S = 1.5
RUNAWAY_MS = 30000
RUNAWAY_DUTY = 60
RUNAWAY_ERR_C = 15.0
RUNAWAY_DELTA_C = 5.0
ABS_MAX_C = 520.0

CTRL_FAULT_NONE, CTRL_FAULT_SENSOR = 0, 2


class Control:
    def __init__(self):
        self.kp, self.ki, self.kd = KP_DEF, KI_DEF, KD_DEF
        self.integ = 0.0
        self.d_term = 0.0
        self.prev_meas = 0.0
        self.out_prev = 0.0
        self.primed = False
        self.prev_ms = 0
        self.enabled = False
        self.fault = CTRL_FAULT_NONE
        self.heater_pct = 0
        self.rise_active = False
        self.rise_ms = 0

    def shutdown(self):
        self.heater_pct = 0
        self.integ = 0.0
        self.d_term = 0.0
        self.out_prev = 0.0

    def update(self, measured, sp, now, ambient=25.0):
        dt = (now - self.prev_ms) / 1000.0
        self.prev_ms = now
        dt = min(max(dt, 0.02), 0.50)

        if measured > ABS_MAX_C:
            self.shutdown()
            self.prev_meas = measured
            self.primed = True
            return self.heater_pct

        if (not self.enabled) or self.fault != CTRL_FAULT_NONE or sp <= 0.0:
            self.shutdown()
            self.prev_meas = measured
            self.primed = True
            return self.heater_pct

        err = sp - measured

        if self.primed:
            dm = (measured - self.prev_meas) / dt
            target = -self.kp * self.kd * dm
            a = dt / (DERIV_TAU_S + dt)
            self.d_term += a * (target - self.d_term)
        else:
            self.d_term = 0.0
            self.primed = True

        di = self.ki * err * dt
        self.integ += di
        self.integ = min(max(self.integ, 0.0), 100.0)

        out = self.kp * err + self.integ + self.d_term
        if out > 100.0:
            out = 100.0
            if err > 0.0:
                self.integ -= di
        elif out < 0.0:
            out = 0.0
            if err < 0.0:
                self.integ -= di
        self.integ = min(max(self.integ, 0.0), 100.0)

        step = OUT_SLEW_PCT_S * dt
        if out > self.out_prev + step:
            out = self.out_prev + step
            if err > 0.0:
                self.integ -= di
        elif out < self.out_prev - step:
            out = self.out_prev - step
            if err < 0.0:
                self.integ -= di
        self.integ = min(max(self.integ, 0.0), 100.0)

        self.heater_pct = int(out + 0.5)
        self.out_prev = out
        self.prev_meas = measured

        if self.heater_pct >= RUNAWAY_DUTY and err > RUNAWAY_ERR_C:
            if not self.rise_active:
                self.rise_active = True
                self.rise_ms = now
            elif (now - self.rise_ms) >= RUNAWAY_MS:
                ambc = ambient if -40.0 < ambient < 100.0 else 25.0
                if (measured - ambc) < RUNAWAY_DELTA_C:
                    self.fault = CTRL_FAULT_SENSOR
                    self.shutdown()
                else:
                    self.rise_ms = now
        else:
            self.rise_active = False
        return self.heater_pct


TAU_S = 70.0
K_C = 260.0


def plant(T, duty, Tamb):
    target = Tamb + K_C * duty / 100.0
    return T + (target - T) * (0.1 / TAU_S)


def run(name, sp, stuck=None, seconds=1200, tamb=25.0):
    c = Control()
    c.enabled = True
    T, Tamb = tamb, tamb
    duty = 0
    now = 1000
    peak = 0.0
    settle = None
    print(f"\n--- {name} (sp {sp:.0f} C) ---")
    print("    t[s]    T[C]  duty  fault")
    for i in range(int(seconds * 10)):
        now += 100
        T = plant(T, duty, Tamb) if stuck is None else stuck
        duty = c.update(T, sp, now, Tamb)
        peak = max(peak, T)
        if settle is None and i > 300 and abs(T - sp) < 2.0:
            settle = i * 0.1
        if i % 1000 == 0:
            print(f"  {i*0.1:6.0f} {T:7.1f} {duty:5d} {c.fault:6d}")
    sd = f"{settle:.0f} s" if settle is not None else "never"
    print(f"  peak {peak:.1f} C (overshoot {peak-sp:+.1f})  settled {sd}  final duty {duty}%  fault {c.fault}")


run("cold start", 200.0)
run("setpoint above what the plant can reach", 350.0)
run("open thermocouple (reads ambient)", 200.0, stuck=25.0)
run("cold room, normal", 200.0, tamb=5.0)
