#include <gio/gio.h>
#include "jubjub-gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

struct jubjub_unit;

struct jubjub_input {
	struct jubjub_unit *unit;
	JubjubInput *dbus;
	int idx;
	int state;
};

struct jubjub_output {
	struct jubjub_unit *unit;
	JubjubOutput *dbus;
	int idx;
	int state;
	struct jubjub_out_waiter *state_waiter;
	int state_read_pending;
	struct jubjub_out_waiter *state_read_waiter;
	int state_pending;
	struct jubjub_out_waiter *pulse_waiter;
	int pulse_pending;
};

struct jubjub_unit {
	int fd;
	GIOChannel *gio;
	JubjubUnit *dbus;
	struct jubjub_input inputs[8];
	struct jubjub_output outputs[16];
};

struct jubjub_out_waiter {
	struct jubjub_out_waiter *next;
	JubjubOutput *object;
	GDBusMethodInvocation *invocation;
};

void jubjub_send_byte(struct jubjub_unit *unit, int arg) {
	char byte = arg;
	int res = write(unit->fd, &byte, 1);
	if (res != 1) {
		perror("write");
		exit(1);
	}
}

void jubjub_send_out_pulse(struct jubjub_output *out) {
	jubjub_send_byte(out->unit, 0x00 | out->idx | (out->pulse_pending - 1) << 4);
}

void jubjub_send_out_set(struct jubjub_output *out) {
	jubjub_send_byte(out->unit, 0x20 | out->idx | out->state << 4);
}

void jubjub_send_out_read(struct jubjub_output *out) {
	jubjub_send_byte(out->unit, 0x70 | out->idx);
}

void jubjub_send_in_ack(struct jubjub_input *in) {
	jubjub_send_byte(in->unit, 0x60 | in->idx | in->state << 3);
}

int jubjub_retry_out_read(void *priv) {
	struct jubjub_output *out = priv;
	if (!out->state_read_pending)
		return 0;
	jubjub_send_out_read(out);
	return 1;
}

void jubjub_queue_out_read(struct jubjub_output *out) {
	if (!out->state_read_pending) {
		jubjub_send_out_read(out);
		g_timeout_add(1000, jubjub_retry_out_read, out);
		out->state_read_pending = 1;
	}
}

void jubjub_out_got_val(struct jubjub_output *out) {
	out->state_read_pending = 0;
	while (out->state_read_waiter) {
		struct jubjub_out_waiter *waiter = out->state_read_waiter;
		out->state_read_waiter = waiter->next;
		jubjub_output_complete_get_state(waiter->object, waiter->invocation, out->state);
		free(waiter);
	}
}

int jubjub_retry_out_set(void *priv) {
	struct jubjub_output *out = priv;
	if (!out->state_pending)
		return 0;
	jubjub_send_out_set(out);
	return 1;
}

void jubjub_queue_out_set(struct jubjub_output *out) {
	if (!out->state_pending) {
		jubjub_send_out_set(out);
		g_timeout_add(1000, jubjub_retry_out_set, out);
		out->state_pending = 1;
	}
}

int jubjub_out_set(struct jubjub_output *out, int state) {
	if (out->state_read_pending || out->state != state) {
		jubjub_out_got_val(out);
		out->state = state;
		jubjub_queue_out_set(out);
		return 0;
	} else {
		return 1;
	}
}

int jubjub_retry_out_pulse(void *priv) {
	struct jubjub_output *out = priv;
	if (!out->pulse_pending)
		return 0;
	jubjub_send_out_pulse(out);
	return 1;
}

void jubjub_pulse(struct jubjub_output *out, int is_long) {
	if (!out->pulse_pending) {
		out->pulse_pending = is_long + 1;
		jubjub_send_out_pulse(out);
		g_timeout_add(is_long ? 10000 : 1000, jubjub_retry_out_pulse, out);
	}
}

void jubjub_out_got_read(struct jubjub_output *out, int val) {
	if (out->state_read_pending) {
		out->state = val;
		jubjub_out_got_val(out);
	} else {
		if (out->state == val) {
			out->state_pending = 0;
			while (out->state_waiter) {
				struct jubjub_out_waiter *waiter = out->state_waiter;
				out->state_waiter = waiter->next;
				jubjub_output_complete_set_state(waiter->object, waiter->invocation);
				free(waiter);
			}
		} else {
			jubjub_queue_out_set(out);
		}
	}
}

void jubjub_out_pulse_done(struct jubjub_output *out) {
	out->pulse_pending = 0;
	while (out->pulse_waiter) {
		struct jubjub_out_waiter *waiter = out->pulse_waiter;
		out->pulse_waiter = waiter->next;
		jubjub_output_complete_pulse(waiter->object, waiter->invocation);
		free(waiter);
	}
}

void jubjub_send_init(struct jubjub_unit *unit) {
	char seq[14] = "TZWUVRVZWUVRRQ";
	tcflush(unit->fd, TCIOFLUSH);
	int res = write(unit->fd, seq, 14);
	if (res != 14) {
		perror("write");
		exit(1);
	}
	int i;
	for (i = 0; i < 16; i++) {
		struct jubjub_output *out = &unit->outputs[i];
		if (!out->state_read_pending)
			jubjub_queue_out_set(out);
	}
}

void jubjub_recv(struct jubjub_unit *unit, int byte) {
	struct jubjub_output *out = &unit->outputs[byte&0xf];
	struct jubjub_input *in = &unit->inputs[byte&7];
	printf("RECV %02x\n", byte);
	int ov = byte >> 4 & 1;
	int iv = byte >> 3 & 1;
	switch (byte & 0xf0) {
		case 0x00:
			break;
		case 0x10:
			jubjub_out_pulse_done(out);
			break;
		case 0x20:
		case 0x30:
			jubjub_out_got_read(out, ov);
			break;
		case 0x40:
			break;
		case 0x60:
			in->state = iv;
			jubjub_send_in_ack(in);
			break;
		default:
			if (byte != 0x51)
				jubjub_send_init(unit);
			break;
	}
}

int on_pulse(JubjubOutput *object, GDBusMethodInvocation *invocation, int is_long, void *priv) {
	struct jubjub_output *out = priv;
	jubjub_pulse(out, is_long);
	struct jubjub_out_waiter *waiter = calloc(sizeof *waiter, 1);
	waiter->object = object;
	waiter->invocation = invocation;
	waiter->next = out->pulse_waiter;
	out->pulse_waiter = waiter;
	return 1;
}

int on_get_out(JubjubOutput *object, GDBusMethodInvocation *invocation, void *priv) {
	struct jubjub_output *out = priv;
	if (!out->state_read_pending) {
		jubjub_output_complete_get_state(object, invocation, out->state);
	} else {
		struct jubjub_out_waiter *waiter = calloc(sizeof *waiter, 1);
		waiter->object = object;
		waiter->invocation = invocation;
		waiter->next = out->state_read_waiter;
		out->state_read_waiter = waiter;
	}
	return 1;
}

int on_get_in(JubjubInput *object, GDBusMethodInvocation *invocation, void *priv) {
	struct jubjub_input *in = priv;
	jubjub_input_complete_get_state(object, invocation, in->state);
	return 1;
}

int on_set_out(JubjubOutput *object, GDBusMethodInvocation *invocation, int val, void *priv) {
	struct jubjub_output *out = priv;
	if (jubjub_out_set(out, val)) {
		jubjub_output_complete_set_state(object, invocation);
	} else {
		struct jubjub_out_waiter *waiter = calloc(sizeof *waiter, 1);
		waiter->object = object;
		waiter->invocation = invocation;
		waiter->next = out->state_waiter;
		out->state_waiter = waiter;
	}
	return 1;
}

int jubjub_handle_gio(GIOChannel *gio, GIOCondition cond, void *priv) {
	struct jubjub_unit *unit = priv;
	char byte;
	int res = read(unit->fd, &byte, 1);
	if (res == 1) {
		jubjub_recv(unit, byte);
	} else {
		perror("read");
		exit(1);
	}
	return 1;
}

struct jubjub_unit *jubjub_open(const char *fname) {
	struct jubjub_unit *unit = calloc(sizeof *unit, 1);
	unit->fd = open(fname, O_RDWR | O_NOCTTY);
	if (unit->fd < 0) {
		perror("open");
		free(unit);
		return 0;
	}
	unit->gio = g_io_channel_unix_new(unit->fd);
	if (!unit->gio) {
		perror("gio");
		close(unit->fd);
		free(unit);
		return 0;
	}
	g_io_add_watch(unit->gio, G_IO_IN | G_IO_ERR, jubjub_handle_gio, unit);
	struct termios tio;
	if (tcgetattr(unit->fd, &tio)) {
		perror("tcgetattr");
		free(unit);
		return 0;
	}
	tio.c_iflag = IGNBRK;
	tio.c_oflag = 0;
	tio.c_cflag = B9600 | CS8 | CREAD | CLOCAL;
	tio.c_lflag = 0;
	if (tcsetattr(unit->fd, TCSANOW, &tio)) {
		perror("tcsetattr");
		free(unit);
		return 0;
	}
	int i;
	for (i = 0; i < 8; i++) {
		struct jubjub_input *in = &unit->inputs[i];
		in->unit = unit;
		in->dbus = jubjub_input_skeleton_new();
		in->idx = i;
		in->state = 0;
		g_signal_connect(in->dbus, "handle-get-state", G_CALLBACK(on_get_in), in);
	}
	for (i = 0; i < 16; i++) {
		struct jubjub_output *out = &unit->outputs[i];
		out->unit = unit;
		out->dbus = jubjub_output_skeleton_new();
		out->idx = i;
		jubjub_queue_out_read(out);
		g_signal_connect(out->dbus, "handle-pulse", G_CALLBACK(on_pulse), out);
		g_signal_connect(out->dbus, "handle-set-state", G_CALLBACK(on_set_out), out);
		g_signal_connect(out->dbus, "handle-get-state", G_CALLBACK(on_get_out), out);
	}
	jubjub_send_init(unit);
	return unit;
}

static void on_bus_acquired(GDBusConnection *connection, const char *name, void *priv) {
	struct jubjub_unit *unit = priv;
	int i;
	for (i = 0; i < 8; i++) {
		struct jubjub_input *in = &unit->inputs[i];
		char *name = g_strdup_printf("/in%d", i);
		GError *error = 0;
		g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(in->dbus), connection, name, &error);
		g_free(name);
	}
	for (i = 0; i < 16; i++) {
		struct jubjub_output *out = &unit->outputs[i];
		char *name = g_strdup_printf("/out%d", i);
		GError *error = 0;
		g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(out->dbus), connection, name, &error);
		g_free(name);
	}
}

int main(int argc, char **argv) {
	g_type_init();
	struct jubjub_unit *unit = jubjub_open(argv[1]);
	if (!unit)
		return 1;
	unsigned owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, "pl.koriakin.Jubjub", 0, on_bus_acquired, 0, 0, unit, 0);
	GMainLoop *loop = g_main_loop_new(0, 0);
	g_main_loop_run(loop);
	g_bus_unown_name(owner_id);
	return 0;
}

