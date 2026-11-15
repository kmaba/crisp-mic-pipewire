#!/bin/sh
# Crisp audio auto-switch v4
#
# Rules:
#   * Buds connected  -> "Buds 4 Pro (EQ)" sink on top of the buds becomes the
#     default output (+3.5 dB @ 150 Hz, -2 dB @ 7 kHz corrective EQ). Direct BT
#     playback, no canceller loop. On every CONNECTION the EQ module is bound
#     fresh to the current buds node (they get recreated on reconnect) and:
#       buds master volume -> 100%, EQ sink volume -> 25%.
#     Manual volume changes afterwards are never overridden.
#   * Buds disconnected -> EQ module unloaded, default output = pebble_out.
#   * "Crisp Mic" (mic_crisp) always pinned as default source.
#   * If the buds ever get stuck in HFP headset profile (mono 16 kHz) while no
#     Bluetooth transport is actually active, they are rescued back to A2DP
#     stereo. If a transport IS active (e.g. you are on a real phone call with
#     the buds), they are left alone.
#
#   * Pebbles tiny-bass boost: 1 = on, 0 = off (original sound quality).
PEBBLE_BASS_ENABLE=0

# Auto-connect the buds when they are powered on but not linked to the PC
# (TWS buds only auto-reconnect to their last-used device; this makes the PC
# go get them within ~15 s). Set to 0 if you ever want them to stay off.
BUDS_AUTOCONNECT=1
BUDS_MAC="41:42:0E:C5:48:67"

bt_now() {
    pactl list short sinks 2>/dev/null | awk '$2 ~ /^bluez_(sink|output)/ {print $2; exit}'
}

bt_card() {
    pactl list short cards 2>/dev/null | awk '$2 ~ /^bluez_card/ {print $2; exit}'
}

eq_module_id() {
    pactl list short modules 2>/dev/null | awk '$2 == "module-ladspa-sink" && index($0, "sink_name=buds_eq") > 0 {print $1; exit}'
}

pebble_eq_module_id() {
    pactl list short modules 2>/dev/null | awk '$2 == "module-ladspa-sink" && index($0, "sink_name=pebble_eq") > 0 {print $1; exit}'
}

# Print the sink-input index owned by the given pulse module (the EQ's
# internal playback stream), so it can be pinned to the right master.
module_stream_id() {
    pactl list sink-inputs 2>/dev/null | awk -v mod="$1" '
        /Sink Input #/ { idx = $3; gsub("#", "", idx) }
        /Owner Module:/ { om = $3; if (om == mod) { print idx; exit } }
    '
}

pin_stream() { # $1 = module id, $2 = master sink name
    idx=$(module_stream_id "$1")
    if [ -n "$idx" ]; then
        pactl move-sink-input "$idx" "$2" >/dev/null 2>&1
    fi
}

load_eq() {
    pactl load-module module-ladspa-sink \
        sink_name=buds_eq \
        sink_properties='device.description="Buds 4 Pro (EQ)"' \
        sink_master="$1" \
        sink_input_properties="node.target=$1" \
        plugin=libcrisp_polish label=buds4pro_eq control=7.5,4.0,0.35,-2.0 >/dev/null 2>&1
    pin_stream "$(eq_module_id)" "$1"
}

any_active_transport() {
    pw-dump 2>/dev/null | python3 -c "
import json,sys
try:
    data=json.load(sys.stdin)
    for o in data:
        p=o.get('info',{}).get('props',{}) or {}
        if p.get('api.bluez5.transport.state') == 'active':
            print('yes'); break
except Exception:
    pass
"
}

# Seed so a restart while buds are already connected doesn't stomp volume.
prev_bt=$(bt_now)
attempts=0

while :; do
    bt=$(bt_now)

    # auto-connect: buds powered on but not linked -> go get them every ~16 s.
    # timeout-guarded so an absent device can never stall the loop.
    if [ "$BUDS_AUTOCONNECT" = "1" ] && [ -z "$bt" ]; then
        attempts=$((attempts + 1))
        if [ $((attempts % 4)) -eq 0 ]; then
            timeout 6 bluetoothctl connect "$BUDS_MAC" >/dev/null 2>&1
        fi
    else
        attempts=0
    fi

    if [ -n "$bt" ]; then
        # rescue from stuck HFP (only when nothing is actively using the link)
        card=$(bt_card)
        if [ -n "$card" ]; then
            prof=$(pactl list cards | awk '/bluez_card/{inb=1} inb && /Active Profile:/{print $3; exit}')
            case "$prof" in
                headset-*)
                    if [ -z "$(any_active_transport)" ]; then
                        pactl set-card-profile "$card" a2dp-sink >/dev/null 2>&1
                    fi
                ;;
            esac
        fi

        if [ "$prev_bt" != "$bt" ]; then
            # connection (re)established: fresh EQ binding + volume policy
            mod=$(eq_module_id)
            [ -n "$mod" ] && pactl unload-module "$mod"
            load_eq "$bt"
            pactl set-default-sink buds_eq
            pactl set-sink-volume "$bt" 50% 2>/dev/null
            pactl set-sink-volume buds_eq 20% 2>/dev/null
            prev_bt="$bt"
        else
            # steady state: make sure the EQ module exists and is default
            if [ -z "$(eq_module_id)" ]; then
                load_eq "$bt"
            fi
            def=$(pactl get-default-sink 2>/dev/null)
            if [ "$def" != "buds_eq" ]; then
                pactl set-default-sink buds_eq
            fi
        fi
    else
        # Pebbles path. PEBBLE_BASS_ENABLE=1 would wrap it in a tiny-bass EQ
        # sink (+2 dB @ 110 Hz, soft limited; pebble_eq -> pebble_out -> AEC
        # reference -> hardware). With the flag off, apps play straight into
        # the plain echo-cancelling sink: original sound quality, unchanged.
        if [ "$PEBBLE_BASS_ENABLE" = "1" ]; then
            mod=$(pebble_eq_module_id)
            if [ -z "$mod" ]; then
                pactl load-module module-ladspa-sink \
                    sink_name=pebble_eq \
                    sink_properties='device.description="Pebbles (Bass+)"' \
                    sink_master=pebble_out \
                    sink_input_properties='node.target=pebble_out' \
                    plugin=libcrisp_polish label=pebble_bass control=2.0 >/dev/null 2>&1
                pactl set-sink-volume pebble_eq 100% 2>/dev/null
                pin_stream "$(pebble_eq_module_id)" pebble_out
            fi

            def=$(pactl get-default-sink 2>/dev/null)
            if [ "$def" != "pebble_eq" ]; then
                pactl set-default-sink pebble_eq
            fi
        else
            mod=$(pebble_eq_module_id)
            if [ -n "$mod" ]; then
                pactl unload-module "$mod"
            fi

            def=$(pactl get-default-sink 2>/dev/null)
            if [ "$def" != "pebble_out" ]; then
                pactl set-default-sink pebble_out
            fi
        fi

        mod=$(eq_module_id)
        if [ -n "$mod" ]; then
            pactl unload-module "$mod"
        fi

        prev_bt=""
    fi

    def_src=$(pactl get-default-source 2>/dev/null)
    if [ "$def_src" != "mic_crisp" ]; then
        pactl set-default-source mic_crisp
    fi

    sleep 2
done
