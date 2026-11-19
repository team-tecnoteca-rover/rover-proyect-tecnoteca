import wave, struct, os
d = r"c:\Users\Tecnoteca\Documents\Arduino\ojosmic"
names = ["hola","robertito","bienvenido","ay","bip","robot"]
out = ["// Generado por tools/wav_a_header.py (voz es-ES, 22050 Hz, 16 bit, mono)", "#pragma once", "#include <stdint.h>", ""]
for n in names:
    w = wave.open(os.path.join(d,"tools",n+".wav"))
    assert w.getframerate()==22050 and w.getnchannels()==1 and w.getsampwidth()==2
    raw = w.readframes(w.getnframes())
    s = list(struct.unpack("<%dh"%(len(raw)//2), raw))
    thr = 300
    a = 0
    while a < len(s) and abs(s[a]) < thr: a += 1
    b = len(s)
    while b > a and abs(s[b-1]) < thr: b -= 1
    s = s[max(0,a-100):min(len(s), b+100)]
    pk = max(abs(x) for x in s) or 1
    # La voz tiene picos altos y poca energia media (suena bajito aunque el pico sea maximo).
    # Se sube la ganancia por RMS y un limitador suave (tanh) evita el recorte duro.
    import math
    # Preenfasis: los parlantes chicos casi no reproducen graves (la voz vive ahi) y
    # rinden mucho mas en agudos (como los bips). Se refuerzan agudos y se quitan graves.
    s = [s[0]] + [s[i] - 0.7*s[i-1] for i in range(1, len(s))]
    rms = math.sqrt(sum(x*x for x in s)/len(s)) or 1
    TARGET_RMS = 12000
    g = TARGET_RMS / rms
    s = [int(32000*math.tanh(x*g/32000)) for x in s]
    print("  ganancia x%.1f" % g)
    out.append("const int16_t voz_%s[] = {" % n)
    for i in range(0, len(s), 16):
        out.append("  " + ",".join(str(x) for x in s[i:i+16]) + ",")
    out.append("};")
    out.append("const uint32_t voz_%s_len = %d;" % (n, len(s)))
    out.append("")
    print(n, len(s), round(len(s)/22050,2),"s")
open(os.path.join(d,"voces.h"),"w").write("\n".join(out))
