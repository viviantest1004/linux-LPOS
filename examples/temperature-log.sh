# temperature-log.sh - write the board's temperature to a CSV, forever.
#
# Copy to /data/rc.local and reboot. It runs in the background from then
# on, including after every power cut, with nothing to maintain.
#
# The file is on /data, which survives a reboot, and guard drops the
# oldest rotated log first when that partition fills - so this cannot
# fill the card and stop the machine.
#
#   tail -f /data/log/temperature.csv
#   scp root@<board>:/data/log/temperature.csv .

LOG=/data/log/temperature.csv
EVERY=60

mkdir -p /data/log

# A header, but only once - appending one at every boot would put a row
# of column names in the middle of a year of data.
if test -f $LOG
then
    echo "[temp] appending to $LOG"
else
    echo "time,uptime_s,temp_c,load,mem_available_kb" > $LOG
fi

reading() {
    # The thermal zone is in thousandths of a degree. On a virtual
    # machine there is no sensor and this file does not exist, which is
    # why the fallback is a dash rather than a zero - a zero would read
    # as "very cold" in the data six months from now.
    T=$(cat /sys/class/thermal/thermal_zone0/temp)
    if test -z "$T"
    then
        T=-
    else
        T=$(calc "$T / 1000")
    fi

    UP=$(cut -w -f 1 /proc/uptime)
    LOAD=$(cut -w -f 1 /proc/loadavg)
    # -w splits on runs of whitespace, which is what /proc gives -
    # "MemAvailable:   440404 kB" cut on a single space is padding.
    FREE=$(grep MemAvailable /proc/meminfo | cut -w -f 2)

    # date -e is seconds since 1970, which sorts and plots without
    # anything having to parse it.
    echo "$(date -e),$UP,$T,$LOAD,$FREE" >> $LOG
}

logger() {
    while true
    do
        reading
        sleep $EVERY
    done
}

logger &
echo "[temp] logging to $LOG every ${EVERY}s"
