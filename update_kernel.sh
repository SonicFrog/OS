#!/bin/sh

set -eux

LOCAL_KERNEL_SOURCE=linux-3.18.3/
VM_IP=root@192.168.2.130
VM_SRC_DIR=/usr/src/linux

[ -x `which rsync` ] || (echo "rsync not found!" && exit 1)

# Copying kernel source to VM
rsync -hrvva --progress $LOCAL_KERNEL_SOURCE $VM_IP:$VM_SRC_DIR

ssh ${VM_IP} "make -j2 bzImage"
ssh ${VM_IP} "cp $VM_SRC_DIR/arch/x86/boot/bzImage /boot/vmlinuz-3.18.3+"
ssh ${VM_IP} "update-initramfs -k 3.18.3+ -u"
ssh ${VM_IP} reboot
