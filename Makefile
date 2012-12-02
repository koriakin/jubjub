all: avr.bin

avr.bin: avr.elf
	avr-objcopy avr.elf avr.bin -O binary

avr.elf: avr.c
	avr-gcc -mmcu=atmega16 avr.c -o avr.elf -O2 -Wall

run: avr.bin
	avrdude -p m16 -c stk500v2 -P /dev/ttyUSB0 -U flash:w:avr.bin:r
