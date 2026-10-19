# Raspberry Pi 3 B+ - enlace UART con ESP32-S3

La Raspberry Pi usa el PL011 (`UART0`) a 115200, 8N1. El overlay libera el UART
del Bluetooth integrado y declara el ESP32 como hijo `serdev`; el modulo crea
`/dev/esp32_link`, que es el unico acceso de usuario al puerto mientras el
modulo esta cargado.

## Cableado

| Raspberry Pi 3 B+ | ESP32-S3 | Pin fisico RPi |
| --- | --- | --- |
| GPIO14 / TXD0 | GPIO44 / RX | 8 |
| GPIO15 / RXD0 | GPIO43 / TX | 10 |
| GND | GND | 6, 9 o 14 |

TX y RX van cruzados. Ambas placas deben compartir GND y usar niveles logicos
de 3,3 V. No conectes el pin de 5 V de la Raspberry al ESP32.

## Compilar en la Raspberry Pi

Copia esta carpeta a la Pi y ejecuta desde ella:

```sh
sudo apt update
sudo apt install build-essential raspberrypi-kernel-headers device-tree-compiler
make
```

`make` compila el modulo contra el kernel que esta corriendo, la consola
`esp32-ctl` y el overlay Device Tree. Si el enlace `build` de
`/lib/modules/$(uname -r)/` no existe, instala las cabeceras correspondientes
al kernel en ejecucion y vuelve a compilar.

Instala el overlay en la particion de arranque activa:

```sh
if [ -d /boot/firmware/overlays ]; then
    OVERLAYS=/boot/firmware/overlays
else
    OVERLAYS=/boot/overlays
fi
sudo install -D -m 0644 esp32-link.dtbo "$OVERLAYS/esp32-link.dtbo"
```

Edita `config.txt` (en `/boot/firmware/config.txt` en Raspberry Pi OS Bookworm
y posterior; en `/boot/config.txt` en imagenes anteriores) y agrega:

```ini
enable_uart=1
dtoverlay=esp32-link
```

Este overlay ya desactiva el Bluetooth UART y libera UART0; no agregues tambien
`dtoverlay=disable-bt`. El Bluetooth integrado de la Pi queda deshabilitado.
Si existe el servicio `hciuart`, deshabilitalo para que no intente abrir ese
UART:

```sh
sudo systemctl disable --now hciuart.service
```

En `sudo raspi-config`, abre **Interface Options > Serial Port** y responde
**No** a la consola de login serie y **Yes** al hardware serie. Reinicia la Pi.
Esto evita que la consola Linux compita con los datos de la carga. La conexion
SSH/Ethernet o Wi-Fi no se ve afectada.

## Cargar y usar

Despues del reinicio, vuelve a copiar los archivos fuente si compilaste en otro
equipo; compilar en la Pi es lo recomendado para usar exactamente su kernel.
Desde esta carpeta:

```sh
sudo insmod esp32_link.ko
ls -l /dev/esp32_link
sudo ./esp32-ctl
```

En la consola, prueba `get` y espera `PONG`. La telemetria llega como lineas
JSON; por defecto el firmware ESP32 transmite continuamente. Comandos:

```text
arm
set CC 1.5
stream 0
stream 1
stop
quit
```

El protocolo de texto coincide con `components/uart_bridge/uart_bridge.c` del
firmware. No ejecutes a la vez otra aplicacion que lea `/dev/esp32_link`: la
cola de recepcion es compartida y el primer lector consume los bytes.

El dashboard opcional `webpage.py` usa el mismo dispositivo y protocolo. Para
ejecutarlo, instala `streamlit` y `pandas` en un entorno Python y abre el
dispositivo con un usuario que tenga permisos de lectura y escritura.

Para sacar el modulo, primero cierra `esp32-ctl` y ejecuta:

```sh
sudo rmmod esp32_link
```

## Diagnostico

```sh
uname -r
readlink -f /dev/serial0
ls -l /dev/esp32_link
sudo dmesg | grep -E 'esp32_link|serdev|ttyAMA'
```

`/dev/serial0` debe apuntar al PL011 (`/dev/ttyAMA0`) en la configuracion
habitual de Pi 3 B+. El driver que se enlaza con el nodo `serdev` es el unico
propietario del UART; no abras `/dev/serial0` con pyserial mientras el modulo
este cargado.

Si `/dev/esp32_link` no aparece, revisa que el overlay figure en
`/boot/firmware/config.txt` (o `/boot/config.txt`), que se haya reiniciado, que
`CONFIG_SERIAL_DEV_BUS` este habilitado en el kernel y que el modulo se haya
cargado sin errores. Para ver si el nodo del ESP32 fue creado:

```sh
find /sys/bus/serial/devices -maxdepth 2 -name of_node -print
sudo dmesg | tail -n 80
```
