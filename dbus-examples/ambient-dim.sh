#!/bin/bash

# === Verify `compton --dbus` status ===

if [ -z "`dbus-send --session --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep compton`" ]; then
    echo "compton DBus interface unavailable"
    if [ -n "`pgrep compton`" ]; then
        echo "compton running without dbus interface"
        #killall compton & # Causes all windows to flicker away and come back ugly.
        #compton --dbus & # Causes all windows to flicker away and come back beautiful
    else
        echo "compton not running"
    fi
    exit 1;
fi

# === Get connection parameters ===

dpy=$(echo -n "$DISPLAY" | tr -c '[:alnum:]' _)

if [ -z "$dpy" ]; then
    echo "Cannot find display."
    exit 1;
fi

service="com.github.chjj.compton.${dpy}"
interface="com.github.chjj.compton"

compton_dbus="dbus-send --print-reply=literal --dest="${service}" / "${interface}"."
compton_dbus_h="dbus-send --print-reply --dest="${service}" / "${interface}"."

# === Try using flock ===

unbright="/dev/shm/unbright.$dpy"
if hash flock 2>/dev/null; then
    flock="flock ${unbright}"
else
    echo 'flock not found, running without it'
fi

# === Get camera device ===
if [[ -z "$1" ]] || [[ ! -c "$1" ]]; then
    echo "usage: $0 <camera device>"
    exit 1
else
    cam="$1"
fi

# === Sample parameters ===

max_dim=55
frame_ms=30
rule='!(class_g ~= "^(URxvt|mpv|Sxiv)$" || _NET_WM_WINDOW_TYPE@:32a *= "DOCK")'

# === Trap ===

trap '_remove; _repaint; trap - SIGTERM && kill -- -$$' SIGINT SIGTERM EXIT

_insert() {
    ${compton_dbus}dim_rule_update string:insert string:"${1}:${rule}"
}

_remove() {
    if [ ! -z "$last_repr" ]; then
        ${compton_dbus}dim_rule_update string:remove string:"$last_repr" >/dev/null 2>&1
        if [[ $? -ne 0 ]]; then
            # Exiting during replace(). Fingers crossed we're removing the
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

replace() {
    local new
    if [[ -z "$last_repr" ]]; then
        new=$(_insert $1)
    else
        new=$(_replace $1)
    fi
    last_repr=${new:3}
    _repaint
}

_normalize() {
    old_min=72
    old_max=184
    new_min=0
    new_max=100
    old_val=$1
    new_val=$(( (((old_val - old_min) * (new_max - new_min)) / \
        (old_max - old_min)) + new_min ))
    echo $new_val
}

read_camera() {
    while read -r brightness; do
        $flock echo $(_normalize "$brightness") > $unbright
    done < <(ffmpeg \
        -hide_banner \
        -f video4linux2 \
        -s 640x480 \
        -i "$cam" \
        -filter:v "fps=fps=30, showinfo" \
        -f null - 2>&1 \
        | stdbuf -oL grep -Po '(?<=mean:\[)[0-9]*')
}

init_camera() {
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

rm -f "$unbright"
init_camera
read_camera &

last_set=0
last_brightness=0
while :; do
    # TODO Implement frame-dropping
    start_date=$(date +%s%3N)

    brightness=$($flock cat "$unbright" 2>/dev/null)
    if [[ ! -z $brightness ]]; then

        new_aim=$(( $max_dim - $brightness ))
        [[ $new_aim -lt 0 ]] && new_aim=0
        [[ $new_aim -gt 100 ]] && new_aim=100

        # +1 is poor man's way of avoiding some flicker. TODO become rich
        if [[ $new_aim -gt $(( last_set + 1 )) ]]; then
            last_set=$(( last_set + 1 ))
            replace $last_set
        elif [[ $new_aim -lt $(( $last_set - 1 )) ]]; then
            last_set=$(( last_set - 1 ))
            replace $last_set
        fi
        # echo $brightness $new_aim $last_set
    fi

    last_brightness=$brightness

    end_date=$(date +%s%3N)
    diff_date=$(( end_date - start_date ))
    # -1 is time spent on invoking `sleep` and `printf`, kind of
    to_sleep=$(( frame_ms - diff_date - 1 ))
    [[ $to_sleep -gt 0 ]] && sleep $(printf "0.0%02d" $to_sleep)
done
