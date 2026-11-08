#!/bin/zsh
cd /tmp/opencode/mictest
rm -f c_amb_proc.wav c_amb_raw.wav c_vox_proc.wav c_vox_raw.wav c_mus_proc.wav c_mus_raw.wav

ZID=$(pactl list sink-inputs | awk '/Sink Input #/{id=$3} /application.name = "Zen"/{print id; exit}')
if [ -n "$ZID" ]; then pactl set-sink-input-mute "$ZID" 1 && echo "Zen muted ($ZID)"; fi

echo "[1/3] ambient 10s (stay quiet)..."
pw-cat -r -n 480000 --target mic_crisp c_amb_proc.wav & P1=$!
pw-cat -r -n 480000 --target alsa_input.pci-0000_06_00.6.analog-stereo c_amb_raw.wav & P2=$!
wait $P1; wait $P2

echo "[2/3] voice alone 10s (synth bursts via speakers)..."
pw-cat -r -n 480000 --target mic_crisp c_vox_proc.wav & P1=$!
pw-cat -r -n 480000 --target alsa_input.pci-0000_06_00.6.analog-stereo c_vox_raw.wav & P2=$!
sleep 0.8
pw-cat -p --target alsa_output.pci-0000_06_00.6.analog-stereo --volume 0.55 speechish.wav
wait $P1; wait $P2

echo "[3/3] music+voice 26s (music via pebble_out, voice via bypass)..."
pw-cat -r -n 1248000 --target mic_crisp c_mus_proc.wav & P1=$!
pw-cat -r -n 1248000 --target alsa_input.pci-0000_06_00.6.analog-stereo c_mus_raw.wav & P2=$!
sleep 0.5
pw-cat -p --target pebble_out --volume 0.5 musicish.wav &
sleep 0.8
pw-cat -p --target alsa_output.pci-0000_06_00.6.analog-stereo --volume 0.55 speechish.wav
wait $P1; wait $P2

if [ -n "$ZID" ]; then pactl set-sink-input-mute "$ZID" 0 && echo "Zen restored"; fi
echo "[clean] DONE"
