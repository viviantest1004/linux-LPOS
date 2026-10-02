# usb-backup.sh - copy a directory to a USB stick whenever one appears.
#
# Plug a stick in and it is copied to; unplug it and nothing happens.
# Nobody has to log in.
#
# The stick has to be ext4 - this system reads FAT but only writes ext4
# reliably. `datadisk /dev/sda --format` makes one, or format it
# elsewhere. It must NOT be labelled LPZERODATA, or the boot would
# mount it as /data instead.

WHAT=/data
WHERE=/mnt/backup
EVERY=60

mkdir -p $WHERE

backup_once() {
    # -w waits for the bus to enumerate, which takes about a second
    # after a stick is plugged in.
    if mount -w /dev/sda1 $WHERE
    then
        echo "[backup] copying $WHAT"
        # Not the backups themselves, or every run copies the last one.
        cp -r -q $WHAT/log $WHERE/
        cp -r -q $WHAT/root $WHERE/
        sync
        umount $WHERE
        echo "[backup] done at $(date)"
    fi
}

watcher() {
    while true
    do
        if test -b /dev/sda1
        then
            backup_once
            # Wait for it to be taken out before doing it again, rather
            # than copying the same files every minute.
            while test -b /dev/sda1
            do
                sleep $EVERY
            done
            echo "[backup] stick removed"
        fi
        sleep $EVERY
    done
}

watcher &
echo "[backup] watching for a USB stick"
