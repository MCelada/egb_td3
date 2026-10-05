#ifndef _ESP32_LINK_H_
#define _ESP32_LINK_H_

#include <linux/ioctl.h>

#define ESP32_VAR_NAME_MAX 16

struct esp32_var{
    char nombre[ESP32_VAR_NAME_MAX];
    int valor;
}

#define ESP32_MAGIC 'y'
#define ESP32_SET _IOW(ESP32_MAGIC, 1, struct esp32_var)
#define ESP32_GET _IOR(ESP32_MAGIC, 2, struct esp32_var)

#endif
