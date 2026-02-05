#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "freertos/timers.h"

#define salida 2
TimerHandle_t TimerUnaVez;
void call_back (TimerHandle_t xTimer){

printf ("Tiempo agotado \n");
gpio_set_level (salida,1);

}
void app_main (void){

gpio_reset_pin (salida);
gpio_set_direction (salida, GPIO_MODE_OUTPUT);

TimerUnaVez = xTimerCreate (

    "Timer1",
    pdMS_TO_TICKS (7000),
    pdFALSE,
    NULL,
    call_back);

    if (TimerUnaVez != NULL) 
    {
        xTimerStart (TimerUnaVez,0);
    }
}
