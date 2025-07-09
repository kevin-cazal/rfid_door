#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <algorithm>

#define MAX_SVC 2
#define MAX_CLIENTS 5
#define MAX_CMD 6
#define SOFT_RESET_BTN 2

/**
 * @brief Represents a configuration command.
 * 
 * This struct holds information about a configuration command, including its name,
 * a function pointer to the command handler, and a help string.
 */
typedef struct {
  String name;
  void (*config_cmd_handler) (WiFiClient &client, String arg);
  String help;
} config_cmd_t;

/**
 * @brief Represents a server object that handles client connections and data.
 */
typedef struct {
  WiFiServer socket;
  WiFiClient clients[MAX_CLIENTS];
  void (*server_data_handler)(void *context);
} server_t;

/**
 * @brief Structure representing the configuration for ESP module.
typedef struct {
    char ssid[32];               < SSID of the Wi-Fi network.
    char psk[64];                < Pre-shared key for the Wi-Fi network.
    unsigned char ip[4];         < IP address of the ESP module.
    unsigned char mask[4];       < Subnet mask for the ESP module.
    unsigned long baudrate;      < Baud rate for serial communication.
} esp_config_t;
*/
typedef struct {
  char ssid[32];
  char psk[64];
  unsigned char ip[4];
  unsigned char mask[4];
  unsigned long baudrate;
} esp_config_t;

void config_server_data_handler(void *context);
void io_server_data_handler(void *context);
void config_cmd_help(WiFiClient &client, String arg);
void config_cmd_set_ssid(WiFiClient &client, String arg);
void config_cmd_set_psk(WiFiClient &client, String arg);
void config_cmd_set_baudrate(WiFiClient &client, String arg);
void config_cmd_save_config(WiFiClient &client, String arg);
void config_cmd_show_config(WiFiClient &client, String arg);
void setup_network();
void setup_io();
void setup_load_config();
void setup_soft_reset();
void setup_services();
void handle_accept(server_t &server);
void handle_command(WiFiClient &client, char buf[512]);
void server_readline(server_t &server);
void clear_eeprom();
String ap_to_str(esp_config_t &cfg);


 esp_config_t default_cfg = {
  .ssid = {'E', 'S', 'P', '_', 'A', 'P', 0},
  .psk = {'1', '2', '3', '4', '5', '6', '7', '8', 0},
  .ip = {172, 16, 0, 1},
  .mask = {255, 255, 255, 0},
  .baudrate = 115200,
};

esp_config_t current_cfg = {0};

server_t services[MAX_SVC] = {
  {.socket = WiFiServer(23), .clients = {WiFiClient()}, .server_data_handler = &io_server_data_handler},
  {.socket = WiFiServer(42), .clients = {WiFiClient()}, .server_data_handler = &config_server_data_handler}
};

config_cmd_t commands[MAX_CMD] = {
  {.name = "help", .config_cmd_handler = config_cmd_help, .help = "HELP"},
  {.name = "set_ssid", .config_cmd_handler = config_cmd_set_ssid, .help = "set_ssid <value>"},
  {.name = "set_psk", .config_cmd_handler = config_cmd_set_psk, .help = "set_psk <value>"},
  {.name = "set_baudrate", .config_cmd_handler = config_cmd_set_baudrate, .help = "set_baudrate <value>"},
  {.name = "save", .config_cmd_handler = config_cmd_save_config, .help = "save"},
  {.name = "show", .config_cmd_handler = config_cmd_show_config, .help = "show"}
};

/**
 * @brief Clears the contents of the EEPROM.
 * 
 * This function clears the contents of the EEPROM by writing 0 to each memory location.
 * After clearing the EEPROM, it commits the changes and ends the EEPROM session.
 */
void clear_eeprom()
{
  EEPROM.begin(512);
  for (int i = 0; i < EEPROM.length(); i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  EEPROM.end();
}

/**
 * Sets the SSID (Service Set Identifier) in the current configuration.
 * 
 * @param client The WiFiClient object.
 * @param arg The SSID to set.
 */
void config_cmd_set_ssid(WiFiClient &client, String arg)
{
  arg.trim();
  if (!arg.length())
    return;

  memset(&current_cfg.ssid, 0, 32);
  memcpy(&current_cfg.ssid, arg.c_str(), arg.length() > 32 ? 32 : arg.length());
}

/**
 * Sets the pre-shared key (PSK) for the WiFi configuration.
 * 
 * @param client The WiFi client object.
 * @param arg The PSK value to set.
 */
void config_cmd_set_psk(WiFiClient &client, String arg)
{
  arg.trim();
  if (arg.length() < 8)
    return;
  memset(&current_cfg.psk, 0, 64);
  memcpy(&current_cfg.psk, arg.c_str(), arg.length() > 64 ? 64 : arg.length());
}

/**
 * Sends the current configuration details to the client.
 * 
 * @param client The WiFiClient object representing the client connection.
 * @param arg The argument passed to the command (not used in this function).
 */
void config_cmd_show_config(WiFiClient &client, String arg)
{
  if (client.availableForWrite()) {
    client.println("SSID: " + String(current_cfg.ssid));
    client.println("PSK: " + String(current_cfg.psk));
    client.println("IP: " + IPAddress(current_cfg.ip).toString());
    client.println("NETMASK: " + IPAddress(current_cfg.mask).toString());
    client.println("BAUDRATE: " + String(current_cfg.baudrate));
  }
}

/**
 * Saves the current configuration to EEPROM and performs setup operations.
 * 
 * @param client The WiFiClient object.
 * @param arg The argument string (not used in this function).
 */
void config_cmd_save_config(WiFiClient &client, String arg)
{
  EEPROM.begin(512);
  EEPROM.put(0, current_cfg);
  EEPROM.commit();
  EEPROM.end();
  delay(1000);
  setup_network();
  delay(1000);
  setup_io();
}

/**
 * Loads the configuration from EEPROM and initializes it.
 * If no configuration is found in EEPROM, the default configuration is used.
 */
void setup_load_config()
{
  EEPROM.begin(512);
  EEPROM.get(0, current_cfg);
  EEPROM.end();
  if (!current_cfg.ssid[0]) {
    memcpy(&current_cfg, &default_cfg, sizeof(esp_config_t));
  }
}

/**
 * Sets the baudrate for the io server.
 * 
 * @param c The WiFi client object.
 * @param arg The baudrate value as a string.
 */
void config_cmd_set_baudrate(WiFiClient &c, String arg)
{
  unsigned long bps = 0;
  
  arg.trim();
  bps = arg.toInt();
  current_cfg.baudrate = bps;
}

/**
 * Sends the help information for all available commands to the client.
 * 
 * @param client The WiFiClient object representing the client connection.
 * @param arg The argument passed to the command (not used in this function).
 */
void config_cmd_help(WiFiClient &client, String arg)
{
  for (int i = 0; i < MAX_CMD; i++) {
    if (client.availableForWrite()) {
      client.println(commands[i].help);
    }
  }
}



/**
 * Handles the incoming command from the WiFi client.
 * 
 * @param client The WiFi client object representing the client connection.
 * @param buf The buffer containing the command received from the client.
 */
void handle_command(WiFiClient &client, char buf[512])
{
  String input = String(buf);
  String cmd;
  String arg;

  input.trim();
  if (!input.length())
    return;
  cmd = input.substring(0, input.indexOf(' '));
  input = input.substring(input.indexOf(' '));
  input.trim();
  arg = input.substring(0, input.indexOf(' '));
  for (int i = 0; i < MAX_CMD; i++) {
    if (commands[i].name == cmd) {
      commands[i].config_cmd_handler(client, arg);
      return;
    }
  }
  Serial.println("Command not found");
}

/**
 * Reads a line of input from each connected client and handles the command.
 * 
 * @param server The server object containing the connected clients.
 */
void server_readline(server_t &server)
{
  char buf[512];
  int index;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    index = 0;
    memset(buf, 0, 512);
    if (!server.clients[i].available())
      continue;
    while (server.clients[i].available() && index < 512) {
      buf[index] = server.clients[i].read();
      if (buf[index] == '\n') {
        handle_command(server.clients[i], buf);
        break;
      }
      index++;
    }
  }
}

/**
 * Handles the server configuration data.
 *
 * This function is responsible for handling the server configuration data.
 * It reads data from the connected clients and performs necessary operations.
 *
 * @param context A pointer to the server context.
 */
void config_server_data_handler(void *context)
{
  server_t server = *((server_t *)context);

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!server.clients[i])
      continue;
    server_readline(server);
  }
}

/**
 * @brief Handles incoming and outgoing data for the server.
 *
 * This function reads data from the Serial port and sends it to all connected clients.
 * It also reads data from the clients and sends it back to the Serial port.
 *
 * @param context A pointer to the server object.
 */
void io_server_data_handler(void *context)
{
  server_t server = *((server_t*)context);
  char in;
  char out[512];
  
  int index = 0;

  if (Serial.available()) {
    in = Serial.read();
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (!server.clients[i])
        continue;
      server.clients[i].write(in);
    }
  }
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!server.clients[i])
        continue;
    memset(out, 0, 512);
    index = 0;
    while (server.clients[i].available() && index < 512) {
      out[index] = server.clients[i].read();
      if (out[index] == '\n') {
        Serial.write(out);
      }
      index++;
    }
  }

}

/**
 * Handles accepting new client connections.
 * 
 * @param server The server object.
 */
void handle_accept(server_t &server)
{
  int i = 0;

  if (!server.socket.hasClient())
    return;
  for (i = 0; i < MAX_CLIENTS; i++) {
    if (!server.clients[i].connected()) {
      server.clients[i] = server.socket.accept();
    }
  }
  if (i >= MAX_CLIENTS)
    server.socket.accept().println("busy");
  delay(1000);
}

/**
 * @brief Sets up the network access point (AP) with the provided configuration.
 * 
 * This function configures the network access point (AP) using the specified IP address, subnet mask,
 * SSID (network name), and password. It sets up the AP with the given IP address and subnet mask,
 * and then sets the SSID and password for the AP.
 * 
 * @note This function assumes that the `current_cfg` structure contains the necessary configuration data.
 */
void setup_network()
{
    WiFi.softAPConfig(IPAddress(current_cfg.ip[0], current_cfg.ip[1], current_cfg.ip[2],current_cfg.ip[3]),
    IPAddress(current_cfg.ip[0], current_cfg.ip[1], current_cfg.ip[2], current_cfg.ip[3]),
    IPAddress(current_cfg.mask[0], current_cfg.mask[1], current_cfg.mask[2], current_cfg.mask[3]));
    WiFi.softAP((const char *)current_cfg.ssid, (const char *)current_cfg.psk);
}

/**
 * Initializes the input/output settings.
 * This function sets up the serial communication with the specified baudrate.
 */
void setup_io()
{
  Serial.end();
  Serial.begin(current_cfg.baudrate);
}

/**
 * @brief Sets up the soft reset functionality.
 * 
 * This function checks if the soft reset button is pressed within the first 5 seconds of program execution.
 * If the button is pressed, it resets the configuration by clearing the EEPROM memory.
 * 
 * @note This function assumes that the SOFT_RESET_BTN pin has been defined and configured properly.
 */
void setup_soft_reset()
{
  int reset = 0;

  pinMode(SOFT_RESET_BTN, INPUT_PULLUP);
    while (millis() < 5000) {
    delay(50);
    reset += digitalRead(SOFT_RESET_BTN);
  }
  if (reset < 50) {
    EEPROM.begin(512);
    EEPROM.write(0, 0);
    EEPROM.commit();
    EEPROM.end();
 }
}

/**
 * @brief Initializes the services for the application.
 * 
 * This function sets up the necessary configurations for the services used in the application.
 * It initializes the sockets and sets the TCP_NODELAY option to true for each service.
 * 
 * @note This function assumes that the services array has been properly initialized.
 */
void setup_services()
{
  for (int i = 0; i < MAX_SVC; i++) {
    services[i].socket.begin();
    services[i].socket.setNoDelay(true);
  }
}

/**
 * @brief Performs the initial setup for the program.
 * 
 * This function calls several setup functions in a specific order to initialize various components of the program.
 * It first sets up the soft reset functionality, then loads the configuration, waits for a delay of 1000 milliseconds,
 * sets up the network, sets up the input/output, waits for another delay of 1000 milliseconds, and finally sets up the services.
 */
void setup()
{
 setup_soft_reset();
 setup_load_config();
 delay(1000);
 setup_network();
 setup_io();
 delay(1000);
 setup_services();
}

/**
 * The main loop function that handles the execution of services.
 * It iterates through the services array, accepts incoming connections,
 * and handles the server data for each service.
 */
void loop()
{
  for (int i = 0; i < MAX_SVC; i++) {
    handle_accept(services[i]);
  }
  for (int i = 0; i < MAX_SVC; i++) {
    services[i].server_data_handler(&services[i]);
  }
}