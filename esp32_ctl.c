#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp32_link.h"

#define BUFFER_SIZE 256

static void *monitor_thread(void *arg)
{
	const int fd = *(const int *)arg;
	char rx_buffer[BUFFER_SIZE];
	char line_buffer[BUFFER_SIZE];
	size_t line_pos = 0;
	bool discarding_line = false;
	ssize_t bytes_read;

	for (;;) {
		bytes_read = read(fd, rx_buffer, sizeof(rx_buffer));
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			perror("[Monitor] Error en read()");
			break;
		}
		if (bytes_read == 0)
			break;

		for (ssize_t i = 0; i < bytes_read; i++) {
			const char c = rx_buffer[i];

			if (c == '\n') {
				if (!discarding_line) {
					line_buffer[line_pos] = '\0';
					printf("\r\033[K[ESP32] %s\n", line_buffer);
					printf("esp32-ctl> ");
					fflush(stdout);
				}
				line_pos = 0;
				discarding_line = false;
			} else if (c != '\r' && !discarding_line) {
				if (line_pos < sizeof(line_buffer) - 1) {
					line_buffer[line_pos++] = c;
				} else {
					discarding_line = true;
					line_pos = 0;
				}
			}
		}
	}

	return NULL;
}

static int send_bytes(int fd, const char *data, size_t length)
{
	size_t sent = 0;

	while (sent < length) {
		ssize_t result = write(fd, data + sent, length - sent);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (result == 0) {
			errno = EIO;
			return -1;
		}
		sent += (size_t)result;
	}

	return 0;
}

static int send_line(int fd, const char *line)
{
	char payload[BUFFER_SIZE];
	int length = snprintf(payload, sizeof(payload), "%s\n", line);

	if (length < 0 || (size_t)length >= sizeof(payload)) {
		errno = EMSGSIZE;
		return -1;
	}
	return send_bytes(fd, payload, (size_t)length);
}

static void print_help(void)
{
	puts("\n--- Consola de Control de Carga Electronica ---");
	puts("Comandos:");
	puts("  arm                  : habilita las protecciones");
	puts("  stop                 : detiene la carga");
	puts("  stream <0|1>          : desactiva/activa la telemetria JSON");
	puts("  set <CC|CR> <val>    : cambia modo y setpoint");
	puts("  exec <kp> <ki> <kd>  : envia parametros PID");
	puts("  get                  : envia PING; la respuesta PONG aparece abajo");
	puts("  help                 : muestra esta ayuda");
	puts("  quit / exit          : cierra el programa");
	puts("------------------------------------------------\n");
}

int main(void)
{
	int fd;
	pthread_t tid;
	char input[BUFFER_SIZE];
	char cmd[32] = {0};
	char arg1[32] = {0};
	char arg2[32] = {0};
	char arg3[32] = {0};
	char tx_payload[BUFFER_SIZE];
	int parsed;
	int ret;

	fd = open(ESP32_LINK_DEVICE_NODE, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error al abrir %s: %s\n",
			ESP32_LINK_DEVICE_NODE, strerror(errno));
		fprintf(stderr, "Comprueba que el overlay y el modulo esp32_link esten cargados.\n");
		return EXIT_FAILURE;
	}

	ret = pthread_create(&tid, NULL, monitor_thread, &fd);
	if (ret != 0) {
		fprintf(stderr, "Fallo al crear el hilo monitor: %s\n", strerror(ret));
		close(fd);
		return EXIT_FAILURE;
	}

	print_help();
	for (;;) {
		printf("esp32-ctl> ");
		fflush(stdout);
		if (!fgets(input, sizeof(input), stdin))
			break;

		input[strcspn(input, "\r\n")] = '\0';
		if (input[0] == '\0')
			continue;

		cmd[0] = arg1[0] = arg2[0] = arg3[0] = '\0';
		parsed = sscanf(input, "%31s %31s %31s %31s",
				cmd, arg1, arg2, arg3);
		if (parsed < 1)
			continue;

		if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
			break;
		} else if (strcmp(cmd, "help") == 0) {
			print_help();
			continue;
		} else if (strcmp(cmd, "arm") == 0) {
			ret = send_line(fd, "ARM");
		} else if (strcmp(cmd, "stop") == 0) {
			ret = send_line(fd, "STOP");
		} else if (strcmp(cmd, "stream") == 0 && parsed >= 2 &&
			   (strcmp(arg1, "0") == 0 || strcmp(arg1, "1") == 0)) {
			snprintf(tx_payload, sizeof(tx_payload), "STREAM %s", arg1);
			ret = send_line(fd, tx_payload);
		} else if (strcmp(cmd, "set") == 0 && parsed >= 3 &&
			   (strcmp(arg1, "CC") == 0 || strcmp(arg1, "CR") == 0)) {
			snprintf(tx_payload, sizeof(tx_payload), "MODE %s", arg1);
			ret = send_line(fd, tx_payload);
			if (ret == 0) {
				const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
				nanosleep(&pause, NULL);
				snprintf(tx_payload, sizeof(tx_payload), "SETPOINT %s", arg2);
				ret = send_line(fd, tx_payload);
			}
		} else if (strcmp(cmd, "exec") == 0 && parsed >= 4) {
			snprintf(tx_payload, sizeof(tx_payload), "PID %s %s %s",
				 arg1, arg2, arg3);
			ret = send_line(fd, tx_payload);
		} else if (strcmp(cmd, "get") == 0) {
			ret = send_line(fd, "PING");
		} else {
			printf("Uso incorrecto o comando desconocido: '%s'. Escribe 'help'.\n", cmd);
			continue;
		}

		if (ret < 0)
			fprintf(stderr, "Error enviando comando: %s\n", strerror(errno));
	}

	puts("\nCerrando programa...");
	pthread_cancel(tid);
	pthread_join(tid, NULL);
	close(fd);
	return EXIT_SUCCESS;
}
