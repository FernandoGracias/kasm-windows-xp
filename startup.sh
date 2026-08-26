#!/bin/bash
while ! xdpyinfo -display :1 >/dev/null 2>&1; do
    sleep 0.5
done

export DISPLAY=:1

OVERLAY_PATH="$HOME/winxp-overlay.qcow2"
if [ ! -f "$OVERLAY_PATH" ]; then
    qemu-img create -f qcow2 -b /opt/winxp/winxp.qcow2 -F qcow2 "$OVERLAY_PATH"
fi

qemu-system-x86_64 \
    -name "Windows XP" \
    -machine pc-i440fx-5.2,accel=kvm \
    -cpu core2duo -smp 2 -m 2048 \
    -drive file="$OVERLAY_PATH",format=qcow2 \
    -usb -device usb-tablet \
    -device AC97 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -vga std \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -drive if=ide,index=1,media=cdrom \
    -display gtk,zoom-to-fit=on,window-close=off \
    -full-screen &

# Wait for QEMU to be ready
sleep 5

# Start the bridge daemon
exec python3 /opt/winxp/xp_bridge_daemon.py
