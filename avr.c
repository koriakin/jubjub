#include <avr/io.h>
#include <avr/interrupt.h>
#define F_CPU 8000000
#include <util/delay.h>

#define NOUT 16
#define NIN 8
#define IRETRY 50

uint8_t pulserem[NOUT] = { 0 };
uint8_t pap[NOUT] = { 0 };
uint8_t pdp[NOUT] = { 0 };
uint8_t osp[NOUT] = { 0 };
uint8_t ostat[NOUT] = { 0 };
uint8_t istat[NIN] = { 0 };
uint8_t isp[NIN] = { 0 };
uint8_t iret[NIN] = { 0 };
uint8_t active = 0;

static inline void setout(int8_t i, int8_t val) {
	if (i < 8) {
		uint8_t v = PORTA;
		v &= ~(1 << i);
		v |= val << i;
		PORTA = v;
	} else {
		i -= 8;
		uint8_t v = PORTB;
		v &= ~(1 << i);
		v |= val << i;
		PORTB = v;
	}
}

static inline int8_t getin(int8_t i) {
	return PINC >> i & 1;
}

static inline void p() {
	UCSRB |= 0x20;
}

static inline void d() {
	UCSRB &= ~0x20;
}

void err() {
	UDR = 0x50;
	active = 0;
	d();
}

void outtick() {
	int8_t i;
	for (i = 0; i < NOUT; i++) {
		if (pulserem[i]) {
			setout(i, ostat[i]^1);
			pulserem[i]--;
			if (!pulserem[i]) {
				pdp[i] = 1;
				p();
			}
		} else {
			setout(i, ostat[i]);
		}
	}
}

void intick() {
	int8_t i;
	for (i = 0; i < NIN; i++) {
		int8_t st = getin(i);
		if (iret[i]) {
			iret[i]--;
			if (!iret[i]) {
				isp[i] = 1;
				iret[i] = IRETRY;
				p();
			}
		}
		if (st != istat[i]) {
			istat[i] = st;
			isp[i] = 1;
			iret[i] = IRETRY;
			p();
		}
	}
}

int8_t sbyte() {
	int8_t i;
	if (!active)
		return 0;
	for (i = 0; i < NOUT; i++) {
		if (pap[i]) {
			UDR = 0x00 | i;
			pap[i] = 0;
			return 1;
		}
		if (pdp[i]) {
			UDR = 0x10 | i;
			pdp[i] = 0;
			return 1;
		}
		if (osp[i]) {
			UDR = 0x20 | i | ostat[i] << 4;
			osp[i] = 0;
			return 1;
		}
	}
	for (i = 0; i < NIN; i++) {
		if (isp[i]) {
			UDR = 0x60 | i | istat[i] << 3;
			isp[i] = 0;
			return 1;
		}
	}
	return 0;
}

void cmd_pulse_short(int8_t i) {
	pulserem[i] = 5;
	pap[i] = 1;
	p();
}

void cmd_pulse_long(int8_t i) {
	pulserem[i] = 250;
	pap[i] = 1;
	p();
}

void cmd_out(int8_t i, int8_t v) {
	ostat[i] = v;
	osp[i] = 1;
	p();
}

void cmd_oread(int8_t i) {
	osp[i] = 1;
	p();
}

void cmd_iack(int8_t i, int8_t val) {
	if (istat[i] == val) {
		iret[i] = 0;
	} else {
		isp[i] = 1;
		iret[i] = IRETRY;
		p();
	}
}

#define NSVALS 14

const uint8_t svals[NSVALS] = {
	0x4, 0xa,
	0x7, 0x5,
	0x6, 0x2,
	0x6, 0xa,
	0x7, 0x5,
	0x6, 0x2,
	0x2, 0x1,
};
uint8_t svidx = 0;

uint8_t sstat = 1;

void cmd_start(int8_t parm) {
	if (svals[svidx] == parm) {
		svidx++;
		active = 0;
		if (svidx == NSVALS) {
			active = 1;
			int8_t i;
			for (i = 0; i < NOUT; i++) {
				pulserem[i] = 0;
				osp[i] = 1;
			}
			for (i = 0; i < NIN; i++) {
				iret[i] = IRETRY;
				isp[i] = 1;
			}
//			UDR = 0x50 | sstat;
			p();
			svidx = 0;
		}
	} else if (svals[0] == parm) {
		svidx = 1;
		active = 0;
		d();
	} else {
		svidx = 0;
		active = 0;
		d();
	}
}

void cmd(uint8_t byte) {
	if ((byte & 0xf0) == 0x50) {
		cmd_start(byte & 0xf);
	} else {
		if (!active) {
			err();
			return;
		}
		switch (byte >> 4) {
			case 0:
				cmd_pulse_short(byte & 0xf);
				break;
			case 1:
				cmd_pulse_long(byte & 0xf);
				break;
			case 2:
			case 3:
				cmd_out(byte & 0xf, byte >> 4 & 1);
				break;
			/* XXX: 4 */
			case 6:
				cmd_iack(byte & 0x7, byte >> 3 & 1);
				break;
			case 7:
				cmd_oread(byte & 0xf);
				break;
			default:
				err();
		}
	}
}

ISR(TIMER0_COMP_vect) {
	outtick();
	intick();
}

ISR(USART_RXC_vect) {
	int8_t e = 0;
	if (UCSRA & 0x1c)
		e = 1;	
	uint8_t byte = UDR;
	if (e) {
		active = 0;
		UDR = 0x50;
	} else {
		cmd(byte);
	}
}

ISR(USART_UDRE_vect) {
	if (!sbyte())
		d();
}

int main() {
	PORTA = 0x00;
	PORTB = 0x00;
	PORTC = 0x00;
	DDRA = 0xff;
	DDRB = 0xff;
	DDRC = 0x00;
	DDRD = 0;
	/* 160k we want */
	OCR0 = 155;
	TCCR0 = 0xd;
	TIMSK |= 2;
//	ADMUX = 0xe0;
//	SFIOR &= 0xf;
	UCSRA = 0x40;
	UCSRB = 0x98;
	UCSRC = 0x86;
	UBRRH = 0x00;
	UBRRL = 0x33;
	err();
	sei();
	while(1);
}
