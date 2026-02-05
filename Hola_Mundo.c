#include <stdio.h>
#include "driver/gpio.h"

#define pin_salida 2

void app_main (void) 
{
printf ("Hola mundo!");

gpio_reset_pin (pin_salida);
gpio_set_direction (pin_salida,GPIO_MODE_OUTPUT);
gpio_set_level (pin_salida, 1);
}

