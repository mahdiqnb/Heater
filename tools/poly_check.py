import re
src = open('../Core/Src/temp.c', encoding='utf-8').read()
blk = re.search(r'd\[10\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
vals = [float(x.strip().rstrip('f')) for x in blk.replace('\n',' ').split(',') if x.strip()]
print("coefficients parsed:", len(vals))
def horner(mv):          # exactly the loop in temp.c
    t = vals[9]
    for i in range(8, 0, -1):
        t = t * mv + vals[i]
    return t * mv
ref = {0:0.000, 25:1.000, 100:4.096, 200:8.138, 300:12.209,
       400:16.397, 450:18.516, 500:20.644}
bad = 0
for T, e in ref.items():
    got = horner(e)
    err = got - T
    if abs(err) > 1.0: bad += 1
    print(f"  E={e:6.3f}mV  expect {T:3d}C  got {got:7.2f}C  err {err:+5.2f}")
print("FAIL" if bad else "poly OK")
print("ADC full scale ->", round(horner(3.3/150.7*1000), 1), "C")
