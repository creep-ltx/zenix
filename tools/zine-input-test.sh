#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZINE's input for programs (docs/d29-zine.md 16.17) in FS-UAE: the test program zinput (tools/tests/zine/zinput.S)
# runs under ZINE while keys are pressed on the emulated keyboard (wtype in the headless cage rig, tools/shot.py's
# `shot key-NAME`): a, b, c, F1, the fire button of a joystick in port 0 (the left mouse button's line: FS-UAE's
# keyboard joystick, Right Ctrl) and q. The window is on a screen of its own (IntuitionBase's FirstScreen checked). zinput writes what reached it each way (a window's IDCMP through gadtools,
# console.device's reads, an input.device handler, keyboard.device, keymap.library, lowlevel.library's interrupts,
# GetKey and ReadJoyPort); the line is compared.
# The machine: the A1200 with 4 MB fast RAM (68EC020); MACH=060 the 68060 one. ZENIX_SERIAL=/tmp/name gives the run
# its own serial port, ZENIX_OUT its folder.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-zinput}; mkdir -p $OUT
CONF=fsuae/a1200-020-fast.fs-uae
[ "$MACH" = 060 ] && CONF=fsuae/zine/a1200-060fpu.fs-uae
sed 's/^joystick_port_0 = .*/joystick_port_0 = keyboard/' $CONF > $OUT/zinput.fs-uae
grep -q '^joystick_port_0' $OUT/zinput.fs-uae || echo 'joystick_port_0 = keyboard' >> $OUT/zinput.fs-uae
sed -i "s|\(\.\./\)*build/|$(realpath $BD)/|g" $OUT/zinput.fs-uae    # ../build/, and ../../build/ in fsuae/zine/'s 68060
cat > $OUT/script.txt <<'EOF'
wait zenix:/# 240
sleep 2
shot key-Shift_L
sleep 1
send zine /tests/zine/zinput; echo zinput-rc=$?\r
wait ZINE: handing the machine 60
sleep 8
shot key-a
sleep 0.5
shot key-b
sleep 0.5
shot key-c
sleep 0.5
shot key-F1
sleep 0.5
shot key-Control_R:400
sleep 1
shot key-q
wait zinput-rc= 60
sleep 1
EOF
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-zinput-serial} SHOT_LIMIT=400 python3 tools/shot.py $OUT/zinput.fs-uae $OUT/script.txt $OUT/run > $OUT/shot.out 2>&1
log=$(tr -d '\r' < $OUT/run.serial.log 2>/dev/null)
line=$(echo "$log" | grep -a '^zinput: console' | tail -1)
pass=1
chk() { if echo "$line" | grep -Eq "$1"; then echo "PASS $2"; else echo "FAIL $2: $line"; pass=0; fi; }
echo "$log" | grep -aq '^zinput-rc=0' && echo "PASS zinput ended by the q key (return code 0)" || { echo "FAIL zinput: $(echo "$log" | grep -a 'zinput\|ZINE' | tail -4)"; pass=0; }
chk 'console abc\^0~' "console.device's CMD_READ: a b c and F1 as CSI 0 ~"
chk 'vanilla abcq ' "IDCMP VANILLAKEY through GT_GetIMsg: a b c q (F1 has no character)"
chk ' raw 50 ' "IDCMP RAWKEY: F1's code 0x50"
chk ' keyboard 20 ' "keyboard.device's KBD_READEVENT: the first key, a (0x20)"
chk ' handler ([1-9][0-9]) ' "the input.device handler saw the key events (10 or more: five keys down and up)"
chk ' buttons DU ' "IDCMP MOUSEBUTTONS: SELECTDOWN and SELECTUP from port 0's button"
chk ' map A ' "keymap.library's MapRawKey: Shift+a is A"
chk ' gadtools 1 ' "gadtools: CreateContext, CreateGadgetA, FreeGadgets, CreateMenusA (user data), FreeMenus"
chk ' front 1 ' "IntuitionBase's FirstScreen: the screen shown (the window is on it)"
chk ' rows 312 ' "GetDisplayInfoData: the PAL monitor's 312 rows (DTAG_MNTR)"
chk ' lowlevel 0 23 ' "lowlevel.library: SystemControlA took its tags (0); ReadJoyPort: port 0 a mouse, port 1 a joystick"
chk ' lowlevel 0 23 20 ' "lowlevel.library's GetKey in its keyboard interrupt: the first key, a (0x20)"
chk ' lowlevel 0 23 20 ([5-9]|[1-9][0-9]+) [1-9][0-9][0-9]+ [1-9][0-9][0-9]+ ([2-9]|[1-9][0-9]+)$' "lowlevel.library: AddKBInt (5 or more key events), AddVBlankInt and a 20 ms AddTimerInt (100 or more each), ElapsedTime (2 s or more)"
echo "$log" | grep -aq '^ZINE: zinput ended (return code 0) after .*Zenix is back' && echo "PASS Zenix is back" || { echo "FAIL Zenix is not back"; pass=0; }
[ $pass = 1 ] && echo "zine-input-test: all passed" || { echo "zine-input-test: FAILED (log $OUT/run.serial.log)"; exit 1; }
