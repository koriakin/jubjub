all:
	avr-gcc -mmcu=atmega16 avr.c -o avr.elf -O2
	avr-objcopy avr.elf avr.bin -O binary
run: all
	avrdude -p m16 -c stk500v2 -P /dev/ttyUSB0 -U flash:w:avr.bin:r
