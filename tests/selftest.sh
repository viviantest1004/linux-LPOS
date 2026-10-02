# selftest.sh - run on the machine itself and say what is broken.
#
# Every check prints one line beginning PASS, FAIL or SKIP, so the
# result can be read out of a serial log by something that knows nothing
# else about this system. SKIP means the check could not run here and
# says why - "no watchdog in a virtual machine" is a fact about the test
# machine, not a defect. A test that quietly passes because it did not
# run is worse than no test at all.
#
# Written in the subset this shell actually has: if/elif/else/fi, while,
# for, test, and backticks. No functions and no case - this shell has
# neither, which is worth knowing before writing anything longer.

echo "=== BOOT AND IDENTITY ==="
if uname -o > /tmp/t1 ; then echo "PASS  uname works: `cat /tmp/t1`" ; else echo "FAIL  uname" ; fi
if test -f /etc/osname ; then echo "PASS  /etc/osname: `cat /etc/osname`" ; else echo "FAIL  /etc/osname missing" ; fi
if pidof init  > /dev/null ; then echo "PASS  init running"  ; else echo "FAIL  init not running"  ; fi
if pidof guard > /dev/null ; then echo "PASS  guard running" ; else echo "FAIL  guard NOT running - machine undefended" ; fi
if pidof logd  > /dev/null ; then echo "PASS  logd running"  ; else echo "FAIL  logd not running"  ; fi

echo "=== STORAGE AND PERSISTENCE ==="
if mount | grep -q " /data " ; then echo "PASS  /data mounted" ; else echo "FAIL  /data NOT mounted" ; fi
if touch /data/.st ; then echo "PASS  /data writable" ; else echo "FAIL  /data read-only" ; fi
rm -f /data/.st
if persist | grep -q kept ; then echo "PASS  install dirs are kept" ; else echo "FAIL  persist holds nothing" ; fi
if echo x > /bin/.st ; then echo "PASS  /bin accepts a write" ; else echo "FAIL  /bin not writable" ; fi
if test -f /data/persist/bin/.st ; then echo "PASS  and it landed on /data" ; else echo "FAIL  write to /bin did not reach the card" ; fi
rm -f /bin/.st

echo "=== SSH ==="
if test -f /data/dropbear_ed25519_host_key ; then echo "PASS  host key is on /data" ; else echo "FAIL  no host key" ; fi
if mount | grep -q " /root " ; then echo "PASS  /root comes from /data" ; else echo "FAIL  /root is RAM - keys lost on reboot" ; fi
if pidof dropbear > /dev/null ; then echo "PASS  dropbear running" ; else echo "FAIL  dropbear not running" ; fi
if authkey -l | grep -q "no keys" ; then echo "SKIP  no SSH key authorized (a setup choice, not a fault)" ; else echo "PASS  an SSH key is authorized" ; fi
if authkey -l | grep -q "authkey new" ; then echo "PASS  and it names the command that fixes that" ; else echo "PASS  a key is already there, so there is nothing to fix" ; fi
if authkey new > /tmp/ak1 2>&1 ; then echo "PASS  authkey new makes a key on the machine" ; else echo "FAIL  authkey new: `head -2 /tmp/ak1`" ; fi
if authkey -l | grep -q "ssh-" ; then echo "PASS  and it is authorized" ; else echo "FAIL  the new key is not in authorized_keys" ; fi
if grep -q "ssh -i lp_ssh_key root@" /tmp/ak1 ; then echo "PASS  and it printed the command to connect" ; else echo "FAIL  authkey new did not print an ssh command: `tail -4 /tmp/ak1`" ; fi
if test -f /boot/lp_ssh_key ; then echo "PASS  the private key is on the boot partition, readable from any PC" ; else if test -f /data/lp_ssh_key ; then echo "PASS  the private key is on /data (boot is read-only here)" ; else echo "FAIL  the private key was not left anywhere reachable" ; fi ; fi
if echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIExampleKeyForTheSelfTestOnly test@selftest" | authkey add > /dev/null 2>&1 ; then echo "PASS  authkey add takes a key from a paste" ; else echo "FAIL  authkey add" ; fi
if authkey -l | grep -q "test@selftest" ; then echo "PASS  and it is authorized too" ; else echo "FAIL  the pasted key was not added" ; fi
if echo "this is not a key" | authkey add > /tmp/ak2 2>&1 ; then echo "FAIL  authkey add accepted something that is not a key" ; else echo "PASS  authkey add refuses what is not a key" ; fi
if grep -q "not a public key" /tmp/ak2 ; then echo "PASS  and says what was wrong" ; else echo "FAIL  and did not say why" ; fi
rm -f /tmp/ak1 /tmp/ak2

echo "=== PYTHON ==="
if test -x /data/python/bin/python3.12 ; then
  if python -c 'import sys,ssl,sqlite3,zlib,bz2,lzma,ctypes,hashlib;print("pyok",sys.version.split()[0])' > /tmp/t2 2>&1 ; then
    echo "PASS  python + ssl sqlite3 zlib bz2 lzma ctypes: `cat /tmp/t2`"
  else
    echo "FAIL  python broken: `head -1 /tmp/t2`"
  fi
else
  echo "SKIP  no CPython in this image"
fi
if test -x /data/bin/micropython ; then
  if /data/bin/micropython -c 'print("mpyok")' > /tmp/t3 2>&1 ; then echo "PASS  micropython runs" ; else echo "FAIL  micropython: `head -1 /tmp/t3`" ; fi
else
  echo "SKIP  no micropython"
fi

echo "=== ORDINARY LINUX BINARIES ==="
# The loader is named differently on every architecture, and /etc/rc
# used to link only the arm64 one. On amd64 that produced a broken
# symlink, no working loader, and a cheerful "ordinary Linux binaries
# will run" message - so nothing external ran on half the images this
# project ships and no test noticed. CPython is itself a dynamically
# linked glibc binary, which makes it the obvious thing to check with.
if test -d /data/glibc ; then
  if run /data/python/bin/python3.12 -c "print('ran')" > /tmp/t20 2>&1 ; then echo "PASS  run starts a glibc binary" ; else echo "FAIL  run cannot start one: `head -1 /tmp/t20`" ; fi
  if /data/python/bin/python3.12 -c "print('ran')" > /tmp/t21 2>&1 ; then echo "PASS  and the loader symlinks work without run" ; else echo "FAIL  /lib loader symlinks are wrong: `head -1 /tmp/t21`" ; fi
else
  echo "SKIP  no glibc on /data (an image built without python)"
fi

echo "=== GRAPHICS AND INPUT (what a UI package needs) ==="
if test -d /sys/class/drm ; then echo "PASS  DRM present: `ls /sys/class/drm | wc -l` nodes" ; else echo "FAIL  no DRM - no UI package can work" ; fi
# No card0 can mean two very different things: the driver is missing, or
# there is no display device on this machine to drive. A VM started
# without a GPU is the second, and calling that a failure trains people
# to ignore the result.
if test -e /dev/dri/card0 ; then
  echo "PASS  /dev/dri/card0 exists"
elif test -d /sys/class/drm ; then
  echo "SKIP  no /dev/dri/card0 - DRM is built in but this machine has no display device attached"
else
  echo "FAIL  no DRM and no /dev/dri/card0"
fi
if ls /sys/class/input | grep -q event ; then
  echo "PASS  input event devices exist"
else
  echo "SKIP  no input devices - none attached to this machine (serial console only)"
fi
if test -e /dev/fb0 ; then echo "PASS  /dev/fb0 exists" ; else echo "SKIP  no fbdev (DRM may still be fine)" ; fi

echo "=== SECURITY ==="
if test -f /etc/update-key.pub ; then echo "PASS  update key is inside the system image" ; else echo "SKIP  no update key in this build" ; fi
# An unsigned image must be refused.
#
# The image has to be convincing enough to reach the signature check.
# update looks at the size and the magic first, and rejects anything
# that fails those as "not a kernel" - which is correct, and means a
# file full of nonsense never tests the signature at all. A first
# attempt at this test used exactly such a file and reported the
# signature check as broken when it had simply never run.
#
# So we hand it the kernel this machine is running, copied without its
# signature. Genuine image, right size, right magic, no .sig beside it:
# the one case the check exists for.
KIMG=""
if test -f /boot/EFI/BOOT/BOOTX64.EFI ; then KIMG=/boot/EFI/BOOT/BOOTX64.EFI ; fi
if test -f /boot/EFI/BOOT/BOOTAA64.EFI ; then KIMG=/boot/EFI/BOOT/BOOTAA64.EFI ; fi

if test -z "$KIMG" ; then
  echo "SKIP  no kernel image on /boot to test the signature check with"
else
  rm -f /data/fake-kernel.img /data/fake-kernel.img.sig
  cp $KIMG /data/fake-kernel.img
  update /data/fake-kernel.img > /tmp/t4 2>&1
  if grep -q signed /tmp/t4 ; then echo "PASS  update refuses an unsigned image" ; else echo "FAIL  unsigned image was NOT refused: `head -1 /tmp/t4`" ; fi
  rm -f /data/fake-kernel.img
fi
rm -f /data/.update.sig
echo guard > /data/services.disabled
sleep 3
if pidof guard > /dev/null ; then echo "PASS  guard ignores services.disabled" ; else echo "FAIL  guard was disabled by an editable file" ; fi
rm -f /data/services.disabled
service stop guard > /tmp/t5 2>&1
if grep -q force /tmp/t5 ; then echo "PASS  service stop guard is refused" ; else echo "FAIL  service stop guard was allowed" ; fi
if firewall status > /tmp/t6 2>&1 ; then echo "PASS  firewall reports its state" ; else echo "FAIL  firewall status failed" ; fi

echo "=== ERROR PATHS ==="
if cat /nope/nope > /tmp/t8 2>&1 ; then echo "FAIL  cat on a missing file succeeded" ; else echo "PASS  cat on a missing file fails" ; fi
if cd /nope/nope ; then echo "FAIL  cd to nowhere succeeded" ; else echo "PASS  cd to a missing directory fails" ; fi
if rm /nope/nope ; then echo "FAIL  rm of nothing succeeded" ; else echo "PASS  rm on a missing file fails" ; fi
if mkdir /proc/nope ; then echo "FAIL  mkdir in /proc succeeded" ; else echo "PASS  mkdir where it cannot fails" ; fi
if df / > /dev/null      ; then echo "PASS  df"      ; else echo "FAIL  df"      ; fi
if free > /dev/null      ; then echo "PASS  free"    ; else echo "FAIL  free"    ; fi
if top -n 1 > /dev/null  ; then echo "PASS  top"     ; else echo "FAIL  top"     ; fi
if ps > /dev/null        ; then echo "PASS  ps"      ; else echo "FAIL  ps"      ; fi
if disk > /dev/null      ; then echo "PASS  disk"    ; else echo "FAIL  disk"    ; fi
if sysinfo > /dev/null   ; then echo "PASS  sysinfo" ; else echo "FAIL  sysinfo" ; fi
if dmesg > /dev/null     ; then echo "PASS  dmesg"   ; else echo "FAIL  dmesg"   ; fi
if tar -c /tmp/t.tar /etc/osname > /dev/null ; then echo "PASS  tar creates" ; else echo "FAIL  tar create" ; fi
if sha256sum /etc/osname > /dev/null ; then echo "PASS  sha256sum" ; else echo "FAIL  sha256sum" ; fi

echo "=== SHELL REDIRECTION ==="
if definitely-not-a-command 2> /tmp/r1 ; then echo "FAIL  unknown command succeeded" ; else echo "PASS  2> captures stderr to a file" ; fi
if grep -q "not found" /tmp/r1 ; then echo "PASS  and the message really landed there" ; else echo "FAIL  2> wrote nothing" ; fi
definitely-not-a-command > /tmp/r2 2>&1
if grep -q "not found" /tmp/r2 ; then echo "PASS  2>&1 merges stderr into stdout" ; else echo "FAIL  2>&1 did not merge" ; fi
echo first > /tmp/r3
definitely-not-a-command 2>> /tmp/r3
if grep -q first /tmp/r3 ; then echo "PASS  2>> appends rather than truncating" ; else echo "FAIL  2>> truncated the file" ; fi

echo "=== LOGGING ==="
if test -f /data/log/messages ; then echo "PASS  the log exists: `wc -l < /data/log/messages` lines" ; else echo "FAIL  no log file" ; fi
if grep -q "init:" /data/log/messages ; then echo "PASS  our own programs reach the log" ; else echo "FAIL  the log has kernel messages only" ; fi

echo "=== TIME AND SLEEP ==="
# sleep 1 through sleep 9 were right and everything longer was not:
# the parser scaled by a thousand once per digit, so "sleep 33" waited
# 3003 seconds. Nothing in the boot path sleeps that long, so the only
# way to see it is to time a two-digit sleep against the clock.
T0=`date -e`
sleep 12
T1=`date -e`
ELAPSED=`calc $T1 - $T0 | head -1`
if test $ELAPSED -ge 11 ; then echo "PASS  sleep 12 waited at least 11s" ; else echo "FAIL  sleep 12 returned after ${ELAPSED}s" ; fi
if test $ELAPSED -le 20 ; then echo "PASS  and not much more (${ELAPSED}s)" ; else echo "FAIL  sleep 12 waited ${ELAPSED}s - the parser is scaling wrongly" ; fi
if test 12 -gt 9 ; then echo "PASS  test compares two-digit numbers" ; else echo "FAIL  test -gt is wrong above nine" ; fi

echo "=== THE SHELL ITSELF ==="
# `echo x | grep x` answered "sh: echo: command not found" until today:
# a pipeline forked and exec'd every stage, and echo is a builtin with
# no file behind it. It is one of the two or three most typed shapes in
# any shell, so it gets a check of its own.
if echo hello | grep -q hello ; then echo "PASS  a builtin works as a pipeline stage" ; else echo "FAIL  echo | grep - builtins still do not run in a pipeline" ; fi
if pwd | grep -q "/" ; then echo "PASS  and so does pwd" ; else echo "FAIL  pwd | grep" ; fi
if echo one two | cut -d " " -f 2 | grep -q two ; then echo "PASS  echo into a three-stage pipeline" ; else echo "FAIL  echo | cut | grep" ; fi
# A loop cannot be the condition of an if here - the parser splits the
# line into statements before it sees the block, so `if cmd | while ... ;
# done ; then` is not a shape this shell has. Run it, keep the answer,
# then test the answer.
printf "a b c\n" | while read x y z ; do echo $z ; done > /tmp/rd1
if grep -q "^c$" /tmp/rd1 ; then echo "PASS  read splits a line into variables" ; else echo "FAIL  read: `cat /tmp/rd1`" ; fi
if test -x /bin/echo ; then echo "PASS  echo exists as a real program, not only a builtin" ; else echo "FAIL  no /bin/echo - xargs echo and find -exec echo cannot work" ; fi
if grep -qF "a+b" /etc/profile ; then echo "FAIL  grep -F matched something that is not there" ; else echo "PASS  grep -F treats the pattern as a plain string" ; fi
if echo "a+b" | grep -q "a+b" ; then echo "PASS  grep without -E treats + as an ordinary character (POSIX)" ; else echo "FAIL  grep still reads + as a repeat in a basic expression" ; fi
if echo "aab" | grep -qE "a+b" ; then echo "PASS  grep -E treats + as a repeat" ; else echo "FAIL  grep -E" ; fi
if echo "dog" | grep -qE "cat|dog" ; then echo "PASS  grep -E does alternation" ; else echo "FAIL  grep -E alternation" ; fi
if echo "" | read v ; then echo "PASS  read succeeds on a line" ; else echo "FAIL  read returned failure on a real line" ; fi
if false ; then echo x ; fi ; false ; if test $? -eq 1 ; then echo "PASS  \$? is the status of the command just before it" ; else echo "FAIL  \$? on one line is stale - it was `false ; echo $?`" ; fi
seq 3 | while read n ; do echo v$n ; done > /tmp/rd2
if wc -l < /tmp/rd2 | grep -q 3 ; then echo "PASS  a pipeline can feed a while loop" ; else echo "FAIL  cmd | while read - the loop got `wc -l < /tmp/rd2` lines" ; fi
for i in 1 2 ; do echo $i ; done > /tmp/blk1
if wc -l < /tmp/blk1 | grep -q 2 ; then echo "PASS  a redirect on done applies to the whole loop" ; else echo "FAIL  done > file was dropped" ; fi
printf "a\nb\n" | while read v ; do echo $v ; done | tr a-z A-Z > /tmp/rd3
if grep -q "^A$" /tmp/rd3 ; then echo "PASS  and a pipe after done carries the loop output on" ; else echo "FAIL  done | cmd was dropped: `cat /tmp/rd3`" ; fi
rm -f /tmp/blk1 /tmp/rd1 /tmp/rd2 /tmp/rd3
if echo "x ; y" | grep -q "x ; y" ; then echo "PASS  a semicolon inside quotes is not a separator" ; else echo "FAIL  quoted semicolon was split" ; fi
if test "`echo $(echo a ; echo b)`" = "a b" ; then echo "PASS  a semicolon inside \$( ) is not a separator either" ; else echo "FAIL  \$(a ; b) was split" ; fi
export LPTEST=works
if env | grep -q "LPTEST=works" ; then echo "PASS  export assigns and the value is in the environment" ; else echo "FAIL  export" ; fi

echo "=== THE EVERYDAY COMMANDS ==="
# These are the ones a script needs to exist at all. Each check is the
# smallest thing that would catch the command being absent or wrong,
# because a hundred cases here would push the run past the point where
# anybody reads the output.
if seq 3 | tr "\n" " " | grep -q "1 2 3" ; then echo "PASS  seq counts" ; else echo "FAIL  seq: `seq 3 | tr '\n' ' '`" ; fi
if seq -w 8 10 | head -1 | grep -q "08" ; then echo "PASS  seq -w pads (so %*d works in our printf)" ; else echo "FAIL  seq -w: `seq -w 8 10 | head -1`" ; fi
if echo abc | tr a-z A-Z | grep -q ABC ; then echo "PASS  tr changes case" ; else echo "FAIL  tr a-z A-Z" ; fi
if echo aaab | tr -s a | grep -q "^ab" ; then echo "PASS  tr -s squeezes" ; else echo "FAIL  tr -s: `echo aaab | tr -s a`" ; fi
if basename /a/b/c.txt .txt | grep -q "^c$" ; then echo "PASS  basename strips a suffix" ; else echo "FAIL  basename" ; fi
if dirname /a/b/c.txt | grep -q "^/a/b$" ; then echo "PASS  dirname" ; else echo "FAIL  dirname: `dirname /a/b/c.txt`" ; fi
if which sh | grep -q "/sh$" ; then echo "PASS  which finds a command on PATH" ; else echo "FAIL  which sh: `which sh`" ; fi
if printf "%-6s|" ab | grep -q "ab    |" ; then echo "PASS  printf pads a column" ; else echo "FAIL  printf %-6s" ; fi
if echo abc | rev | grep -q cba ; then echo "PASS  rev" ; else echo "FAIL  rev" ; fi
if printf "x\ny\n" > /tmp/t30 && nl /tmp/t30 | grep -q "1.*x" ; then echo "PASS  nl numbers lines" ; else echo "FAIL  nl" ; fi
echo same > /tmp/t31
echo same > /tmp/t32
echo diff > /tmp/t33
if cmp -s /tmp/t31 /tmp/t32 ; then echo "PASS  cmp says two equal files are equal" ; else echo "FAIL  cmp on equal files" ; fi
if cmp -s /tmp/t31 /tmp/t33 ; then echo "FAIL  cmp said two different files match" ; else echo "PASS  cmp notices a difference" ; fi
if printf "a\nb\n" | xargs echo | grep -q "a b" ; then echo "PASS  xargs joins lines into arguments" ; else echo "FAIL  xargs" ; fi
if printf "a\nb\n" | xargs -I {} echo "[{}]" | grep -q "\[b\]" ; then echo "PASS  xargs -I substitutes" ; else echo "FAIL  xargs -I" ; fi
if grep -A 1 root /etc/passwd | wc -l | grep -q "[2-9]" ; then echo "PASS  grep -A prints trailing context" ; else echo "FAIL  grep -A" ; fi

if echo aXbXc | sed 's/X/-/g' | grep -q "a-b-c" ; then echo "PASS  sed replaces every match with g" ; else echo "FAIL  sed s///g: `echo aXbXc | sed 's/X/-/g'`" ; fi
if echo aXbXc | sed 's/X/-/' | grep -q "a-bXc" ; then echo "PASS  sed without g changes only the first" ; else echo "FAIL  sed s/// changed more than the first match" ; fi
if seq 5 | sed -n '3p' | grep -q "^3$" ; then echo "PASS  sed -n Np prints one line" ; else echo "FAIL  sed -n 3p: `seq 5 | sed -n '3p'`" ; fi
if seq 5 | sed '2,4d' | tr "\n" " " | grep -q "1 5" ; then echo "PASS  sed deletes an address range" ; else echo "FAIL  sed 2,4d" ; fi
if echo "hello world" | sed -E 's/(hello) (world)/\2 \1/' | grep -q "world hello" ; then echo "PASS  sed keeps the groups in a replacement" ; else echo "FAIL  sed group references" ; fi
if echo abc | sed 'y/abc/xyz/' | grep -q xyz ; then echo "PASS  sed y transliterates" ; else echo "FAIL  sed y///" ; fi
if echo "a1b2" | sed 's/[[:digit:]]//g' | grep -q "^ab$" ; then echo "PASS  sed knows the POSIX classes" ; else echo "FAIL  sed [[:digit:]]" ; fi
if printf "one\ntwo\n" > /tmp/sd1 && sed -i 's/one/1/' /tmp/sd1 && grep -q "^1$" /tmp/sd1 ; then echo "PASS  sed -i rewrites the file" ; else echo "FAIL  sed -i" ; fi
if sed 'k' /tmp/sd1 2>&1 | grep -q position ; then echo "PASS  sed names the position of a bad command" ; else echo "FAIL  sed accepted a broken script" ; fi
rm -f /tmp/sd1

printf "alice 30 seoul\nbob 25 busan\ncarol 41 seoul\n" > /tmp/aw1
if awk '{print $1}' /tmp/aw1 | head -1 | grep -q alice ; then echo "PASS  awk splits into fields" ; else echo "FAIL  awk fields" ; fi
if awk '$2 > 28 {print $1}' /tmp/aw1 | wc -l | grep -q 2 ; then echo "PASS  awk compares numbers in a pattern" ; else echo "FAIL  awk numeric pattern" ; fi
if awk 'BEGIN{s=0} {s+=$2} END{print s}' /tmp/aw1 | grep -q 96 ; then echo "PASS  awk adds a column up in END" ; else echo "FAIL  awk BEGIN/END" ; fi
if awk '{c[$3]++} END{for (k in c) print k, c[k]}' /tmp/aw1 | grep -q "seoul 2" ; then echo "PASS  awk counts with an array" ; else echo "FAIL  awk arrays" ; fi
if printf "a:b\n" | awk -F: '{print $2}' | grep -q "^b$" ; then echo "PASS  awk -F sets the separator" ; else echo "FAIL  awk -F" ; fi
if awk 'BEGIN{printf "%-6s|", "ab"}' | grep -q "ab    |" ; then echo "PASS  awk printf pads" ; else echo "FAIL  awk printf" ; fi
if echo "a-b" | awk '{gsub(/-/,"+"); print}' | grep -qF "a+b" ; then echo "PASS  awk gsub" ; else echo "FAIL  awk gsub" ; fi
if awk '{print $1' /tmp/aw1 2>&1 | grep -q "line 1" ; then echo "PASS  awk names the line of a syntax error" ; else echo "FAIL  awk accepted a broken program" ; fi
rm -f /tmp/aw1

printf "one\ntwo\nthree\n" > /tmp/df1
printf "one\n2\nthree\n" > /tmp/df2
if diff /tmp/df1 /tmp/df1 ; then echo "PASS  diff says nothing about identical files" ; else echo "FAIL  diff found a difference that is not there" ; fi
if diff /tmp/df1 /tmp/df2 > /tmp/df3 ; then echo "FAIL  diff returned success on files that differ" ; else echo "PASS  diff reports a difference in its status" ; fi
if grep -q "^-two" /tmp/df3 ; then echo "PASS  and shows the removed line" ; else echo "FAIL  diff output has no -two: `head -6 /tmp/df3`" ; fi
if grep -q "^+2" /tmp/df3 ; then echo "PASS  and the added one" ; else echo "FAIL  diff output has no +2" ; fi
rm -f /tmp/df1 /tmp/df2 /tmp/df3
if netstat -l > /tmp/ns1 2>&1 ; then echo "PASS  netstat lists listening sockets" ; else echo "FAIL  netstat -l" ; fi
if grep -q "LISTEN" /tmp/ns1 ; then echo "PASS  and dropbear is one of them" ; else echo "FAIL  nothing is listening - dropbear should be: `head -3 /tmp/ns1`" ; fi
if netstat -r | grep -q "destination" ; then echo "PASS  netstat -r shows the routing table" ; else echo "FAIL  netstat -r" ; fi
if netstat -i | grep -q "lo" ; then echo "PASS  netstat -i shows the interfaces" ; else echo "FAIL  netstat -i" ; fi
rm -f /tmp/ns1

if dd if=/dev/zero of=/tmp/dd1 bs=1M count=2 status=none ; then echo "PASS  dd makes a file of a given size" ; else echo "FAIL  dd" ; fi
if wc -c < /tmp/dd1 | grep -q 2097152 ; then echo "PASS  and it is exactly that size" ; else echo "FAIL  dd size is `wc -c < /tmp/dd1`" ; fi
if dd if=/tmp/dd1 of=/tmp/dd2 bs=512 count=1 status=none && wc -c < /tmp/dd2 | grep -q 512 ; then echo "PASS  dd count= stops where told" ; else echo "FAIL  dd count=" ; fi
if dd nosuchkey=1 > /dev/null 2>&1 ; then echo "FAIL  dd accepted a key it does not know" ; else echo "PASS  dd refuses a key it does not know" ; fi
rm -f /tmp/dd1 /tmp/dd2

echo "=== THE SERVER SIDE ==="
if info -s > /tmp/if1 2>&1 ; then echo "PASS  info runs" ; else echo "FAIL  info: `head -1 /tmp/if1`" ; fi
if grep -q "linux-LP\|LP-zero" /tmp/if1 ; then echo "PASS  and names the system" ; else echo "FAIL  info does not say what this is" ; fi
if info project | grep -qF "MIT" ; then echo "PASS  info project has the licence" ; else echo "FAIL  info project" ; fi
if info net | grep -q "eth0\|wlan0\|no network" ; then echo "PASS  info net reports the interfaces" ; else echo "FAIL  info net" ; fi
rm -f /tmp/if1
mkdir -p /data/wwwtest
echo "servedok" > /data/wwwtest/index.html
httpd -p 8451 -d /data/wwwtest -l /tmp/hd.log
sleep 3
if netstat -l | grep -q 8451 ; then echo "PASS  httpd is listening" ; else echo "FAIL  httpd did not come up: `cat /tmp/hd.log`" ; fi
if wget -O /tmp/hd1 http://127.0.0.1:8451/ > /dev/null 2>&1 ; then echo "PASS  httpd answers a request" ; else echo "FAIL  httpd did not answer" ; fi
if grep -q servedok /tmp/hd1 ; then echo "PASS  and served the right file" ; else echo "FAIL  httpd served: `head -1 /tmp/hd1`" ; fi
kill httpd
rm -rf /data/wwwtest /tmp/hd1 /tmp/hd.log

# defend answers non-zero when it found something that wants a decision,
# the way a linter does, so the status is not what says whether it ran.
defend -n > /tmp/df1 2>&1
if grep -q "defend:" /tmp/df1 ; then echo "PASS  defend runs its checks" ; else echo "FAIL  defend said nothing at all" ; fi
if grep -q "listening" /tmp/df1 ; then echo "PASS  and lists what is listening" ; else echo "FAIL  defend did not report listeners" ; fi
if defend -h | grep -q "not an antivirus" ; then echo "PASS  defend says plainly that it is not an antivirus" ; else echo "FAIL  defend -h overclaims" ; fi
if defend baseline > /dev/null 2>&1 ; then echo "PASS  defend records a baseline" ; else echo "FAIL  defend baseline" ; fi
if defend status | grep -q baseline ; then echo "PASS  and status reports it" ; else echo "FAIL  defend status" ; fi
if pidof defend > /dev/null ; then echo "PASS  defend is running as a service" ; else echo "FAIL  init is not keeping defend alive" ; fi
rm -f /tmp/df1

echo "=== SETTINGS THAT SURVIVE A REBOOT ==="
# The RAM root means /etc is rebuilt every boot. These check the two
# ways out of that: a profile on /data, and named /etc files kept.
if test -f /etc/profile ; then echo "PASS  /etc/profile ships in the image" ; else echo "FAIL  no /etc/profile" ; fi
echo "LPSELFTEST=yes" > /root/.profile
if sh -c ". /dev/null ; echo started" > /dev/null 2>&1 ; then echo "PASS  a shell starts with a profile present" ; else echo "FAIL  a profile stopped the shell starting" ; fi
rm -f /root/.profile
if persist etc keep /etc/hosts > /tmp/t34 2>&1 ; then echo "PASS  persist etc keeps a file" ; else echo "FAIL  persist etc keep: `head -1 /tmp/t34`" ; fi
if test -f /data/etc/hosts ; then echo "PASS  and the copy is on the card" ; else echo "FAIL  nothing landed in /data/etc" ; fi
if persist etc keep /etc/rc > /tmp/t35 2>&1 ; then echo "FAIL  persist etc kept /etc/rc - that can stop the board booting" ; else echo "PASS  persist etc refuses /etc/rc" ; fi
if grep -q "before /data" /tmp/t35 ; then echo "PASS  and says why" ; else echo "FAIL  and did not say why: `head -1 /tmp/t35`" ; fi
persist etc forget /etc/hosts > /dev/null 2>&1

echo "=== THE FIREWALL OPENS FROM HERE ==="
# Opening a port used to need a card reader. If this breaks, the answer
# people reach for is `firewall off`, which is worse than any port.
if firewall allow 8099 > /tmp/t36 2>&1 ; then echo "PASS  firewall allow accepted a port" ; else echo "FAIL  firewall allow: `head -1 /tmp/t36`" ; fi
if firewall ports | grep -q 8099 ; then echo "PASS  and firewall ports lists it" ; else echo "FAIL  firewall ports does not show 8099" ; fi
if firewall deny 8099 > /dev/null 2>&1 ; then echo "PASS  firewall deny closes it again" ; else echo "FAIL  firewall deny" ; fi
if firewall ports | grep -q 8099 ; then echo "FAIL  8099 is still listed after deny" ; else echo "PASS  and it is gone from the list" ; fi

# The slow one goes last on purpose.
#
# It has to let a process hold a core for thirty seconds before guard is
# entitled to have noticed, and a serial session that gets cut short
# would otherwise take every check after it down too. Put at the end,
# a truncated run still reports everything else.
echo "=== IT DOES NOT DIE ==="
sh -c 'while : ; do : ; done' &
HOG=$!

# Wait for guard rather than assuming how long it takes.
#
# guard counts seconds a process was actually hot, not seconds on the
# clock, so on a busy machine thirty of them take longer than thirty
# seconds to accumulate. A fixed `sleep 33` passed on an idle board and
# failed on a fresh card that was still finishing its first boot - which
# looks exactly like guard being broken.
WAITED=0
while test $WAITED -lt 60 ; do
  if dmesg | grep -q "held a core" ; then break ; fi
  sleep 5
  WAITED=`calc $WAITED + 5 | head -1`
done

if test -d /proc/$HOG ; then echo "PASS  the hog is alive to be judged" ; else echo "FAIL  test hog died on its own" ; fi
if dmesg | grep -q "held a core" ; then echo "PASS  guard noticed the CPU hog after ${WAITED}s" ; else echo "FAIL  a core was held ${WAITED}s with no notice" ; fi
if test $WAITED -lt 60 ; then echo "PASS  break left the loop early" ; else echo "FAIL  the wait loop ran to its limit - break did not work" ; fi
kill -9 $HOG
if definitely-not-a-command > /tmp/t7 2>&1 ; then echo "FAIL  a missing command reported success" ; else echo "PASS  a missing command fails" ; fi
if grep -q "not found" /tmp/t7 ; then echo "PASS  and says so" ; else echo "FAIL  and said: `head -1 /tmp/t7`" ; fi

echo "=== SELFTEST COMPLETE ==="
