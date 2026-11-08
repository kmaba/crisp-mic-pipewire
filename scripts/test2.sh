#!/bin/zsh
cd /tmp/opencode/mictest
rm -f t_proc.wav t_raw.wav b_proc.wav b_raw.wav

echo "[t] music+voice..."
pw-cat -r -n 1248000 --target mic_crisp t_proc.wav & P1=$!
pw-cat -r -n 1248000 --target alsa_input.pci-0000_06_00.6.analog-stereo t_raw.wav & P2=$!
sleep 0.5
pw-cat -p --target pebble_out --volume 0.5 musicish.wav &
sleep 0.8
pw-cat -p --target alsa_output.pci-0000_06_00.6.analog-stereo --volume 0.55 speechish.wav
wait $P1; wait $P2

sleep 1
echo "[t] voice alone baseline..."
pw-cat -r -n 480000 --target mic_crisp b_proc.wav & P1=$!
pw-cat -r -n 480000 --target alsa_input.pci-0000_06_00.6.analog-stereo b_raw.wav & P2=$!
sleep 0.8
pw-cat -p --target alsa_output.pci-0000_06_00.6.analog-stereo --volume 0.55 speechish.wav
wait $P1; wait $P2
echo "[t] DONE"
