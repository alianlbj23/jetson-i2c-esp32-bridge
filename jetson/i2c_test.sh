#!/usr/bin/env bash
# Run this on the Jetson. Needs i2c-tools (preinstalled on JetPack):
#   sudo apt install i2c-tools
#
# Usage:  sudo ./i2c_test.sh [bus]
#   bus = the /dev/i2c-N number. If omitted, every bus is scanned for the ESP32.
#
# Jetson 40-pin header -> which I2C bus:
#   Orin Nano / Orin NX : pins 3/5 = i2c-7, pins 27/28 = i2c-1
#   Xavier NX           : pins 3/5 = i2c-8, pins 27/28 = i2c-1
#   Nano (original)     : pins 3/5 = i2c-1, pins 27/28 = i2c-0
# Check with `i2cdetect -l` if unsure.

set -u
ADDR=0x28
BUS="${1:-}"

REG_WHO_AM_I=0x00; REG_UPTIME=0x04; REG_SCRATCH=0x08; REG_MESSAGE=0x10
EXPECT_WHO_AM_I=0xa5

pass=0; fail=0
ok()   { echo "  [PASS] $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }

command -v i2cdetect >/dev/null || { echo "i2c-tools not installed: sudo apt install i2c-tools"; exit 2; }

if [[ -z "$BUS" ]]; then
    echo "No bus given, scanning all buses for $ADDR ..."
    for dev in /dev/i2c-*; do
        n="${dev#/dev/i2c-}"
        if i2cdetect -y -r "$n" 2>/dev/null | grep -qi " ${ADDR#0x} "; then
            BUS="$n"; echo "  found ESP32 at $ADDR on i2c-$n"; break
        fi
    done
    [[ -z "$BUS" ]] && { echo "ESP32 not found on any bus. Check wiring (SDA/SCL/GND) and that the ESP32 is powered."; exit 1; }
fi

echo "== 1. i2cdetect on bus $BUS =="
i2cdetect -y -r "$BUS"
if i2cdetect -y -r "$BUS" | grep -qi " ${ADDR#0x} "; then ok "ESP32 answers at $ADDR"; else bad "ESP32 not seen at $ADDR"; fi

echo "== 2. WHO_AM_I =="
v=$(i2cget -y "$BUS" $ADDR $REG_WHO_AM_I 2>&1)
if [[ "${v,,}" == "$EXPECT_WHO_AM_I" ]]; then ok "WHO_AM_I = $v"; else bad "WHO_AM_I = '$v' (expected $EXPECT_WHO_AM_I)"; fi

echo "== 3. Message string (16-byte burst read) =="
raw=$(i2ctransfer -y "$BUS" w1@$ADDR $REG_MESSAGE r16 2>&1)
# i2ctransfer prints "0x45 0x53 ..." -> strip "0x", decode hex to ASCII
msg=$(echo "$raw" | sed 's/0x//g' | tr -d ' \n' | xxd -r -p | tr -d '\0')
if [[ "$msg" == "ESP32-S3 I2C OK" ]]; then ok "message = \"$msg\""; else bad "message = \"$msg\" (raw: $raw)"; fi

echo "== 4. Scratch register write / read back =="
val=$(( RANDOM % 256 )); hex=$(printf '0x%02x' "$val")
i2cset -y "$BUS" $ADDR $REG_SCRATCH "$hex"
rb=$(i2cget -y "$BUS" $ADDR $REG_SCRATCH 2>&1)
if [[ "${rb,,}" == "$hex" ]]; then ok "wrote $hex, read back $rb"; else bad "wrote $hex, read back '$rb'"; fi

echo "== 5. Multi-byte scratch write (8 bytes) =="
i2ctransfer -y "$BUS" w9@$ADDR $REG_SCRATCH 0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88
rb=$(i2ctransfer -y "$BUS" w1@$ADDR $REG_SCRATCH r8 | tr -s ' ')
if [[ "$rb" == "0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88" ]]; then ok "burst write/read ok"; else bad "burst read = '$rb'"; fi

echo "== 6. Uptime counter increases =="
read_uptime() { # u32 little-endian ms
    local b; b=($(i2ctransfer -y "$BUS" w1@$ADDR $REG_UPTIME r4))
    echo $(( (b[3] << 24) | (b[2] << 16) | (b[1] << 8) | b[0] ))
}
u1=$(read_uptime); sleep 0.5; u2=$(read_uptime)
if (( u2 > u1 )); then ok "uptime ${u1}ms -> ${u2}ms"; else bad "uptime did not advance (${u1} -> ${u2})"; fi

echo
echo "Result: $pass passed, $fail failed"
(( fail == 0 ))
