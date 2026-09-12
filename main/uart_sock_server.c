#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <errno.h> // Added for errno definition
#include <sys/socket.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/uart.h"

void rx_task(void *arg);

#define WIFI_SSID "<WIFI_SSID>"
#define WIFI_PASSWORD "<PASSWORD>"
#define PORT 8080

#define RX_TASK_STACK_SIZE 4096

#define BUF_SIZE 1024
 char buffer[BUF_SIZE];

static const char *TAG = "Socket_Server";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

typedef struct {
int socket_fd;
}task_params_t;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    /* small delay to avoid a hot reconnect-fail loop */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init(void)
{
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = WIFI_SSID,
          .password = WIFI_PASSWORD,
      },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}

/* Blocks until we actually have an IP, instead of guessing a fixed delay. */
static void wifi_wait_connected(void)
{
  ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
  xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                      pdFALSE, pdTRUE, portMAX_DELAY);
}

void start_socket_server()
{
  esp_err_t err_ret;


  int sock, accepted, ret;

  struct sockaddr_in source_addr;
  struct sockaddr_in dest_addr;
  socklen_t addr_len = sizeof(source_addr);

  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(PORT);

  // step 1: Create a socket
  sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0)
  {
    printf("Unable to create socket: errno %d\n", errno);
    return;
  }

  // step 2: Bind the socket to the port
  err_ret = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err_ret < 0)
  {
    printf("Socket unable to bind: errno %d\n", errno);
    return;
  }

  // step 3: Listen for incoming connections
  ret = listen(sock, 1);
  if (ret < 0)
  {
    printf("Socket unable to listen: errno %d\n", errno);
    return;
  }

  printf("Socket server listening on port %d\n", PORT);

  // step 4: Accept incoming connections
  accepted = accept(sock, (struct sockaddr *)&source_addr, &addr_len);
  if (accepted < 0)
  {
    printf("Unable to accept connection: errno %d\n", errno);
    close(sock);
    return;
  }

  // 3. CORRECT WAY: Allocate memory safely AFTER connection is accepted
  task_params_t *params = malloc(sizeof(task_params_t));
  if (params == NULL)
  {
    printf("Failed to allocate memory for task parameters\n");
    close(accepted);
  }

  // 4. Assign the freshly created file descriptor to the allocated struct
  params->socket_fd = accepted;
  xTaskCreate(rx_task, "uart_rx_task", RX_TASK_STACK_SIZE, (void *)params, 5, NULL);

  while (1)
  {
    // step 5: Receive data from the client
    int bytes_recev = recv(accepted, buffer, BUF_SIZE - 1, 0);
    if (bytes_recev < 0)
    {
      printf("Error occurred during receiving: errno %d\n", errno);
    }
    else if (bytes_recev == 0)
    {
      printf("Connection closed by client\n");
    }
    else
    {
      buffer[bytes_recev] = '\0'; // Null-terminate whatever is received and treat it as a string
      printf("\nReceived %d bytes: %s\n", bytes_recev, (char *)buffer);

    }

    // 5. Create the UART transmission task and pass the parameter pointer
  }

  // step 7: Close the sockets
  close(accepted);
  close(sock);
  return;
}

void rx_task(void *arg)
{
  task_params_t *params = (task_params_t *)arg;
  int accepted = params->socket_fd;

  const uart_port_t uart_num = UART_NUM_0;

  uart_config_t uart_config = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  };

  ESP_ERROR_CHECK(uart_driver_install(uart_num, BUF_SIZE, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));

  uint8_t *data = (uint8_t *)malloc(BUF_SIZE);

  while (1){
    // Read UART data (timeout: 100 ms)
    int len = uart_read_bytes(uart_num, data, (BUF_SIZE - 1), pdMS_TO_TICKS(100));
    if (len > 0)
    {
      data[len] = '\0';                        // Null terminate if treating as text
      int bytes_sent = send(accepted, data, len, 0);
      if (bytes_sent < 0)
      {
        printf("Error occurred during sending: errno %d\n", errno);
        return;
      }
      else
      {
        printf("\nServer sent %d bytes", bytes_sent);
      }
    }
  }
  free(data);
  vTaskDelete(NULL);
}

void app_main(void)
{
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize Wi-Fi
  wifi_init();
  wifi_wait_connected();

  // Start the socket server
  start_socket_server();

  return;
}
