# ESP32 Wi-Fi TCP Socket Server + UART Bridge

This project connects an ESP32 to a Wi-Fi network and runs a TCP **server**
on it that accepts one client. Once a client connects, incoming UART data
is forwarded to that client (uplink), while anything the client sends over
TCP is received and printed (downlink) — the reverse pairing of the earlier
UART-bridge client sketch.

---

## How it works (high level)

1. `app_main()` initializes NVS storage, brings up Wi-Fi, and waits until an
   IP address is obtained.
2. `start_socket_server()` opens a TCP socket, binds it to `PORT` on all
   interfaces, `listen()`s, and `accept()`s a single client connection.
3. Once a client is connected, a background FreeRTOS task (`rx_task`) is
   spawned to read from UART0 and forward that data to the connected TCP
   client via `send()`.
4. The main task then loops forever, calling `recv()` on the accepted
   socket and printing anything the client sends.

```
 [UART0] --uart_read_bytes--> rx_task --send()--> [TCP client]
 [TCP client] --send()--> recv() loop --printf()--> [console]
```

---

## Configuration constants

| Macro | Purpose |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Wi-Fi credentials the ESP32 connects to as a station. |
| `PORT` | TCP port the ESP32 listens on (`8080`). |
| `BUF_SIZE` | Size (bytes) of the shared receive buffer (`buffer`) and the UART task's buffer. |
| `RX_TASK_STACK_SIZE` | Stack size passed to `xTaskCreate` for `rx_task` (this file actually uses the macro correctly, unlike the client version). |

---

## Function-by-function explanation

### `wifi_event_handler(...)`
Callback registered with the ESP-IDF event loop. It reacts to three events:
- **`WIFI_EVENT_STA_START`** – station mode has started, so it calls
  `esp_wifi_connect()` to begin connecting.
- **`WIFI_EVENT_STA_DISCONNECTED`** – Wi-Fi dropped. Clears the "connected"
  bit in the event group, waits 1 second, then retries
  `esp_wifi_connect()`.
- **`IP_EVENT_STA_GOT_IP`** – DHCP assigned an address. Logs the IP and sets
  `WIFI_CONNECTED_BIT` so other tasks know the network is ready.

### `wifi_init(void)`
Brings up Wi-Fi station mode:
1. Creates the event group used to signal connection state.
2. Initializes the network interface and default event loop.
3. Creates the default Wi-Fi station netif.
4. Initializes the Wi-Fi driver with default config.
5. Registers `wifi_event_handler` for both Wi-Fi and IP events.
6. Applies the SSID/password from the macros.
7. Sets station mode and starts Wi-Fi (`esp_wifi_start()`), which triggers
   `WIFI_EVENT_STA_START` and begins the connection attempt.

### `wifi_wait_connected(void)`
Blocks the calling task using `xEventGroupWaitBits()` until
`WIFI_CONNECTED_BIT` is set — i.e., until a real IP address has been
obtained — instead of guessing a fixed startup delay.

### `start_socket_server(void)`
The TCP server + orchestration logic:
1. Builds a `sockaddr_in` bound to `INADDR_ANY` (all interfaces) on `PORT`.
2. **Create**: `socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)` creates a TCP
   socket.
3. **Bind**: `bind()` attaches the socket to the chosen port.
4. **Listen**: `listen(sock, 1)` puts it into listening mode with a backlog
   of 1.
5. **Accept**: `accept()` blocks until a client connects, returning a new
   descriptor `accepted` for that client.
6. Allocates a `task_params_t` on the heap to carry the accepted socket
   descriptor into `rx_task`.
7. Spawns `rx_task` (the UART→TCP uplink task) via `xTaskCreate`.
8. Enters an infinite loop calling `recv()` on `accepted` and printing
   anything the client sends (downlink: client → ESP32 console).

### `rx_task(void *arg)`
Runs as its own FreeRTOS task. It:
1. Casts `arg` back to `task_params_t*` to recover the accepted socket
   descriptor.
2. Configures and installs the UART0 driver (115200 baud, 8N1, no flow
   control) via `uart_param_config` and `uart_driver_install`.
3. Loops forever: reads up to `BUF_SIZE - 1` bytes from UART with a 100 ms
   timeout (`uart_read_bytes`), and if any bytes were read, forwards them
   to the TCP client with `send()`.

### `app_main(void)`
Entry point called by the ESP-IDF startup code:
1. Initializes NVS flash (erasing/reinitializing it if it's a fresh or
   incompatible version — required by the Wi-Fi driver).
2. Calls `wifi_init()` then `wifi_wait_connected()` to bring up and confirm
   connectivity.
3. Calls `start_socket_server()`, which sets up the listening socket,
   accepts a client, spins up the UART bridge task, and then serves the
   client forever.

---

## Build & flash (ESP-IDF)

```bash
idf.py set-target esp32
idf.py menuconfig      # optional
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Update `WIFI_SSID`/`WIFI_PASSWORD` before building. After flashing, note
the IP printed in the serial monitor, then connect to it:

```bash
nc <esp32-ip> 8080
```

Anything sent to UART0 on the ESP32 should appear at the `nc` client;
anything typed at `nc` should be printed on the ESP32's serial monitor.

---

## Improvements
- **No reconnect logic** — only one client is ever accepted; if it
  disconnects, the server loop and `rx_task` don't handle that gracefully
  or return to `accept()` for a new client.
- **Hardcoded Wi-Fi credentials/port** — fine for a quick test, but move to
  `menuconfig`/NVS for real deployments.
