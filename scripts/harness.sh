#!/bin/zsh
cd /tmp/opencode/mictest
rm -f a_proc.wav a_raw.wav b_proc.wav b_raw.wav
ZID=$(pactl list sink-inputs | grep -B2 'application.name = "Zen"' | grep -m1 'Sink Input #' | awk '{print $3}')
[ -n "$ZID" ] && pactl set-sink-input-mute "$ZID" 1 && echo "Zen muted ($ZID)"
pw-cat -r -n 384000 --target mic_crisp a_proc.wav & P1=$!
pw-cat -r -n 384000 --target alsa_input.pci-0000_06_00.6.analog-stereo a_raw.wav & P2=$!
wait $P1; wait $P2
pw-cat -r -n 480000 --target mic_crisp b_proc.wav & P1=$!
pw-cat -r -n 480000 --target alsa_input.pci-0000_06_00.6.analog-stereo b_raw.wav & P2=$!
sleep 0.8
pw-cat -p --target alsa_output.pci-0000_06_00.6.analog-stereo --volume 0.55 speechish.wav
wait $P1; wait $P2
[ -n "$ZID" ] && pactl set-sink-input-mute "$ZID" 0 && echo "Zen restored"
echo DONE
