# kasm-windows-xp

A Kasm Workspace that runs Windows XP in QEMU with full integration — clipboard sync, file transfer, and auto-resolution scaling — via a custom bridge protocol.

## Features

- **Clipboard sync**: Bidirectional between your browser and XP
- **File upload**: Drop files into Kasm's upload UI → appears at `C:\Uploads\` in XP. ISOs are auto-mounted as CD-ROM.
- **File download**: Place files in `C:\Downloads\` inside XP → available in Kasm's download UI
- **Auto-resolution**: XP display resolution tracks your browser window size (nearest VESA mode)
- **KVM acceleration**: Full hardware virtualization, runs at native speed
- **Sound**: AC97 audio device, works out of the box in XP
- **Networking**: NAT via QEMU user-mode networking

## Architecture

```
Browser ←→ KasmVNC ←→ X11 (display :1) ←→ QEMU GTK (fullscreen)
                          ↕                         ↕
                     X11 clipboard            QEMU VM (Windows XP)
                          ↕                         ↕
                   xp_bridge_daemon.py ←TCP:9500→ xp_bridge_agent.exe
```

## Prerequisites

- Docker host with `/dev/kvm` available (Intel VT-x or AMD-V)
- Kasm Workspaces installed
- A prepared Windows XP qcow2 disk image (see below)

## Preparing the Base Image

The qcow2 image is not included (Windows XP is copyrighted by Microsoft). You must prepare your own:

1. **Get a Windows XP disk image.** Archive.org has pre-installed QEMU images:
   ```
   https://archive.org/details/en_winxp_vm-qemu_5.2
   ```
   Download `en_winxp_sp3_x86_ie8.qcow2` (~1.3GB).

2. **Compile the guest agent** (requires MinGW cross-compiler):
   ```bash
   sudo apt install gcc-mingw-w64-i686
   i686-w64-mingw32-gcc -o xp_bridge_agent.exe guest/agent.c -lws2_32 -luser32 -lgdi32 -mwindows
   ```

3. **Install the agent into the XP image.** Boot the image with QEMU:
   ```bash
   # Create an ISO with the agent
   mkdir -p /tmp/agent-iso
   cp xp_bridge_agent.exe /tmp/agent-iso/
   cat > /tmp/agent-iso/install.bat << 'EOF'
   @echo off
   copy D:\xp_bridge_agent.exe C:\xp_bridge_agent.exe
   mkdir C:\Uploads
   mkdir C:\Downloads
   reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v "XPBridge" /t REG_SZ /d "C:\xp_bridge_agent.exe" /f
   echo Done!
   pause
   EOF
   genisoimage -o /tmp/agent.iso /tmp/agent-iso/

   # Boot XP with the ISO mounted
   qemu-system-x86_64 -enable-kvm -m 2048 \
       -drive file=en_winxp_sp3_x86_ie8.qcow2,format=qcow2 \
       -vga std -usb -device usb-tablet \
       -device AC97 \
       -net nic,model=rtl8139 -net user \
       -drive file=/tmp/agent.iso,if=ide,index=1,media=cdrom
   ```

4. Inside XP, run `D:\install.bat`. This copies the agent and sets it to auto-start.

5. Optionally while you're in there:
   - Set your preferred display resolution (Display Properties → Settings)
   - Install a browser (MyPal68 is recommended — modern Firefox fork for XP)
   - Disable screensaver, auto-updates, registration nag
   - Install curl with a modern CA bundle for HTTPS

6. Shut down XP cleanly (Start → Shut Down).

7. The qcow2 is now your prepared base image.

## Building the Docker Image

Place your prepared qcow2 as `winxp.qcow2` in this directory, then:

```bash
docker build -t windows-xp:latest .
```

## Kasm Workspace Configuration

In Kasm Admin → Workspaces → Add Workspace:

| Field | Value |
|-------|-------|
| Friendly Name | `Windows XP` |
| Docker Image | `windows-xp:latest` |
| Docker Run Config Override | `{"devices":["/dev/kvm:/dev/kvm"],"group_add":["994"],"shm_size":"512m"}` |
| Cores | 2 |
| Memory (MB) | 3072 |

**Note:** The `group_add` value `994` is the GID of the `input` group inside the container, which owns `/dev/kvm`. If your system maps it differently, check with:
```bash
docker run --rm --device /dev/kvm --entrypoint stat windows-xp:latest -c '%g' /dev/kvm
```

## Protocol

See [PROTOCOL.md](PROTOCOL.md) for the wire protocol specification between the host daemon and guest agent.

## License

The bridge daemon and guest agent source code are MIT licensed. Windows XP itself is proprietary Microsoft software — you are responsible for your own licensing compliance.
