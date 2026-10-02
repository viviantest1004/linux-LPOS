# http-server.sh - serve a directory over HTTP.
#
# Python is on /data, so this is one line of it. The board has 480MB
# free after boot because the whole system is 22MB, so the memory is
# yours to spend.
#
# The firewall drops everything except SSH, so the port has to be opened
# on purpose. Edit /boot/firewall.conf from any PC with a card reader:
#
#   tcp 8080
#
# and reboot, or run `firewall boot` to apply it now.
#
# Then: http://<board address>:8080/

SERVE=/data/www
PORT=8080

mkdir -p $SERVE

if test -f $SERVE/index.html
then
    echo "[www] serving $SERVE"
else
    echo "<h1>It works</h1><p>Served from a Pi Zero 2 W.</p>" > $SERVE/index.html
fi

# -u so that output is not buffered - otherwise the log says nothing
# until the buffer fills, which on a quiet server is never.
python3 -u -m http.server $PORT -d $SERVE >> /data/log/www.log 2>&1 &

echo "[www] http://$(ifconfig | grep 'inet ' | head -1 | cut -w -f 2):$PORT/"
echo "[www]   open tcp $PORT in /boot/firewall.conf or nothing gets in"
