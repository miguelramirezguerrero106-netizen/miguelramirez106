#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

/* ===================== CONFIGURACIÓN RED ===================== */
#define WIFI_SSID      "Docentes_Administrativos" 
#define WIFI_PASS      "Adm1N2584km" 
#define MQTT_URL       "mqtt://test.mosquitto.org" 
#define TOPIC_COMMAND  "puerta/control"
#define TOPIC_STATUS   "puerta/estado"

static const char *TAG = "SISTEMA_PUERTA";
esp_mqtt_client_handle_t mqtt_client;
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

/* ===================== ESTADOS ===================== */
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

/* ===================== PINES ===================== */
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

struct IO {
    unsigned int fca, fcc, ftc;
    unsigned int bc, ba, bs, be, pp, reset;
    unsigned int mc, ma, lamp, buzzer;
} io;

int Estado_Actual    = Estado_Inicio;
int Estado_Siguiente = Estado_Inicio;
int Estado_Anterior  = -1;
TimerHandle_t TimerMovimiento;

/* ===================== MANEJADORES DE EVENTOS ===================== */

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "¡Conectado a Mosquitto!");
        esp_mqtt_client_subscribe(mqtt_client, TOPIC_COMMAND, 0);
    } else if (event_id == MQTT_EVENT_DATA) {
        if (strncmp(event->data, "ABRIR", event->data_len) == 0) io.ba = 1;
        else if (strncmp(event->data, "CERRAR", event->data_len) == 0) io.bc = 1;
        else if (strncmp(event->data, "STOP", event->data_len) == 0) io.bs = 1;
        else if (strncmp(event->data, "RESET", event->data_len) == 0) io.reset = 1;
    }
}

/* ===================== LÓGICA DE ESTADOS ===================== */
void Timer_Callback(TimerHandle_t xTimer) { 
    ESP_LOGE(TAG, "TIEMPO AGOTADO: La puerta no llegó a su destino");
    Estado_Siguiente = Estado_Error; 
}
void Start_Timer(void) { xTimerStop(TimerMovimiento, 0); xTimerStart(TimerMovimiento, 0); }
void Stop_Timer(void) { xTimerStop(TimerMovimiento, 0); }

void Func_Estado_Inicio(void) {
    io.ma = Motor_OFF; io.mc = Motor_OFF; io.lamp = Lamp_OFF; io.buzzer = Buzzer_OFF;
    if (io.fca == 0) Estado_Siguiente = Estado_Abierto;
    else if (io.fcc == 0) Estado_Siguiente = Estado_Cerrado;
    else Estado_Siguiente = Estado_Stop;
}

void Func_Estado_Abriendo(void) {
    io.ma = Motor_ON; io.mc = Motor_OFF; io.lamp = Lamp_ON; io.buzzer = Buzzer_ON;
    Start_Timer();
    if (io.fca == 0) {
        ESP_LOGI(TAG, "FCA detectado: Puerta abierta por completo");
        Estado_Siguiente = Estado_Abierto;
    } else if (io.bs || io.ftc) {
        Estado_Siguiente = Estado_Stop;
    }
}

void Func_Estado_Abierto(void) {
    io.ma = Motor_OFF; io.mc = Motor_OFF; io.lamp = Lamp_OFF; io.buzzer = Buzzer_OFF;
    Stop_Timer();
    if (io.bc || io.pp) Estado_Siguiente = Estado_Cerrando;
}

void Func_Estado_Cerrando(void) {
    io.ma = Motor_OFF; io.mc = Motor_ON; io.lamp = Lamp_ON; io.buzzer = Buzzer_ON;
    Start_Timer();
    if (io.fcc == 0) {
        ESP_LOGI(TAG, "FCC detectado: Puerta cerrada por completo");
        Estado_Siguiente = Estado_Cerrado;
    } else if (io.bs || io.ftc) {
        Estado_Siguiente = Estado_Stop;
    }
}

void Func_Estado_Cerrado(void) {
    io.ma = Motor_OFF; io.mc = Motor_OFF; io.lamp = Lamp_OFF; io.buzzer = Buzzer_OFF;
    Stop_Timer();
    if (io.ba || io.pp) Estado_Siguiente = Estado_Abriendo;
}

void Func_Estado_Stop(void) {
    io.ma = Motor_OFF; io.mc = Motor_OFF; io.lamp = Lamp_OFF; io.buzzer = Buzzer_OFF;
    Stop_Timer();
    if (io.ba) Estado_Siguiente = Estado_Abriendo;
    else if (io.bc) Estado_Siguiente = Estado_Cerrando;
}

void Func_Estado_Error(void) {
    io.ma = Motor_OFF; io.mc = Motor_OFF;
    io.lamp ^= 1; io.buzzer ^= 1;
    if (io.reset) Estado_Siguiente = Estado_Inicio;
}

/* ===================== MAIN ===================== */
void app_main(void) {
    esp_log_level_set("wifi", ESP_LOG_WARN);
    nvs_flash_init();
    
    // Inicialización WiFi y MQTT
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_URL };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    // GPIO Config: Entradas
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<pin_ba)|(1ULL<<pin_bc)|(1ULL<<pin_bs)|(1ULL<<pin_be)|(1ULL<<pin_pp)|(1ULL<<pin_reset)|(1ULL<<pin_ftc),
        .pull_down_en = 1, .pull_up_en = 0
    };
    gpio_config(&io_conf);

    // Finales de carrera con Pull-Up (activa en 0)
    io_conf.pin_bit_mask = (1ULL<<pin_fca) | (1ULL<<pin_fcc);
    io_conf.pull_down_en = 0; io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // Salidas
    gpio_set_direction(pin_ma, GPIO_MODE_OUTPUT);
    gpio_set_direction(pin_mc, GPIO_MODE_OUTPUT);
    gpio_set_direction(pin_lamp, GPIO_MODE_OUTPUT);
    gpio_set_direction(pin_buzzer, GPIO_MODE_OUTPUT);

    TimerMovimiento = xTimerCreate("TimerMov", pdMS_TO_TICKS(30000), pdFALSE, NULL, Timer_Callback);

    while (true) {
        // Leer Entradas
        if (gpio_get_level(pin_ba)) io.ba = 1;
        if (gpio_get_level(pin_bc)) io.bc = 1;
        if (gpio_get_level(pin_bs)) io.bs = 1;
        if (gpio_get_level(pin_reset)) io.reset = 1;
        io.be = gpio_get_level(pin_be);
        io.pp = gpio_get_level(pin_pp);
        io.fca = gpio_get_level(pin_fca);
        io.fcc = gpio_get_level(pin_fcc);
        io.ftc = gpio_get_level(pin_ftc);

        // Máquina de Estados
        if (io.be) Estado_Siguiente = Estado_Error;
        else {
            switch(Estado_Actual) {
                case Estado_Inicio:   Func_Estado_Inicio(); break;
                case Estado_Abriendo: Func_Estado_Abriendo(); break;
                case Estado_Abierto:  Func_Estado_Abierto(); break;
                case Estado_Cerrando: Func_Estado_Cerrando(); break;
                case Estado_Cerrado:  Func_Estado_Cerrado(); break;
                case Estado_Stop:     Func_Estado_Stop(); break;
                case Estado_Error:    Func_Estado_Error(); break;
            }
        }

        // Publicar y Loguear cambio de estado
        if (Estado_Actual != Estado_Anterior) {
            char status_text[50];
            switch(Estado_Actual) {
                case 1: strcpy(status_text, "PUERTA ABIERTA"); break;
                case 2: strcpy(status_text, "PUERTA CERRADA"); break;
                case 3: strcpy(status_text, "ESTADO: ABRIENDO..."); break;
                case 4: strcpy(status_text, "ESTADO: CERRANDO..."); break;
                case 5: strcpy(status_text, "DETENIDA (STOP)"); break;
                case 6: strcpy(status_text, "¡ALERTA: ERROR!"); break;
                default: strcpy(status_text, "INICIALIZANDO"); break;
            }
            ESP_LOGI(TAG, "Cambio de estado: %s", status_text);
            esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, status_text, 0, 1, 0);
            Estado_Anterior = Estado_Actual;
        }
        
        Estado_Actual = Estado_Siguiente;

        // Actualizar Salidas
        gpio_set_level(pin_ma, io.ma);
        gpio_set_level(pin_mc, io.mc);
        gpio_set_level(pin_lamp, io.lamp);
        gpio_set_level(pin_buzzer, io.buzzer);

        // Limpiar pulsos
        io.ba = 0; io.bc = 0; io.bs = 0; io.reset = 0;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}