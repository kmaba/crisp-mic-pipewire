#!/bin/bash
# Keeps Buds 4 Pro awake by playing an inaudible 18kHz tone through them.
# Only targets the buds sink, never pebble_out or other outputs.

BUDS_SINK="bluez_output.41_42_0E_C5_48_67.1"
TONE="/home/kmaba/.local/share/buds-keepalive/tone.wav"

while true; do
    # Wait for buds to be connected
    if pactl list short sinks | grep -q "$BUDS_SINK"; then
        # Play tone at very low volume through buds only
        pw-cat -p --target "$BUDS_SINK" --volume 0.01 "$TONE" 2>/dev/null
    fi
    sleep 1
done
