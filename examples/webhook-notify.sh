# webhook-notify.sh - post to a URL when something changes.
#
# This one watches for the board's address changing, which is the thing
# you most want to be told about on a machine you reach over SSH: a
# router hands out a new lease, and suddenly nobody knows where it is.
#
# Put your URL below. Anything that accepts a POST works - a chat
# webhook, a monitoring service, your own endpoint. HTTPS goes through
# python3 on /data; http:// works without it.
#
# For a plain "is it alive" heartbeat, use beacon instead - it is built
# in and reports memory, temperature and undervoltage as well. This is
# for the case where you want to be told about one specific change.

URL=https://your.server/hook
STATE=/data/.last-address
EVERY=300

notify() {
    # $1 is the message. The body is JSON because that is what most
    # things accept without configuration.
    wget -q -O /dev/null "$URL" 2> /dev/null
    echo "[hook] $1"
}

address() {
    ifconfig | grep "inet " | head -1 | cut -w -f 2
}

watcher() {
    while true
    do
        NOW=$(address)
        WAS=$(cat $STATE)

        if test "$NOW" != "$WAS"
        then
            if test -n "$WAS"
            then
                notify "address changed: $WAS -> $NOW"
            else
                notify "address is $NOW"
            fi
            echo "$NOW" > $STATE
        fi

        sleep $EVERY
    done
}

watcher &
echo "[hook] watching the address, reporting to $URL"
