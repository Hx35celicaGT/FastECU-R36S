# Building FastECU-R36S

FastECU-R36S is built on a Linux PC inside an aarch64 Arch Linux ARM chroot.
The PC runs the ARM tools through qemu, so the result is a native R36S
program. Testers never build anything; they get the release zip.

## One-time setup

### 1. qemu on the PC

Arch / Manjaro:

```
sudo pacman -S qemu-user-static qemu-user-static-binfmt
```

Debian / Ubuntu / Mint:

```
sudo apt install qemu-user-static binfmt-support
```

### 2. The chroot

```
mkdir -p ~/r36s-chroot
cd ~/r36s-chroot
wget http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
sudo bsdtar -xpf ArchLinuxARM-aarch64-latest.tar.gz -C ~/r36s-chroot
rm ArchLinuxARM-aarch64-latest.tar.gz
```

### 3. Build tools and Qt inside the chroot

```
sudo mount -t proc /proc ~/r36s-chroot/proc
sudo mount -t sysfs /sys ~/r36s-chroot/sys
sudo mount --bind /dev ~/r36s-chroot/dev
sudo cp /etc/resolv.conf ~/r36s-chroot/etc/resolv.conf
sudo chroot ~/r36s-chroot /bin/bash
```

Inside the chroot:

```
pacman-key --init
pacman-key --populate archlinuxarm
pacman -Syu --needed base-devel git qt5-base qt5-charts qt5-serialport \
    qt5-remoteobjects qt5-websockets openssl
exit
```

### 4. Source in the chroot

```
sudo git clone https://github.com/Hx35celicaGT/FastECU-R36S.git ~/r36s-chroot/root/FastECU
sudo chroot ~/r36s-chroot /bin/bash -c "cd /root/FastECU && git checkout r36s && qmake FastECU.pro"
```

## Every build

```
sudo chroot ~/r36s-chroot /bin/bash -c "cd /root/FastECU && git pull && make -j$(nproc)"
```

## Making a release

1. Commit your changes and push them to GitHub.
2. Build (above).
3. Tag the version and make the zip:

```
git tag v0.1
git push origin v0.1
r36s/make-release.sh v0.1
```

4. On GitHub: **Releases → Draft a new release**, pick tag `v0.1`, and upload
   `dist/FastECU-R36S-v0.1.zip` and its `.sha256` file.

`make-release.sh` also compiles `r36s/gamepad_mouse/gamepad_mouse.cpp` (the
button helper) in the chroot, so its source always matches too.

The zip contains the program, its libraries, all license texts and the source
for exactly that commit, so it is GPL-complete on its own.
