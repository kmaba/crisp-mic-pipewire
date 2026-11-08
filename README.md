# Crisp Mic on PipeWire — WebRTC 2.1 AEC3 for a laptop mic + Pebble speakers

A PipeWire echo-cancellation setup that keeps a laptop internal mic's
quality intact while removing everything the speakers play — including
music and podcasts — without the voice turning "underwater" when you
talk over them.

Built and measured on: CachyOS, PipeWire 1.6.10, ALC256 internal mic,
Creative Pebble V3 speakers on the analog jack (`06:00.6`).

> **TL;DR:** Stock PipeWire ships WebRTC 1.3 (`libspa-aec-webrtc.so`),
> whose AEC3 double-talk tuning is designed for phone calls and low-pass
> filters your voice above ~2 kHz whenever speaker audio is playing.
> This repo builds a WebRTC **2.1** SPA plugin with a custom
> `EchoCanceller3Config` (full-band double-talk, fast near-end detection,
> linear-AEC-guided noise suppression) and a two-node PipeWire graph
> (`mic_crisp` source + `pebble_out` sink). Measured: ~25–34 dB of
> speaker cancellation with only ~1–2 dB of voice loss over music,
> and a flat voice spectrum (no tin can) with NS-low + high-pass-off.

---

## The problem

1. **Speaker pickup with no reference.** The internal mic hears the
   Pebbles. Apps' built-in "noise suppression / echo cancellation"
   fix it, but at the cost of the mic's natural quality.
2. **"Underwater" double-talk.** With any far-end audio (music, podcast,
   a caller's voice) playing while you talk, stock WebRTC AEC3 treated
   near-end speech as echo below ~1.8 kHz and *crushed* it above
   ~2 kHz. Result: muffled, tin-can voice precisely when speakers run.
3. **Gain staging.** The raw mic node sat at **19% / -43.6 dB** and the
   virtual sink kept getting restored to **50% / -18 dB**, so voice
   arrived at the canceller whisper-quiet and playback lost ~18 dB.

## Architecture

```
Mic (ALC256) ──► capture.raw_mic ──┐
                                    ▼
                            libspa-aec-webrtc2 (WebRTC 2.1 AEC3)
                                    │  render reference
Apps ──► pebble_out (sink) ──► playback.speakers ──► Pebble V3 speakers
Video calls / Discord / Audacity ──► mic_crisp (source = processed mic)
```

- **`pebble_out`** — everything played into it goes to the Pebbles *and*
  is fed to the canceller as the exact far-end reference. Whatever comes
  out of the speakers can be modelled and subtracted.
- **`mic_crisp`** — the only mic apps should use. Processing happens once,
  at full quality, before any app touches the signal. In-app noise
  suppression / echo cancellation / AGC must be **off**.

## Why stock WebRTC muffles you (the actual bug)

In `EchoCanceller3Config::Suppressor` (WebRTC defaults):

| Parameter | Stock default | Effect |
|---|---|---|
| `nearend_tuning.mask_lf` (LF, < ~1.8 kHz) | `1.09 / 1.1` | Low voice kept |
| `nearend_tuning.mask_hf` (HF, > ~1.8 kHz) | **`0.1 / 0.3`** | High voice crushed ~15 dB during music = "underwater" |
| `dominant_nearend_detection.snr_threshold` | `30.0` | Needs +15 dB SNR before it even admits you're speaking |
| `dominant_nearend_detection.trigger_threshold` | `12` blocks | Slow to enter double-talk mode |
| NS level | `kHigh`, analysing *post*-suppressor | Watery musical noise on top |

`transient_suppression` (default on in stock) also eats keyboard clicks,
while `gain_control`/AGC (default off in stock, thankfully) is what
boosts "sounds from miles away."

## The fix

### 1. Custom WebRTC 2.1 plugin — `src/aec-webrtc2.cpp`

PipeWire's `aec-webrtc.cpp` with `HAVE_WEBRTC2`, plus a
`CustomAec3Factory : public webrtc::EchoControlFactory` that constructs
`webrtc::EchoCanceller3` with a caller-supplied config (its constructor,
vtable and typeinfo are exported by `libwebrtc-audio-processing-2.so`).
That gives full control over every AEC3 knob. Defaults baked in:

- `nearend_tuning.mask_lf` = `4.0 / 6.0`, `mask_hf` = `3.5 / 5.0`
  (overridable via `aec.args`: `webrtc.nearend_hf_transparent`, etc.)
- `dominant_nearend_detection`: `snr_threshold 6.0`, `trigger 3`,
  `enr_threshold 1.5`
- `export_linear_aec_output = true` +
  `analyze_linear_aec_output_when_available = true` (noise suppression
  analyses the clean linear filter output, not the warped post-suppressor)

The shipped config pushes the near-end masks further to near-transparent
(`8.0 / 12.0`) so the non-linear suppressor barely touches voice in
double-talk; music cancellation when you're silent still comes from the
linear filter + normal tuning.

### 2. PipeWire config — `config/90-crisp-mic.conf`

Final `aec.args`:

```
webrtc.high_pass_filter = false
webrtc.noise_suppression = true
webrtc.noise_suppression.level = low
webrtc.linear_aec_ns = true
webrtc.crisp_doubletalk = true
webrtc.nearend_hf_transparent = 8.0   webrtc.nearend_hf_suppress = 12.0
webrtc.nearend_lf_transparent = 8.0   webrtc.nearend_lf_suppress = 12.0
webrtc.nearend_snr_threshold = 4.0    webrtc.nearend_trigger_blocks = 1
webrtc.gain_control = false  (both gain controllers off)
webrtc.mobile_mode = false
webrtc.transient_suppression = false   (keyboard clicks survive)
```

`noise_suppression.level = low` + `high_pass_filter = false` is the
"studio tone" setting: it restored the 80–300 Hz body that the
high-pass + moderate NS had rolled off by 5–12 dB (the tin-can tilt).
Trade-off: a little more fan hiss in pauses — manageable, and the
config comment/level is a one-word change back to `moderate`/`high`
if you prefer quieter pauses over fuller voice.

### 3. Gain staging + persistence

| Node | Volume | Why |
|---|---|---|
| `alsa_input...analog-stereo` (raw mic, pre-AEC) | **50%** (was 19%) | Voice peaks ≈ -6 dBFS into the canceller; no clipping, far better SNR for detection |
| `mic_crisp` (post-AEC, what apps see) | **70% / -9.3 dB** | Sane level for Audacity/Discord without touching AEC input |
| `pebble_out` (virtual sink) | **100%** | Transparent pass-through; your usual Ryzen-sink (50%) + hardware sliders control loudness as before |
| `Echo-Cancel Playback` stream | 100%, unmuted | The cable between `pebble_out` and the jack |

**Persistence bug found:** WirePlumber saved the virtual sink as
`channelVolumes:[0.125, 0.125]` in
`~/.local/state/wireplumber/stream-properties`, so every restart
restored `pebble_out` to 50% (-18 dB) *in series* with the physical
sink — that's why the Pebbles suddenly got much quieter. Setting it to
100% live makes WirePlumber rewrite that entry; verified it survives
both `wireplumber` and full `pipewire` restarts. Same story for the
raw mic volume (19% → 50%).

## Build & install

```sh
# WebRTC 2.1 (coexists with the legacy -1 package)
sudo pacman -S webrtc-audio-processing

g++ -O2 -std=c++17 -shared -fPIC -fvisibility=hidden \
  -DSPA_EXPORT='__attribute__((visibility("default")))' \
  $(pkg-config --cflags libspa-0.2 webrtc-audio-processing-2) \
  src/aec-webrtc2.cpp \
  $(pkg-config --libs webrtc-audio-processing-2) \
  -o libspa-aec-webrtc2.so

sudo install -m 755 libspa-aec-webrtc2.so /usr/lib/spa-0.2/aec/
mkdir -p ~/.config/pipewire/pipewire.conf.d
cp config/90-crisp-mic.conf ~/.config/pipewire/pipewire.conf.d/

systemctl --user restart pipewire wireplumber pipewire-pulse
pactl set-default-source mic_crisp
pactl set-default-sink pebble_out
pactl set-sink-volume pebble_out 100%
pactl set-source-volume alsa_input.pci-0000_06_00.6.analog-stereo 50%
pactl set-source-volume mic_crisp 70%
```

Verify the engine actually loaded:

```sh
grep -E 'libspa-aec-webrtc2|libwebrtc-audio-processing-2' \
  /proc/$(pgrep -x pipewire)/maps
```

The stock `libspa-aec-webrtc.so` is left untouched, so removal is just
deleting the config + the one `.so` and restarting PipeWire.

## Usage

- **Input:** `Crisp Mic (no speaker pickup)` everywhere.
- **Output:** `Pebbles (microphone-echo-cancelling)` (system default).
- **In every app:** Noise Suppression **off**, Echo Cancellation **off**,
  AGC **off** — the system chain does it once, better.
- **Audacity:** Transport → Software Playthrough **off** (otherwise you
  hear yourself delayed and the canceller fights your own voice). Aim for
  speech peaks around -12 to -6 dB; lower the *Crisp Mic* slider, not
  the raw mic, if you clip.
- Hear yourself live (safe — it goes through the canceller):
  `pw-loopback -C mic_crisp -P pebble_out`
- Back out anytime: pick the raw `Ryzen HD Audio` mic in your panel.

## Measurements

Synthetic voice stand-in (harmonic bursts + consonant sprints, played
from the Pebbles via the *bypass* path so the mic perceives it like a
room voice) + synthesized music groove. Recorded simultaneously as
`mic_crisp` (processed) and the raw ALSA mic. Full scripts in `scripts/`.

### Offline simulation (stock vs. tuned, same room model)

| Metric | Stock AEC3 + NS kHigh | Tuned WebRTC 2.1 |
|---|---|---|
| Music+fan suppression, silent | 25.9 dB | 25.2 dB |
| Voice < 2 kHz over music | -8.2 dB | **-1.3 dB** |
| Voice > 2 kHz over music | **-15.6 dB** | **-2.3 dB** |
| HF-to-LF tilt (0 = natural) | -7.4 dB (underwater) | **-1.0 dB** |

### Real chain, final settings

| Metric | Result |
|---|---|
| Music-only cancellation (11–19 s window) | raw -18.6 → proc -52.5 dBFS (**33.9 dB removed**) |
| Voice alone (processed) | RMS -21.8 dBFS, peak -9.3 dBFS |
| Voice over music (processed) | RMS -22.3 dBFS, peak -9.3 dBFS |
| Per-band voice retention over music | 80–300 Hz -1.1 dB, 300 Hz–1 kHz +0.1 dB, 1–3 kHz -1.4 dB, 3–8 kHz +1.2 dB, 8–12 kHz +1.4 dB |
| Ambient (fan/window) removal, total | **26.2 dB** (raw -15.2 → proc -41.4 dBFS; see caveats) |
| Voice-vs-ambient margin (processed) | ~19 dB |

### Clean rerun (tester silent, steady-state medians)

Re-run with the tester completely quiet (no keyboard/mouse/voice) and
the browser stream silent, using median 0.5 s frames and excluding the
first 1.5 s (capture-start transient):

| Metric | Result |
|---|---|
| Steady ambient, raw → processed | -26.1 → -45.9 dBFS (**19.8 dB removed**) |
| Voice alone, processed (median frame) | -21.9 dBFS |
| Voice margin over steady ambient | **24.1 dB** |
| Voice over music vs. voice alone | **+0.1 dB** (no measurable muffling) |
| Music-only cancellation (median frames) | raw -18.0 → proc -56.6 dBFS (**38.6 dB removed**) |
| Per-band voice retention over music | 80–300 Hz +0.0, 300 Hz–1 kHz +0.5, 1–3 kHz +1.2, 3–8 kHz +1.0, 8–12 kHz +1.0 dB |

A wind gust through the open window around 7 s still shows up in the
processed ambient range (-51…-28 dBFS) — the fan's steady white noise
is what's been removed; gusts are physics, not a processing failure.

### Tone (tin-can fix), voice alone, per band (processed − raw)

| Band | Before (NS moderate, HP on) | After (NS low, HP off)* |
|---|---|---|
| 60–120 Hz | -12.2 dB | body restored |
| 120–250 Hz | -4.7 dB | -2.9 dB |
| 250–500 Hz | -2.1 dB | -1.2 dB |
| 500 Hz–1 kHz | -6.8 dB | -2.2 dB |
| 1–4 kHz | -4.4 / -5.1 dB | ≈ -2.6 dB |
| 4–16 kHz | -3.3 / -2.4 dB | ≈ -2.0 dB |

\* The "after" column is corrected for the -9.3 dB `mic_crisp`
output-volume change, so it's a pure tone comparison: flat within
~3 dB from 80 Hz to 16 kHz, versus a 10 dB low-end hole before.

### Caveats

- The voice stand-in is synthetic; human double-talk (pauses, onsets,
  real formants) is the final judge. Subjective confirmation so far:
  Audacity recordings (Audacity applies no processing of its own) sound
  markedly better than stock, with cancellation described as working
  well and residual fan hiss as "manageable."
- During some test runs the tester used keyboard/mouse, and a browser
  stream was playing for part of the ambient captures. Transient and
  quiet-floor windows in those runs are therefore upper bounds, not
  clean floors; headline numbers use medians and music/voice windows
  where the effect is unambiguous.
- WebRTC 1.3 vs 2.1: the stock PipeWire binary is built against
  `webrtc-audio-processing-1` (1.3). Upstream PipeWire builds against
  2.x will make most of the plugin rebuild unnecessary — the
  `aec.args` keys and config carry over.

## Files

- `config/90-crisp-mic.conf` — the live PipeWire config
- `src/aec-webrtc2.cpp` — the custom SPA plugin (SPA ABI-identical
  exports to stock: `spa_handle_factory_enum`, log-topic symbols)
- `scripts/test2.sh`, `scripts/harness.sh` — measurement harnesses
  (music+voice and ambient+voice, processed/raw in parallel)
