#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "esp_err.h"

//MQTT

#define WIFI_SSID      "Docentes_Administrativos"
#define WIFI_PASS      "Adm1N2584km"
#define MQTT_URL       "mqtt://test.mosquitto.org"
#define TOPIC_COMMAND  "puerta/control"
#define TOPIC_STATUS   "puerta/estado"

//Estados

#define Estado_Inicio     0
#define Estado_Abierto    1
#define Estado_Cerrado    2
#define Estado_Abriendo   3
#define Estado_Cerrando   4
#define Estado_Stop       5
#define Estado_Error      6

#define Motor_ON   1
#define Motor_OFF  0
#define Lamp_ON    1
#define Lamp_OFF   0
#define Buzzer_ON  1
#define Buzzer_OFF 0

#define TIMER_LIMITE 300   // 30 segundos 

// Pines

#define pin_ba     2
#define pin_bc     15
#define pin_bs     4
#define pin_be     16
#define pin_pp     17
#define pin_reset  22
#define pin_mc     13
#define pin_ma     14
#define pin_fcc    18
#define pin_fca    19
#define pin_ftc    5
#define pin_lamp   27
#define pin_buzzer 26

// Variables para sensar

struct IO {
    unsigned int fca, fcc, ftc;
    unsigned int bc, ba, bs, be, pp, reset;
    unsigned int mc, ma, lamp, buzzer;
} io;

int Estado_Actual    = Estado_Inicio;
int Estado_Siguiente = Estado_Inicio;
int Estado_Anterior  = -1;

int contador_timer = 0;
int contador_parpadeo = 0;
int señal_parpadeo = 0;

esp_mqtt_client_handle_t mqtt_client;
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Wifi

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        esp_wifi_connect();

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect();

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}



static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    if (event_id == MQTT_EVENT_CONNECTED)
    {
        printf("Conectado a MQTT\n");
        esp_mqtt_client_subscribe(mqtt_client, TOPIC_COMMAND, 0);
    }

    if (event_id == MQTT_EVENT_DATA)
    {
        if (strncmp(event->data, "ABRIR", event->data_len) == 0)
            io.ba = 1;
        else if (strncmp(event->data, "CERRAR", event->data_len) == 0)
            io.bc = 1;
        else if (strncmp(event->data, "STOP", event->data_len) == 0)
            io.bs = 1;
        else if (strncmp(event->data, "RESET", event->data_len) == 0)
            io.reset = 1;
    }
}

// Estados

void EstadoInicio(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_OFF;

    if (io.fca == 0)
        Estado_Siguiente = Estado_Abierto;
    else if (io.fcc == 0)
        Estado_Siguiente = Estado_Cerrado;
    else
        Estado_Siguiente = Estado_Stop;
}

void EstadoAbierto(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_OFF;

    if (io.bc || io.pp)
        Estado_Siguiente = Estado_Cerrando;
}

void EstadoCerrado(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_OFF;

    if (io.ba || io.pp)
        Estado_Siguiente = Estado_Abriendo;
}

void EstadoAbriendo(void)
{
    io.ma = Motor_ON;
    io.mc = Motor_OFF;

    io.lamp = señal_parpadeo;
    io.buzzer = señal_parpadeo;

    contador_timer++;

    if (io.fca == 0)
    {
        contador_timer = 0;
        Estado_Siguiente = Estado_Abierto;
    }
    else if (contador_timer >= TIMER_LIMITE)
    {
        Estado_Siguiente = Estado_Error;
    }
}

void EstadoCerrando(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_ON;

    io.lamp = señal_parpadeo;
    io.buzzer = señal_parpadeo;

    contador_timer++;

    if (io.fcc == 0)
    {
        contador_timer = 0;
        Estado_Siguiente = Estado_Cerrado;
    }
    else if (contador_timer >= TIMER_LIMITE)
    {
        Estado_Siguiente = Estado_Error;
    }
}

void EstadoStop(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_OFF;

    if (io.ba)
        Estado_Siguiente = Estado_Abriendo;
    else if (io.bc)
        Estado_Siguiente = Estado_Cerrando;
}

void EstadoError(void)
{
    io.ma = Motor_OFF;
    io.mc = Motor_OFF;

    io.lamp = señal_parpadeo;
    io.buzzer = señal_parpadeo;

    if (io.reset)
        Estado_Siguiente = Estado_Inicio;
}

// Main

void app_main(void)
{
    
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

   

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    wifi_config_t wifi_config = { 0 };
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);

    printf("WiFi conectado\n");

    

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URL
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    printf("MQTT iniciado\n");

    

    while (true)
    {
        contador_parpadeo++;

        if (contador_parpadeo >= 2)
        {
            señal_parpadeo ^= 1;
            contador_parpadeo = 0;
        }

        if (io.be)
            Estado_Siguiente = Estado_Error;
        else
        {
            if (Estado_Actual == Estado_Inicio)
                EstadoInicio();
            else if (Estado_Actual == Estado_Abriendo)
                EstadoAbriendo();
            else if (Estado_Actual == Estado_Abierto)
                EstadoAbierto();
            else if (Estado_Actual == Estado_Cerrando)
                EstadoCerrando();
            else if (Estado_Actual == Estado_Cerrado)
                EstadoCerrado();
            else if (Estado_Actual == Estado_Stop)
                EstadoStop();
            else if (Estado_Actual == Estado_Error)
                EstadoError();
        }

        if (Estado_Actual != Estado_Anterior)
        {
            char status_text[40];

            if (Estado_Actual == Estado_Abierto)
                strcpy(status_text, "PUERTA ABIERTA");
            else if (Estado_Actual == Estado_Cerrado)
                strcpy(status_text, "PUERTA CERRADA");
            else if (Estado_Actual == Estado_Abriendo)
                strcpy(status_text, "ABRIENDO");
            else if (Estado_Actual == Estado_Cerrando)
                strcpy(status_text, "CERRANDO");
            else if (Estado_Actual == Estado_Stop)
                strcpy(status_text, "STOP");
            else if (Estado_Actual == Estado_Error)
                strcpy(status_text, "ERROR");
            else
                strcpy(status_text, "INICIO");

            printf("Cambio de estado: %s\n", status_text);

            esp_mqtt_client_publish(mqtt_client,
                                    TOPIC_STATUS,
                                    status_text,
                                    0,
                                    1,
                                    0);

            Estado_Anterior = Estado_Actual;
        }

        Estado_Actual = Estado_Siguiente;

        gpio_set_level(pin_ma, io.ma);
        gpio_set_level(pin_mc, io.mc);
        gpio_set_level(pin_lamp, io.lamp);
        gpio_set_level(pin_buzzer, io.buzzer);

        io.ba = 0;
        io.bc = 0;
        io.bs = 0;
        io.reset = 0;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
