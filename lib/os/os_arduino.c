#include "os.h"

#include <Arduino.h>
#include <WiFi.h>

int os_sync_with_console(const char *wireless_interface, uint16_t code)
{
    // TODO: Connect with WPS
}

int os_connect_to_console(const char *wireless_interface, int (*callback)(void *), void *context);
{
    WiFi.begin("", "");
}