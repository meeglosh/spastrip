#!/usr/bin/env python3
"""Process one original IR -> 48 kHz / 24-bit FLAC.
Usage: process_ir.py in.wav out.flac [--trim-preroll[=KEEP_MS]]
Steps: trim leading digital silence (>5 ms) -> resample 48k (ffmpeg swr, high-quality params)
-> [opt-in] trim low-level pre-roll -> cap at 10 s with 0.5 s fade-out if longer
-> peak-normalise to -1 dBFS. Prints JSON.
--trim-preroll (opt-in, default OFF; used only where an original has a long stretch of
low-level noise before the direct sound, e.g. karlskirche): after resampling, find the onset =
first frame whose max |sample| across channels is within 40 dB of the file's peak (>= 0.01 x peak),
and cut everything before (onset - KEEP_MS), default KEEP_MS = 1.0 ms, so about 1 ms of
lead-in remains before the direct sound. The onset is never cut (the trim point is always
before it); the cut is skipped if the onset is already within KEEP_MS of the start.
The JSON gains "preroll_trimmed_ms" (null when the option is off).
All intermediate stages are float32 (no clipping before the gain stage). No EQ/denoise.
Resampler: ffmpeg swr (libsoxr/sox not available in this environment).
Truncation+fade path verified with synthetic 192 kHz/float32/+8 dBFS-peak test (2026-10-01): fade is quarter-sine, ends at 0."""
import sys, subprocess, struct, array, json, os, tempfile, math, shutil
pos = [a for a in sys.argv[1:] if not a.startswith("--")]
src, dst = pos[0], pos[1]
TRIM_PREROLL = False; KEEP_MS = 1.0
for a in sys.argv[1:]:
    if a == "--trim-preroll": TRIM_PREROLL = True
    elif a.startswith("--trim-preroll="): TRIM_PREROLL = True; KEEP_MS = float(a.split("=",1)[1])
    elif a.startswith("--"): raise SystemExit("unknown option " + a)
SR = 48000
def probe(p):
    o = subprocess.check_output(["ffprobe","-v","error","-select_streams","a:0","-show_entries",
        "stream=sample_rate,channels,duration,codec_name,bits_per_raw_sample,bits_per_sample","-of","json",p])
    return json.loads(o)["streams"][0]
def decode(p, extra=()):
    raw = subprocess.check_output(["ffmpeg","-v","error","-i",p,*extra,"-f","f32le","-c:a","pcm_f32le","-"])
    a = array.array("f"); a.frombytes(raw); return a
info = probe(src); ch = int(info["channels"]); sr = int(info["sample_rate"])
a = decode(src)
n = len(a)//ch
# first non-zero frame (digital silence = exactly 0)
first = next((i for i in range(n) if any(a[i*ch+c] != 0.0 for c in range(ch))), None)
if first is None: raise SystemExit("silent file")
trim = first/sr
trim_applied = trim > 0.005
start = first/sr if trim_applied else 0.0
tdir = tempfile.mkdtemp(prefix="process_ir_")  # private temp dir (honours $TMPDIR), removed at exit
tmp = os.path.join(tdir, "stage.wav")
rs = f"aresample=resampler=swr:out_sample_rate={SR}:filter_size=128:phase_shift=15:linear_interp=0:cutoff=0.99:dither_method=0"
flt = [f"atrim=start={start:.9f}", "asetpts=PTS-STARTPTS"]
if sr != SR: flt.append(rs)
subprocess.check_call(["ffmpeg","-v","error","-y","-i",src,"-af",",".join(flt),"-c:a","pcm_f32le",tmp])
b = decode(tmp); m = len(b)//ch; dur = m/SR
preroll_ms = None
if TRIM_PREROLL:
    bpk = max(max(b), -min(b))
    onset = next(i for i in range(m) if max(abs(b[i*ch+c]) for c in range(ch)) >= bpk*0.01)
    cut = max(0, onset - int(round(KEEP_MS*SR/1000.0)))
    if cut > 0:
        subprocess.check_call(["ffmpeg","-v","error","-y","-i",tmp,"-af",f"atrim=start_sample={cut},asetpts=PTS-STARTPTS",
            "-c:a","pcm_f32le",tmp+".pre.wav"])
        os.replace(tmp+".pre.wav", tmp); b = decode(tmp); m = len(b)//ch; dur = m/SR
    preroll_ms = round(cut/SR*1000, 3)
truncated = dur > 10.0
fade = None
if truncated:
    flt2 = "atrim=end_sample=480000,afade=t=out:st=9.5:d=0.5:curve=qsin"
    subprocess.check_call(["ffmpeg","-v","error","-y","-i",tmp,"-af",flt2,"-c:a","pcm_f32le",tmp+".2.wav"])
    os.replace(tmp+".2.wav", tmp); b = decode(tmp); m = len(b)//ch
peak = max(max(b), -min(b))
gain = 10**(-1/20)/peak
gdb = 20*math.log10(gain)
subprocess.check_call(["ffmpeg","-v","error","-y","-i",tmp,"-af",f"volume={gain:.10f}","-c:a","flac",
    "-sample_fmt","s32","-bits_per_raw_sample","24","-compression_level","8",dst])
shutil.rmtree(tdir, ignore_errors=True)
out = probe(dst); c = decode(dst)
opk = max(max(c), -min(c))
oc = len(c)//ch
fm = [max(abs(c[i*ch+k]) for k in range(ch)) for i in range(oc)]
ipk = max(range(oc), key=fm.__getitem__)
on40 = next(i for i,v in enumerate(fm) if v >= opk*0.01)   # first frame within 40 dB of peak
on20 = next(i for i,v in enumerate(fm) if v >= opk*0.1)    # first frame within 20 dB of peak
print(json.dumps({"src_rate":sr,"src_channels":ch,"src_codec":info["codec_name"],"src_bits":info.get("bits_per_raw_sample") or info.get("bits_per_sample"),
 "src_duration":float(info["duration"]),"leading_silence_s":round(trim,6),"trimmed":trim_applied,"resampled":sr!=SR,
 "truncated":truncated,"preroll_trimmed_ms":preroll_ms,"gain_db":round(gdb,3),"out_rate":int(out["sample_rate"]),"out_channels":int(out["channels"]),
 "out_bits":out.get("bits_per_raw_sample"),"out_duration":round(len(c)/ch/SR,4),"out_peak_dbfs":round(20*math.log10(opk),3),
 "onset_m40_ms":round(on40/SR*1000,3),"onset_m20_ms":round(on20/SR*1000,3),"peak_ms":round(ipk/SR*1000,3)}))
