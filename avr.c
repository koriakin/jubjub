#include <avr/io.h>
#include <avr/interrupt.h>

/* consts */

#define NOUT 16	/* number of outputs */
#define NIN 8	/* number of inputs */
#define HZ 50	/* roughly this many ticks [timer interrupts] per second */
#define PULSE_SHORT HZ/5	/* 200 ms */
#define PULSE_LONG HZ*5		/* 5 s */
#define IRETRY HZ		/* 1s - time between resends of unacked data */
#define WDTIME 60		/* watchdog seconds until boom */
#define WDWARN 15		/* warn host when this many seconds left until
				   watchdog boom */
#define WDOUT 0			/* which output to pulse on watchdog boom */
#define PULSE_WD PULSE_SHORT	/* length of watchdog pulse */

/* data */

uint8_t pulserem[NOUT] = { 0 };	/* ticks remaining until end of pulse on given
				   output, 0 if no pulse in progress */
uint8_t pap[NOUT] = { 0 };	/* pulse ack send pending */
uint8_t pdp[NOUT] = { 0 };	/* pulse done send pending */
uint8_t osp[NOUT] = { 0 };	/* output status send pending */
uint8_t ostat[NOUT] = { 0 };	/* current output state */
uint8_t istat[NIN] = { 0 };	/* last sampled input state */
uint8_t isp[NIN] = { 0 };	/* input state send pending */
uint8_t iret[NIN] = { 0 };	/* ticks remaining until input state resend */
uint8_t wdticks = 0;		/* ticks remaining until next complete second
				   for the watchdog */
uint8_t wdsecs = 0;		/* watchdog seconds remaining until boom */
uint8_t wdwp = 0;		/* watchdog warning pending */
uint8_t wdap = 0;		/* watchdog ping ack pending */
uint8_t wdop = 0;		/* watchdog off ack pending */
uint8_t wdact = 0;		/* watchdog active */
uint8_t active = 0;		/* host communication ok */

/* input/output interface */

/* outputs: PA0-PA7, PB0-PB7 */
/* inputs: PC0-PC7 */

/* set hw output */
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

/* read hw input */
static inline int8_t getin(int8_t i) {
	return PINC >> i & 1;
}

/* host transmit control */

/* mark pending sends - enable interrupt on output buffer empty */
static inline void p() {
	UCSRB |= 0x20;
}

/* mark no pending sends - disable interrupt on output buffer empty */
static inline void d() {
	UCSRB &= ~0x20;
}

/* error in communication with host - send error notification, disable
   interface, wait for host to restart protocol */
void err() {
	UDR = 0x50;
	active = 0;
	d();
}

/* input and output tickers */

/* output tick - set state, handle pulses */
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

/* input tick - read state, notify if changed, retransmit notification if not
   acked */
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

/* watchdog tick */
void wdtick() {
	if (!wdact)
		return;
	wdticks--;
	if (!wdticks) {
		wdticks = HZ;
		wdsecs--;
		if (wdsecs == WDWARN) {
			/* warn the host about impending doom */
			wdwp = 1;
			p();
		}
		if (!wdsecs) {
			/* that's it. everything's going to go boom. */
			pulserem[WDOUT] = PULSE_WD;
			/* not very useful, but can't hurt for testing */
			UDR = 0x43;
			/* abort all operations */
			wdact = 0;
			active = 0;
			d();
		}
	}
}

/* monitor */

uint8_t rxtick = 0;
uint8_t txtick = 0;

void montick() {
	PORTD = active << 5 | !!rxtick << 6 | !!txtick << 7;
	if (rxtick)
		rxtick--;
	if (txtick)
		txtick--;
}

/* host communication */

/* select one piece of pending information and send it */
int8_t sbyte() {
	int8_t i;
	if (!active)
		return 0;
	/* watchdog */
	if (wdwp) {	/* warning has priority */
		UDR = 0x41;
		wdwp = 0;
		return 1;
	}
	if (wdop) {
		UDR = 0x42;
		wdop = 0;
		return 1;
	}
	if (wdap) {
		UDR = 0x40;
		wdap = 0;
		return 1;
	}
	/* outputs */
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
	/* inputs */
	for (i = 0; i < NIN; i++) {
		if (isp[i]) {
			UDR = 0x60 | i | istat[i] << 3;
			isp[i] = 0;
			return 1;
		}
	}
	return 0;
}

/* short pulse command from host */
void cmd_pulse_short(int8_t i) {
	pulserem[i] = PULSE_SHORT;
	pap[i] = 1;
	p();
}

/* long pulse command from host */
void cmd_pulse_long(int8_t i) {
	pulserem[i] = PULSE_LONG;
	pap[i] = 1;
	p();
}

/* set output state command from host */
void cmd_out(int8_t i, int8_t v) {
	ostat[i] = v;
	osp[i] = 1;
	p();
}

/* output state read command from host */
void cmd_oread(int8_t i) {
	osp[i] = 1;
	p();
}

/* input state notification ack from host */
void cmd_iack(int8_t i, int8_t val) {
	if (istat[i] == val) {
		iret[i] = 0;
	} else {
		isp[i] = 1;
		iret[i] = IRETRY;
		p();
	}
}

/* watchdog cmds */
void cmd_watchdog(uint8_t subcmd) {
	switch (subcmd) {
		case 0:
			/* start */
			wdact = 1;
			wdticks = HZ;
			wdsecs = WDTIME;
			wdap = 1;
			p();
			break;
		case 1:
			/* poke */
			wdticks = HZ;
			wdsecs = WDTIME;
			wdap = 1;
			p();
			break;
		case 2:
			/* stop */
			wdact = 0;
			wdop = 1;
			p();
			break;
		default:
			err();
	}
}

/* host interface start sequence */

/* the magic values the host needs to send */
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
uint8_t svidx = 0;	/* index of next value that should be received */

/* start sequuence byte received */
void cmd_start(int8_t parm) {
	if (svals[svidx] == parm) {
		svidx++;
		active = 0;
		if (svidx == NSVALS) {
			/* got full start sequence, send full state to host */
			active = 1;
			int8_t i;
			UDR = 0x51;
			for (i = 0; i < NOUT; i++) {
				pulserem[i] = 0;
				osp[i] = 1;
			}
			for (i = 0; i < NIN; i++) {
				iret[i] = IRETRY;
				isp[i] = 1;
			}
			if (wdact)
				wdwp = 1;
			else
				wdop = 1;
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

/* host command dispatch */

/* select the right command */
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
			case 4:
				cmd_watchdog(byte & 0xf);
				break;
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

/* timer interrupt - run all tickers */
ISR(TIMER0_COMP_vect) {
	outtick();
	intick();
	wdtick();
	montick();
}

/* host receive interrupt */
ISR(USART_RXC_vect) {
	rxtick = 3;
	int8_t e = 0;
	if (UCSRA & 0x1c)
		e = 1;	
	uint8_t byte = UDR;
	if (e) {
		err();
	} else {
		cmd(byte);
	}
}

/* host send ready interrupt */
ISR(USART_UDRE_vect) {
	if (!sbyte())
		d();
	else
		txtick = 3;
}

int main() {
	PORTA = 0x00;
	PORTB = 0x00;
	PORTC = 0xff;
	DDRA = 0xff;
	DDRB = 0xff;
	DDRC = 0x00;
	DDRD = 0xe0;
	/* 160k we want */
	OCR0 = 155;
	TCCR0 = 0xd;
	TIMSK |= 2;
	UCSRA = 0x40;
	UCSRB = 0x98;
	UCSRC = 0x86;
	UBRRH = 0x00;
	UBRRL = 0x34;
	err();
	sei();
	while(1);
}
