#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DEVICE_NODE "/dev/esp32_link"
#define BUFFER_SIZE 256

/* Debe coincidir exactamente con la macro definida en el kernel driver */
#define ESP32_GET _IOR('E', 1, int)

/* 
 * Hilo Monitor: Se ejecuta en segundo plano.
 * Realiza un read() bloqueante. Gracias a la kfifo y wait_queue del driver, 
 * este hilo duerme sin consumir CPU hasta que llega telemetría por UART.
 */
void *monitor_thread(void *arg) {
    int fd = *(int *)arg;
    char rx_buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while (1) {
        bytes_read = read(fd, rx_buffer, sizeof(rx_buffer) - 1);
        
        if (bytes_read > 0) {
            rx_buffer[bytes_read] = '\0';
            
            /* Usamos \r para sobreescribir la línea actual y no romper 
               el prompt del usuario cuando llega telemetría asíncrona */
            printf("\r\033[K[ESP32] Telemetría: %s", rx_buffer);
            
            // Si el buffer no traía su propio salto de línea, lo agregamos
            if (rx_buffer[bytes_read - 1] != '\n') {
                printf("\n");
            }
            
            /* Reimprimir el prompt para el hilo principal */
            printf("esp32-ctl> ");
            fflush(stdout);
        } else if (bytes_read < 0) {
            perror("\r\033[K[Monitor] Error en read()");
            break;
        }
    }
    return NULL;
}

void print_help() {
    printf("\n--- Consola de Control de Carga Electrónica ---\n");
    printf("Comandos:\n");
    printf("  set <modo> <valor>   : Configura modo (CC/CR) y setpoint (Ej: set CC 1.5)\n");
    printf("  exec <kp> <ki> <kd>  : Actualiza parámetros del PID (Ej: exec 1.2 0.5 0.1)\n");
    printf("  get                  : Consulta de estado síncrona (IOCTL)\n");
    printf("  help                 : Muestra esta ayuda\n");
    printf("  quit / exit          : Cierra el programa\n");
    printf("-----------------------------------------------\n\n");
}

int main(int argc, char *argv[]) {
    int fd;
    pthread_t tid;
    char input[BUFFER_SIZE];
    char cmd[32], arg1[32], arg2[32], arg3[32];
    char tx_payload[BUFFER_SIZE];

    /* 1. Abrir el dispositivo creado por el driver Serdev */
    fd = open(DEVICE_NODE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error al abrir %s: %s\n", DEVICE_NODE, strerror(errno));
        fprintf(stderr, "Verifica que el driver esté cargado (lsmod) y los permisos.\n");
        return EXIT_FAILURE;
    }

    /* 2. Lanzar el hilo monitor para la lectura asíncrona */
    if (pthread_create(&tid, NULL, monitor_thread, &fd) != 0) {
        perror("Fallo al crear el hilo monitor");
        close(fd);
        return EXIT_FAILURE;
    }

    print_help();

    /* 3. Bucle principal de la consola interactiva */
    while (1) {
        printf("esp32-ctl> ");
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break; /* EOF detectado (Ctrl+D) */
        }

        /* Limpiar salto de línea */
        input[strcspn(input, "\n")] = 0;

        if (strlen(input) == 0) continue;

        /* Parsear la entrada */
        int parsed = sscanf(input, "%31s %31s %31s %31s", cmd, arg1, arg2, arg3);

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } 
        else if (strcmp(cmd, "help") == 0) {
            print_help();
        }
        else if (strcmp(cmd, "set") == 0) {
            if (parsed >= 3) {
                /* Armar trama JSON para el modo y el valor */
                snprintf(tx_payload, sizeof(tx_payload), 
                         "{\"cmd\":\"set_mode\",\"mode\":\"%s\",\"val\":%s}\n", arg1, arg2);
                
                write(fd, tx_payload, strlen(tx_payload));
                printf("[TX] -> %s", tx_payload);
            } else {
                printf("Error: faltan argumentos. Uso: set <modo> <valor>\n");
            }
        }
        else if (strcmp(cmd, "exec") == 0) {
            if (parsed >= 4) {
                /* Armar trama JSON para el PID */
                snprintf(tx_payload, sizeof(tx_payload), 
                         "{\"cmd\":\"set_pid\",\"kp\":%s,\"ki\":%s,\"kd\":%s}\n", arg1, arg2, arg3);
                
                write(fd, tx_payload, strlen(tx_payload));
                printf("[TX] -> %s", tx_payload);
            } else {
                printf("Error: faltan argumentos. Uso: exec <kp> <ki> <kd>\n");
            }
        }
        else if (strcmp(cmd, "get") == 0) {
            /* Ejecutar llamada síncrona IOCTL. El Kernel Driver tomará un mutex, 
               enviará una solicitud a la ESP32 y esperará la barrera (completion) */
            int sync_result = 0;
            printf("[IOCTL] Solicitando estado síncrono...\n");
            
            if (ioctl(fd, ESP32_GET, &sync_result) == 0) {
                printf("[IOCTL] OK - Valor devuelto por el kernel: %d\n", sync_result);
            } else {
                perror("[IOCTL] Error o Timeout");
            }
        }
        else {
            printf("Comando desconocido: '%s'. Escribe 'help'.\n", cmd);
        }
    }

    /* Limpieza y cierre ordenado */
    printf("\nCerrando programa...\n");
    pthread_cancel(tid);      // Forzar la salida del hilo monitor (bloqueado en read)
    pthread_join(tid, NULL);  // Esperar a que el hilo muera
    close(fd);
    
    return EXIT_SUCCESS;
}