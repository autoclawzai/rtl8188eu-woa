#!/usr/bin/env bash
# USB capture of TP-Link TL-WN722N v3 (2357:010c) on Linux (Raspberry Pi OS etc.)
# Usage:
#   sudo ./linux_capture.sh "Asteroid"              # open network
#   sudo ./linux_capture.sh "MyWifi" "password123"  # WPA2
# Output: ~/pcap_<mode>_<time>.tar.gz  (pcap + dmesg + info) -> isko bhej de.

set -u
if [ "$(id -u)" -ne 0 ]; then echo "sudo se chala: sudo $0 SSID [PASSWORD]"; exit 1; fi
SSID="${1:-}"; PASS="${2:-}"
if [ -z "$SSID" ]; then echo "Usage: sudo $0 SSID [PASSWORD]"; exit 1; fi
MODE=open; [ -n "$PASS" ] && MODE=wpa2
VID=2357; PID=010c
REALHOME="${SUDO_USER:+$(getent passwd "$SUDO_USER" | cut -d: -f6)}"; REALHOME="${REALHOME:-$HOME}"
TS=$(date +%H%M%S)
WD="$REALHOME/pcap_${MODE}_${TS}"
mkdir -p "$WD"; cd "$WD" || exit 1
LOG="$WD/info.txt"
say() { echo "[*] $*" | tee -a "$LOG"; }

say "1/8 packages install (tcpdump, iw, nmcli, usbutils)"
if command -v apt-get >/dev/null 2>&1; then
  apt-get update -qq >/dev/null 2>&1
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq tcpdump iw usbutils network-manager wpasupplicant iputils-ping >/dev/null 2>&1
fi
for c in tcpdump iw nmcli lsusb; do command -v $c >/dev/null || { say "ERROR: $c nahi mila"; exit 1; }; done

say "2/8 usbmon load"
modprobe usbmon 2>&1 | tee -a "$LOG"
mount -t debugfs none /sys/kernel/debug 2>/dev/null

say "3/8 adapter dhundh raha hoon ($VID:$PID)"
DEV=""
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  if [ "$(cat $d/idVendor)" = "$VID" ] && [ "$(cat $d/idProduct)" = "$PID" ]; then DEV=$(basename "$d"); break; fi
done
if [ -z "$DEV" ]; then say "ERROR: TL-WN722N v3 (2357:010c) USB me nahi mila. lsusb dekh."; lsusb | tee -a "$LOG"; exit 1; fi
BUS=$(cat /sys/bus/usb/devices/$DEV/busnum)
say "device=$DEV bus=$BUS  kernel=$(uname -r)"
{ uname -a; lsusb -t; lsusb -d $VID:$PID -v 2>/dev/null; } >> "$LOG" 2>&1

say "4/8 capture start (usbmon$BUS)"
tcpdump -i usbmon$BUS -s 0 -U -w "$WD/usb_${MODE}.pcap" >/dev/null 2>&1 &
TPID=$!
sleep 2
if ! kill -0 $TPID 2>/dev/null; then say "ERROR: tcpdump start nahi hua"; exit 1; fi

say "5/8 adapter ko re-plug (software) taaki poora init capture ho: power-on, firmware, tables"
dmesg -C 2>/dev/null
echo 0 > /sys/bus/usb/devices/$DEV/authorized; sleep 3
echo 1 > /sys/bus/usb/devices/$DEV/authorized; sleep 10

# wifi interface of this adapter
IF=""
for n in /sys/class/net/*; do
  [ -e "$n/device" ] || continue
  if readlink -f "$n/device" | grep -q "/$DEV"; then IF=$(basename "$n"); fi
done
if [ -z "$IF" ]; then
  say "ERROR: koi wlan interface nahi bana. Kernel driver is adapter ko support nahi karta lagta hai."
  say "Driver status:"; (ls -l /sys/bus/usb/devices/${DEV}:1.0/driver 2>&1; dmesg | tail -30) | tee -a "$LOG"
  say "Hint: rtl8xxxu (kernel 6.7+) ya out-of-tree rtl8188eus/rtl8188eu driver chahiye. Kernel naya kar ya driver install karke dobara chala."
  kill $TPID 2>/dev/null; exit 1
fi
say "interface=$IF  driver=$(basename $(readlink -f /sys/class/net/$IF/device/driver 2>/dev/null))"

say "6/8 connect ($MODE) SSID=$SSID"
nmcli radio wifi on >/dev/null 2>&1
nmcli dev set "$IF" managed yes >/dev/null 2>&1
ip link set "$IF" up 2>&1 | tee -a "$LOG"
sleep 2
nmcli dev wifi rescan ifname "$IF" >/dev/null 2>&1; sleep 6
nmcli -f SSID,BSSID,CHAN,SIGNAL,SECURITY dev wifi list ifname "$IF" 2>&1 | tee -a "$LOG"
nmcli connection delete id "$SSID" >/dev/null 2>&1
if [ -n "$PASS" ]; then
  nmcli dev wifi connect "$SSID" password "$PASS" ifname "$IF" 2>&1 | tee -a "$LOG"
else
  nmcli dev wifi connect "$SSID" ifname "$IF" 2>&1 | tee -a "$LOG"
fi

say "7/8 IP ka wait + ping (ye data frames capture karne ke liye zaroori hai)"
for i in $(seq 1 20); do
  IP=$(ip -4 -o addr show dev "$IF" | awk '{print $4}' | head -1)
  [ -n "$IP" ] && break; sleep 1
done
say "IP=$IP"
GW=$(ip route show dev "$IF" | awk '/default/ {print $3}' | head -1)
say "gateway=$GW"
[ -n "$GW" ] && ping -c 8 -I "$IF" "$GW" 2>&1 | tee -a "$LOG"
ping -c 5 -I "$IF" 8.8.8.8 2>&1 | tee -a "$LOG"
iw dev "$IF" link 2>&1 | tee -a "$LOG"
iw dev "$IF" station dump 2>&1 | tee -a "$LOG"

say "8/8 capture stop aur pack"
sleep 2
kill $TPID 2>/dev/null; wait $TPID 2>/dev/null
dmesg > "$WD/dmesg.txt" 2>&1
ip addr > "$WD/ipaddr.txt" 2>&1
cd "$REALHOME" || exit 1
TAR="$REALHOME/$(basename $WD).tar.gz"
tar czf "$TAR" "$(basename $WD)"
[ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER" "$TAR"
say "DONE -> $TAR  ($(du -h "$TAR" | cut -f1))"
