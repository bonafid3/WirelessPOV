#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_spi_flash.h>

#include <cmath>

#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <sys/time.h>

#include <rom/ets_sys.h>

#include <nvs_flash.h>
#include <nvs.h>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/timer.h>

#include <esp_event.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h> 

#include <cstring>

#include <string>
#include <map>
#include <list>
#include <iostream>
#include <sstream>

#include "font.h"

#define L1 GPIO_NUM_23
#define L2 GPIO_NUM_22
#define L3 GPIO_NUM_21
#define L4 GPIO_NUM_19
#define L5 GPIO_NUM_18
#define L6 GPIO_NUM_5
#define L7 GPIO_NUM_17
#define HALL GPIO_NUM_16

#define forever while(1)

static struct sockaddr_in bcast;

static EventGroupHandle_t wifi_event_group;
static EventGroupHandle_t interrupt_event_group;

const int CONNECTED_BIT = BIT0;
const int DRDY_BIT = BIT1;

#define COLUMNS 240
#define FONT_HEIGHT 7
#define STORAGE_SIZE COLUMNS*FONT_HEIGHT

std::string gMessage("WIRELESS POV DISPLAY");

static std::map<char, std::string> font;

volatile int gColCnt=0; // actual column to be displayed

volatile int gStoragePos=0; // used for copying
char *gColBuffer = new char[STORAGE_SIZE];

class cFreeRTOS {
public:
  cFreeRTOS(){}
  ~cFreeRTOS(){}
  static void startTask(void task(void *), std::string taskName, void *param=nullptr, int stackSize = 4096) {
    ::xTaskCreate(task, taskName.data(), stackSize, param, 10, NULL);
  }
};

static uint64_t elapsedUSec()
{
    timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

void enableGPIO(gpio_num_t pin) {
  gpio_pad_select_gpio(pin);
  gpio_set_direction(pin, static_cast<gpio_mode_t>(GPIO_MODE_INPUT_OUTPUT));
  gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
  gpio_set_level(pin, 0);
}

void enableIntPin(gpio_num_t pin, gpio_int_type_t int_type)
{
    gpio_intr_disable(pin);

    gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, static_cast<gpio_mode_t>(GPIO_MODE_INPUT));
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_intr_type(pin, int_type);

    gpio_intr_enable(pin);
}

static void IRAM_ATTR inthandler(void *arg){
    uint32_t gpio_intr_status = READ_PERI_REG(GPIO_STATUS_REG);
    uint32_t gpio_intr_status_h = READ_PERI_REG(GPIO_STATUS1_REG);
    SET_PERI_REG_MASK(GPIO_STATUS_W1TC_REG, gpio_intr_status);
    SET_PERI_REG_MASK(GPIO_STATUS1_W1TC_REG, gpio_intr_status_h);

    if((gpio_intr_status & BIT(HALL)) == BIT(HALL)) {
        BaseType_t xHigherPriorityTaskWoken;
        // xHigherPriorityTaskWoken must be initialised to pdFALSE.
        xHigherPriorityTaskWoken = pdFALSE;
        if(xEventGroupSetBitsFromISR(interrupt_event_group, DRDY_BIT, &xHigherPriorityTaskWoken ) == pdPASS) {
            portYIELD_FROM_ISR( );
        }
    }
}

static void timer0Handler(void *arg) {

    TIMERG0.int_clr_timers.t0 = 1;
    TIMERG0.hw_timer[0].update=1;
    TIMERG0.hw_timer[0].config.alarm_en = 1;

    if(gColCnt < COLUMNS) {
        char *colPtr = gColBuffer + gColCnt * FONT_HEIGHT;

        gpio_set_level(L1, colPtr[0]);
        gpio_set_level(L2, colPtr[1]);
        gpio_set_level(L3, colPtr[2]);
        gpio_set_level(L4, colPtr[3]);
        gpio_set_level(L5, colPtr[4]);
        gpio_set_level(L6, colPtr[5]);
        gpio_set_level(L7, colPtr[6]);

        gColCnt++;
    } else {
        gpio_set_level(L1, 0);
        gpio_set_level(L2, 0);
        gpio_set_level(L3, 0);
        gpio_set_level(L4, 0);
        gpio_set_level(L5, 0);
        gpio_set_level(L6, 0);
        gpio_set_level(L7, 0);
    }
}

void copyChar(const char c) {
    std::string chrStr;
    if(font.find(c) != font.end() ) {
        chrStr = font.at(c);
    } else {
        printf("char not found, using the joker!\n");
        chrStr = font.at(0);
    }

    int width = chrStr.size() / FONT_HEIGHT;

    printf("char: %d, width: %d\n", c, width);

    for(int w = width-1; w >=0; w--) {
        for(int h=0; h<FONT_HEIGHT; h++) {
            if(chrStr[w + h*width] == 1) {
                gColBuffer[gStoragePos++] = 1;
            } else {
                gColBuffer[gStoragePos++] = 0;
            }
        }
    }

}

void storeMessageChars() {
    memset(gColBuffer, 0, STORAGE_SIZE);
    gStoragePos = 0;
    
    for(int i=gMessage.size()-1; i>=0; i--) {
        copyChar(gMessage[i]);
    }
}

void mainTask(void *pvParameter)
{
    fontInit(font);

    printf("Main task is running...\n");
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    storeMessageChars();

    uint64_t past = elapsedUSec();
    std::list<uint64_t> deltas;

    std::ostringstream os;

    forever {

        // wait for data ready interrupt
        bool clearOnExit=true, waitForAllBits=true;
        xEventGroupWaitBits(interrupt_event_group, DRDY_BIT, clearOnExit, waitForAllBits, portMAX_DELAY);

        uint64_t now = elapsedUSec();
        uint64_t dt = now - past;
        past = now;

        uint64_t delay = dt / COLUMNS;
        gColCnt = 0;
        timer_pause(TIMER_GROUP_0, TIMER_0);
        timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, delay);
        timer_start(TIMER_GROUP_0, TIMER_0);

/*
        // do some calc after timer started
        uint32_t rpm = (1000000.0f / dt) * 60;
        os.str("");
        os << "RPM:" << rpm << " | DELAY:" << delay;
        message = os.str();
        storeMessageChars();
        */
    }
}

int createAndBindSocket() {
    int socket, ret;

    socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket < 0) {
        printf("Failed to create UDP socket!");
        return -1;
    }
    printf("UDP socket created: %d, bind...\n", socket);

    struct sockaddr_in sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    sock_addr.sin_port = htons(8080);
    ret = bind(socket, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
    if (ret) {
        printf("Failed to bind to UDP socket!\n");
        return -1;
    }
    printf("UDP socket bound!\n");

    return socket;
}

static void writeSocket(void *arg) {
    int socket = *((int*)arg);

    printf("Socket: %d is ready for writing...\n", socket);

    //struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_addr.s_addr = inet_addr("192.168.0.104");
    //bcast.sin_addr.s_addr = inet_addr("255.255.255.255");
    bcast.sin_port = htons(8080);

    int numbytes;

    int failCnt=0;
    forever {
    }
    printf("Delete task: writeSocket...\n");
    vTaskDelete(NULL);
}

static void readSocket(void *arg) {
    int socket = *((int*)arg);
    struct sockaddr_storage client_addr;
    socklen_t n = (socklen_t) sizeof( client_addr );

    printf("Socket: %d is ready for reading...\n", socket);

    #define MAXBUFLEN 1024
    char *buf = new char[MAXBUFLEN]; 
    int numbytes;

    forever {
      memset(buf, 0, MAXBUFLEN);
        if ((numbytes = recvfrom(socket, buf, MAXBUFLEN, 0, (struct sockaddr *)&client_addr, &n)) != -1) {
            if(numbytes == 0) {
                printf("Connection closed gracefully.\n");
            } else {
                struct sockaddr_in *sin = (struct sockaddr_in *)&client_addr;
                printf("Received %d bytes from: %s:%d\n", numbytes, inet_ntoa(sin->sin_addr.s_addr), ntohs(sin->sin_port));
                bcast.sin_addr.s_addr = sin->sin_addr.s_addr;
            
                buf[numbytes] = 0;

                gMessage = buf;
                storeMessageChars();

                //printf("Unknown data from %s:%d: %s\n", inet_ntoa(sin->sin_addr.s_addr), sin->sin_port, buf);
            }
        } else {
            if(xEventGroupGetBits(wifi_event_group) == CONNECTED_BIT) {
            } else {
                printf("Failed to read UDP socket because not connected!\n");
                break;
            }
        }
    }
    delete []buf;
    printf("Deleting task: readSocket...\n");
    vTaskDelete(NULL);
}

static void wifiTask(void *arg)
{
  int sock;
  forever {
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

    while((sock = createAndBindSocket()) == -1) {
      printf("Failed to create socket, retrying...\n");
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    cFreeRTOS::startTask(readSocket, "read socket task", (void*)&sock);
    //cFreeRTOS::startTask(writeSocket, "write socket task", (void*)&sock);

    while(xEventGroupGetBits(wifi_event_group) == CONNECTED_BIT) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    printf("Closing socket %d\n", sock);
    close(sock);
  }
}

static esp_err_t esp32_wifi_eventHandler(void *ctx, system_event_t *event) {
  //int sock;
    switch(event->event_id) {
        case SYSTEM_EVENT_AP_START:
            printf("we are an access point! good!\n");
            //sock = createSocket();
            //xTaskCreate(readSocket, "readSocket", 1024 * 2, (void* ) &sock, 10, NULL);
            //xTaskCreate(writeSocket, "writeSocket", 1024 * 2, (void* ) &sock, 10, NULL);
        break;
        case SYSTEM_EVENT_AP_STACONNECTED:
            printf("Client connected to us\n");

        break;
        case SYSTEM_EVENT_AP_STADISCONNECTED:
            printf("Client disconnected from us!\n");
        break;

    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;

        default:
            //printf("default case event handler: %d\n", event->event_id);
            break;
    }
    return ESP_OK;
}

void wifi_station_init() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    tcpip_adapter_init();

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(esp32_wifi_eventHandler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );

    wifi_config_t wifi_config = { };

    std::string ssid = "Telekom-eea510";
    std::string pass = "YMYTZKHJYXET";

    memcpy(wifi_config.sta.ssid, ssid.c_str(), ssid.size());
    memcpy(wifi_config.sta.password, pass.c_str(), pass.size());

    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

esp_err_t configTimer(timer_group_t tgrp, timer_idx_t tmr, void (*fn)(void*), void * arg)
{
  esp_err_t res;

  timer_config_t timerCfg = { true, // alarm enable
    false, // counter enable
    TIMER_INTR_LEVEL,
    TIMER_COUNT_UP,
    true, // auto reload
    80 }; // divider 1uS

  if((res = timer_init(tgrp, tmr, &timerCfg)) == ESP_OK) {
    printf("TIMER initialized!\n");
  } else {
    printf("TIMER not initialized!\n");
    return res;
  }

  timer_set_alarm_value(tgrp, tmr, 80000000);
  timer_set_counter_value(tgrp, tmr, 0);

  timer_enable_intr(tgrp, tmr);

  timer_isr_handle_t *handle = 0;
  if((res = timer_isr_register(tgrp, tmr, fn, arg, 0, handle)) == ESP_OK) {
    printf("TIMER isr registered!!! :)\n");
  } else {
    printf("TIMER isr not registered!\n");
    return res;
  }

  if((res = timer_start(tgrp, tmr)) == ESP_OK) {
    printf("TIMER started!\n");
  } else {
    printf("TIMER not started!\n");
    return res;
  }
  return ESP_OK;
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    
    interrupt_event_group = xEventGroupCreate();

    enableGPIO(L1);
    enableGPIO(L2);
    enableGPIO(L3);
    enableGPIO(L4);
    enableGPIO(L5);
    enableGPIO(L6);
    enableGPIO(L7);
 
    enableIntPin(HALL, GPIO_INTR_NEGEDGE);
    gpio_isr_register(inthandler, nullptr, 0, 0);

    if(configTimer(TIMER_GROUP_0, TIMER_0, timer0Handler, nullptr) == ESP_OK) {
        printf("timer0 initialized!\n");
    } 

    printf("Timer base clock: %d\n", TIMER_BASE_CLK);

    printf("Starting Main task...\n");
    cFreeRTOS::startTask(mainTask, "Main task");

    wifi_station_init();
    printf("Starting WiFi task...\n");
    cFreeRTOS::startTask(wifiTask, "WiFi task");
}
