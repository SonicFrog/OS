cmd_/usr/src/uart/uart16550.ko := ld -r -m elf_i386 -T ./scripts/module-common.lds --build-id  -o /usr/src/uart/uart16550.ko /usr/src/uart/uart16550.o /usr/src/uart/uart16550.mod.o
