#!/bin/sh

set -eux

readonly VM="$HOME/tmp/oslab-vm/Debian 7.x.vmx"

readonly KERNEL='/usr/src/'
readonly KERNEL_VER='3.18.3+'
readonly URL='git@github.com:tOverney/OperatingSystem.git'
readonly DIR='tests'
readonly MAKEOPTS='-j2'

script_path='/root/.bashrc_script'
bashrc_num="/root/.bashrc_num"

bashrc="$(mktemp)"
script="$(mktemp)"

cat > "${bashrc}" <<EOF

"${script_path}"

EOF

cat > "${script}" <<EOF

set -eux

sed -i 's/nameserver.*/nameserver 8.8.8.8/' /etc/resolv.conf

[ ! -f "${bashrc_num}" ] && echo 0 > "${bashrc_num}"

level=\$(cat "${bashrc_num}")
echo \$((level + 1)) > "${bashrc_num}"

case \${level} in
        0)
                sed -i 's/tty1$/tty1 --autologin root/' /etc/inittab

                cd "${KERNEL}"

                git remote add origin "${URL}"
                git pull -u origin master

                make ${MAKEOPTS} bzImage
                make ${MAKEOPTS} modules

                reboot

                ;;

        1)
                cd "${KERNEL}"

                git fetch
                git reset --hard FETCH_HEAD
                git pull -u origin master

                cd "${KERNEL}/linux"

                make ${MAKEOPTS} bzImage
                cp arch/x86/boot/bzImage /boot/vmlinuz-${KERNEL_VER}

                make ${MAKEOPTS} modules
                make ${MAKEOPTS} modules_install

                update-initramfs -k ${KERNEL_VER} -u

                sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT=".*7000@/&10.175.67.121\/ kgdboc=ttyS0,115200 kgdbwait/' /etc/default/grub
                update-grub

                reboot

                ;;

        2)
                cd "${KERNEL}/tests"

                ./test.sh

                ;;
esac

EOF

vmrun start "${VM}"

chmod +x "${script}"

vmrun -gu root -gp oslab CopyFileFromHostToGuest "${VM}" "${script}" "${script_path}"
vmrun -gu root -gp oslab CopyFileFromHostToGuest "${VM}" "${bashrc}" '/root/.bashrc'

rm "${script}"
rm "${bashrc}"
