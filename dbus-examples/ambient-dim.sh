#!/bin/bash

# === Get connection parameters ===

dpy=$(echo -n "$DISPLAY" | tr -c '[:alnum:]' _)
unbright="/dev/shm/unbright.$dpy"

if [ -z "$dpy" ]; then
    echo "Cannot find display."
    exit 1;
fi

service="com.github.chjj.compton.${dpy}"
interface="com.github.chjj.compton"

compton_dbus="dbus-send --print-reply=literal --dest="${service}" / "${interface}"."
compton_dbus_h="dbus-send --print-reply --dest="${service}" / "${interface}"."

# === Get camera device ===
if [[ -z "$1" ]] || [[ ! -c "$1" ]]; then
    echo "usage: $0 <camera device>"
    exit 1
else
    cam="$1"
fi

# === Sample parameters ===

max_dim=65
frame_ms=30
rule='!(class_g ~= "^(URxvt|mpv|Sxiv|syncterm|XTerm|stellarium|XEphem)$" || _NET_WM_WINDOW_TYPE@:32a *= "DOCK")'

# === Trap ===

trap 'kill $(jobs -p); _remove; _repaint;' EXIT

_insert() {
    ${compton_dbus}dim_rule_update string:insert string:"${1}:${rule}" 2>/dev/null
}

_remove() {
    if [ ! -z "$last_repr" ]; then
        ${compton_dbus}dim_rule_update string:remove string:"$last_repr" >/dev/null 2>&1
        if [[ $? -ne 0 ]]; then
            # Exiting during set_dim(). Fingers crossed we're removing the
            # right thing
            last_repr=$(${compton_dbus_h}opts_get string:dim-rule | \
                grep -m1 -Po '(?<=string ").*(?=")')
            _remove
        fi
    fi
}

_replace() {
    ${compton_dbus}dim_rule_update string:replace string:"$last_repr" string:"${1}:${rule}"
}

_repaint() {
    ${compton_dbus}repaint >/dev/null
}

set_dim() {
    local new
    if [[ -z "$last_repr" ]]; then
        new=$(_insert $1)
    else
        new=$(_replace $1)
    fi
    last_repr=${new:3}
    last_dim="$1"
    _repaint
}

_normalize() {
    old_min=$1
    old_max=$2
    new_min=$3
    new_max=$4
    old_val=$5
    new_val=$(( (((old_val - old_min) * (new_max - new_min)) / \
        (old_max - old_min)) + new_min ))
    if [[ $new_min -gt $new_max ]]; then
        [[ $new_val -gt $new_min ]] && new_val=$new_min
        [[ $new_val -lt $new_max ]] && new_val=$new_max
    fi
    echo $new_val
}

read_camera() {
    exec ffmpeg \
        -hide_banner \
        -f video4linux2 \
        -s 640x480 \
        -i "$cam" \
        -filter:v "fps=fps=30, showinfo" \
        -f null - > >(stdbuf -oL grep -Po '(?<=mean:\[)[0-9]*' | \
        while read -r brightness; do
            echo $(_normalize 72 184 0 100 "$brightness") > "${unbright}_"
            mv "${unbright}_" "${unbright}"
        done) 2>&1
}

init_camera() {
    [[ ! -c $cam ]] && exit 1

    # Try changing this if you don't get good results
    v4l2-ctl -d "$cam" -c exposure_auto=3 2>/dev/null
    v4l2-ctl -d "$cam" -c exposure_auto=1 2>/dev/null

    local expo=$(v4l2-ctl -d "$cam" -l | grep -m 1 exposure_absolute | grep -Po '(?<=default=)\d+')
    local gain=$(v4l2-ctl -d "$cam" -l | grep -m 1 gain | grep -Po '(?<=default=)\d+')

    if [ ! -z "$gain" ]; then
        v4l2-ctl -d "$cam" -c gain=0 2>/dev/null
        v4l2-ctl -d "$cam" -c exposure_absolute=1000 2>/dev/null
    else
        v4l2-ctl -d "$cam" -c exposure_absolute="$expo" 2>/dev/null
    fi
}

set_ddc() {
    local new="$1"
    if [[ "$new" -ne "$last_ddc" ]]; then
        if ! ps -p $ddc_pid >/dev/null 2>&1; then
            ddccontrol -r 0x10 -w "$new" dev:/dev/i2c-2 >/dev/null 2>&1 &
            ddc_pid=$!
            last_ddc="$new"
        fi
    fi
}

dim_to() {
    local new_aim="$1"
    if [[ $new_aim -gt $(( last_dim + 1 )) ]]; then
        set_dim $(( last_dim + 1 ))
    elif [[ $new_aim -lt $(( $last_dim - 1 )) ]]; then
        set_dim $(( last_dim - 1 ))
    fi
}

echo > "$unbright"
init_camera
read_camera &

last_dim=0
last_ddc=0
while :; do
    # TODO Implement frame-dropping
    start_date=$(date +%s%3N)

    brightness=$(<"$unbright")
    if [[ ! -z $brightness ]]; then

        # DIM
        dim_aim=$(_normalize 0 85 65 0 $brightness)
        dim_to "$dim_aim"

        # DDC
        if [[ $brightness -gt 50 ]]; then
            new_ddc=$(_normalize 50 100 20 100 $brightness)
        else
            new_ddc=20
        fi
        set_ddc $new_ddc

        # echo "$brightness -> ($new_ddc, $dim_aim)"
    fi

    end_date=$(date +%s%3N)
    diff_date=$(( end_date - start_date ))
    # -1 is time spent on invoking `sleep` and `printf`, kind of
    to_sleep=$(( frame_ms - diff_date - 1 ))
    [[ $to_sleep -gt 0 ]] && sleep $(printf "0.0%02d" $to_sleep)
done
