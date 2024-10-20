#include "secrets.h"

// In "utils/server_drv.h" in the WiFiNINA library,
// Header modified to reset watchdog during long connect op.
#include <myWiFiNINA.h>
#include <FlashStorage.h>

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoHttpClient.h>
#include <Arduino_JSON.h>
#include <wdt_samd21.h>

// Constants
#define COLOR_MIN 2900
#define COLOR_MIN_SETTING 143
#define COLOR_MAX 7000
#define COLOR_MAX_SETTING 344
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define LAMP_IP_1 192
#define LAMP_IP_2 168
#define LAMP_IP_3 86
#define LAMP_IP_4 45
#define LAMP_PORT 9123

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
// The pins for I2C are defined by the Wire-library. 
// On an arduino UNO:       A4(SDA), A5(SCL)
// On an arduino MEGA 2560: 20(SDA), 21(SCL)
// On an arduino LEONARDO:   2(SDA),  3(SCL), ...
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define USING_TIMER_TC3         true      // Only TC3 can be used for SAMD51
#define USING_TIMER_TC4         false     // Not to use with Servo library
#define USING_TIMER_TC5         false
#define USING_TIMER_TCC         false
#define USING_TIMER_TCC1        false
#define USING_TIMER_TCC2        false     // Don't use this, can crash on some boards

#include "SAMDTimerInterrupt.h"
#include "SAMD_ISR_Timer.h"

#define WIFI_TIMER TIMER_TC3

// Init selected SAMD timer, used for Wifi connection.
SAMDTimer ITimer(WIFI_TIMER);
SAMD_ISR_Timer ISR_Timer;

// Pin Definitions.
int colorpot = A7;
int brightnesspot = A6; 
int button_select = 2;
int button_right = 3;
int button_left = 9;

// Global state vars.
int color = COLOR_MIN;
int brightness = 100;
int on = 0;
bool changed_setting = false;
int on_setting=-1;
int color_setting=-1;
int brightness_setting=-1;
bool need_screen_refresh=false;
// Wifi vars
char ssid[] = SECRET_SSID;                // your network SSID (name)
char pass[] = SECRET_PASS;                // your network password (use for WPA, or use as key for WEP)
int wifi_status = WL_IDLE_STATUS;
bool need_wifi_connection = false;

WiFiClient wifi_client;
HttpClient* client_ptr;
static char lamp_ip[20];

long last_query = 0;

typedef struct {
  bool valid;
  char ip[4];
  int port;
} ConnectionDetails;
FlashStorage(_s_connection_details,ConnectionDetails);
ConnectionDetails connection_details;

// Graphic symbols:
// 'wifi Connected', 16x16px
const unsigned char icons_wifi_on [] PROGMEM = {
	0xff, 0xff, 0xff, 0xff, 0xf0, 0x0f, 0xc3, 0xc3, 0x9f, 0xf9, 0x38, 0x1c, 0xe3, 0xc7, 0xcf, 0xf3, 
	0xd8, 0x1b, 0xf1, 0x8f, 0xf7, 0xef, 0xfe, 0x7f, 0xfc, 0x3f, 0xfc, 0x3f, 0xff, 0xff, 0xff, 0xff
};
// 'wifi Not connected', 16x16px, white background
const unsigned char icons_wifi_off_w [] PROGMEM = {
	0xff, 0xef, 0xf0, 0x0f, 0xe3, 0x97, 0xcf, 0xb3, 0xf8, 0x3f, 0xf3, 0x4f, 0xfe, 0xff, 0xfc, 0x3f, 
	0xfd, 0xbf, 0xfb, 0xff, 0xfa, 0x7f, 0xf7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
// 'wifi Not connected', 16x16px, black background
const unsigned char icons_wifi_off_b [] PROGMEM = {
	0x00, 0x10, 0x0f, 0xf0, 0x1c, 0x68, 0x30, 0x4c, 0x07, 0xc0, 0x0c, 0xb0, 0x01, 0x00, 0x03, 0xc0, 
	0x02, 0x40, 0x04, 0x00, 0x05, 0x80, 0x08, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
// Power ON, 16x16px
const unsigned char icon_power_on [] PROGMEM = {
	0xfe, 0x7f, 0xfe, 0x7f, 0xe6, 0x67, 0xc6, 0x63, 0x8e, 0x71, 0x9e, 0x79, 0x1e, 0x78, 0x1e, 0x78, 
	0x1e, 0x78, 0x1f, 0xf8, 0x9f, 0xf9, 0x8f, 0xf1, 0xc7, 0xe3, 0xe0, 0x07, 0xf0, 0x0f, 0xfc, 0x3f
};
const unsigned char* nowifi_array[2] = {
	icons_wifi_off_w, 
  icons_wifi_off_b
};
// Tracks which icon is currently displayed
int nowifi_array_idx = 0;
int connection_attempt_duration = 0;

enum display_modes {
  DISPLAY_NORMAL = 0,
  DISPLAY_IP = 1,
  DISPLAY_PORT = 2,
  DISPLAY_SSID = 3,
  DISPLAY_SAVE = 4,
  DISPLAY_RESET = 5
};
const int num_display_modes=6;
int current_display_mode=DISPLAY_NORMAL;
int port_digit_selector=0;
int ip_digit_selector=0;
bool reset_request=false;
bool save_request=false;

void button_select_isr() {
  static unsigned long last_select_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_select_interrupt_time > 200) {
    if (current_display_mode == DISPLAY_NORMAL) {
      if (on_setting == -1) {
        return;
      }
      on_setting ^= 0x01;
      changed_setting = true;
      last_select_interrupt_time = interrupt_time;
    }
    if (current_display_mode == DISPLAY_IP) {
      uint8_t ip_part = connection_details.ip[ip_digit_selector/3];
      int ip_part_digits[3];
      ip_part_digits[2] = ip_part % 10;
      ip_part_digits[1] = (ip_part / 10) % 10;
      ip_part_digits[0] = (ip_part / 100) % 10;
      ip_part_digits[ip_digit_selector % 3] = (ip_part_digits[ip_digit_selector % 3] + 1) % 10;
      ip_part = (ip_part_digits[2] +
                 ip_part_digits[1] * 10 + 
                 ip_part_digits[0] * 100) & 0xFF;
      connection_details.ip[ip_digit_selector/3] = ip_part;
      need_screen_refresh = true;
    }
    if (current_display_mode == DISPLAY_SAVE) {
      save_request = true;
      need_screen_refresh = true;
    }
    if (current_display_mode == DISPLAY_PORT) {
      int current_digits_value[5];
      current_digits_value[4] = connection_details.port % 10;
      current_digits_value[3] = (connection_details.port / 10) % 10;
      current_digits_value[2] = (connection_details.port / 100) % 10;
      current_digits_value[1] = (connection_details.port / 1000) % 10;
      current_digits_value[0] = (connection_details.port / 10000) % 10;
      current_digits_value[port_digit_selector] = ((current_digits_value[port_digit_selector]+1) % 10);
      connection_details.port = (current_digits_value[4] +
                                 current_digits_value[3] * 10 + 
                                 current_digits_value[2] * 100 + 
                                 current_digits_value[1] * 1000 + 
                                 current_digits_value[0] * 10000) & 0xFFFF;
      need_screen_refresh = true;
    }
    if (current_display_mode == DISPLAY_RESET) {
      wdt_reset ();
      reset_request = true;
      need_screen_refresh = true;
    }
    last_select_interrupt_time = interrupt_time;
  }
}

void button_left_isr() {
  static unsigned long last_left_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_left_interrupt_time > 200) {
    current_display_mode = (current_display_mode + 1) % num_display_modes;
    port_digit_selector = 0;
    ip_digit_selector = 0;
    last_left_interrupt_time = interrupt_time;
    need_screen_refresh = true;
  }
}

void button_right_isr() {
  static unsigned long last_right_interrupt_time = 0;
  unsigned long interrupt_time = millis();
  if (interrupt_time - last_right_interrupt_time > 200) {
    if (current_display_mode == DISPLAY_PORT) {
      port_digit_selector = (port_digit_selector + 1) % 5;
    }
    if (current_display_mode == DISPLAY_IP) {
      ip_digit_selector = (ip_digit_selector + 1) % 12;
    }
    last_right_interrupt_time = interrupt_time;
    need_screen_refresh = true;
  }
}

void timer_handler(){
  int old_wifi_status = wifi_status;
  bool wifi_status_changed = old_wifi_status != wifi_status;
  const unsigned char * wifi_icon;
  switch (wifi_status) {
    case WL_CONNECTED:
      wifi_icon = icons_wifi_on;
      break;
    case WL_NO_SSID_AVAIL:
    case WL_SCAN_COMPLETED:
    case WL_IDLE_STATUS:
      nowifi_array_idx ^= 0x1;
      wifi_icon = nowifi_array[nowifi_array_idx];
      break;
    case WL_CONNECT_FAILED:
    case WL_CONNECTION_LOST:
    case WL_DISCONNECTED:
      // Blink.
      nowifi_array_idx ^= 0x1;
      wifi_icon = nowifi_array[nowifi_array_idx];
      if (++connection_attempt_duration > 10) {
        need_wifi_connection = true;
        connection_attempt_duration=0;
      }
      break;
    default:
      wifi_icon = nowifi_array[0];
  }
  display.fillRect(112, 0, 16, 16, SSD1306_BLACK);
  display.drawBitmap(112,0,wifi_icon,16,16,SSD1306_WHITE);
  display.display();
}

// Set the screen based on the values receveid from the lamp.
void set_screen() {
  switch (current_display_mode) {
    case DISPLAY_NORMAL:
      // Clear the areas that needs drawn. Leave the Icons (Wifi + ON/OFF)
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("C: ");
      display.setCursor(0,20);
      display.print("B: ");
      // 90 px bars
      display.fillRect(16,4, (color - COLOR_MIN_SETTING) * 90 / (COLOR_MAX_SETTING - COLOR_MIN_SETTING) ,8, SSD1306_WHITE);
      display.fillRect(16,20, (brightness) * 90 / (100), 8, SSD1306_WHITE);
      // Draw ON/OFF
      if (!on) {
        display.fillRect(112,16,16,16,SSD1306_BLACK);
      } else {
        display.drawBitmap(112,16,icon_power_on,16,16,SSD1306_WHITE);
      }
      break;
    case DISPLAY_IP:
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("Remote IP: ");
      display.setCursor(0,20);
      char padded_lamp_ip[20];
      sprintf(padded_lamp_ip, "%03d.%03d.%03d.%03d",connection_details.ip[0],connection_details.ip[1],connection_details.ip[2],connection_details.ip[3] );
      display.print(padded_lamp_ip);
      display.drawFastHLine(0, 31, 20, SSD1306_BLACK);
      display.drawFastHLine(ip_digit_selector * 6 + (ip_digit_selector/3) * 6, 31, 5, SSD1306_WHITE);
      break;
    case DISPLAY_PORT:
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("Remote Port: ");
      display.setCursor(0,20);
      char zeropadport[6];
      sprintf(zeropadport,"%05d",connection_details.port);
      display.print(zeropadport);
      display.drawFastHLine(0, 31, 20, SSD1306_BLACK);
      display.drawFastHLine(port_digit_selector * 6, 31, 5, SSD1306_WHITE);
      break;
    case DISPLAY_SSID:
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("Wifi SSID: ");
      display.setCursor(0,20);
      display.print(ssid);
      break;
    case DISPLAY_SAVE:
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("Click On to Save");
      if (save_request) {
        display.setCursor(0,20);
        display.print("Saving...");
        delay(500);
      }
      break;
    case DISPLAY_RESET:
      display.fillRect(0,0,112,32,SSD1306_BLACK);
      display.setCursor(0,4);
      display.print("Click On to Reset");
      if (reset_request) {
        display.setCursor(0,20);
        delay(1000);
        display.print("Performing Reset...");
      }
      break;
  }
  display.display();
  need_screen_refresh = false;
}

// Lamp reponse is a JSON that looks like this
// {"numberOfLights":1,"lights":[{"on":1,"brightness":3,"temperature":238}]}
void parse_response(String response) {
      JSONVar lampStatus = JSON.parse(response);
      on = lampStatus["lights"][0]["on"];
      on_setting = on;
      brightness = lampStatus["lights"][0]["brightness"];
      color = lampStatus["lights"][0]["temperature"];
      Serial.print("[numberOfLights] = ");
      Serial.println((int) lampStatus["numberOfLights"]);
      Serial.print("[on] = ");
      Serial.println((int) lampStatus["lights"][0]["on"]);
      Serial.print("[brightness] = ");
      Serial.println((int) lampStatus["lights"][0]["brightness"]);
      Serial.print("[temperature] = ");
      Serial.println((int) lampStatus["lights"][0]["temperature"]);
}

// To set lamp, use a PUT request to /elgato/lights. Content is a JSON like this:
// {"numberOfLights":1,"lights":[{"on":1,"brightness":3,"temperature":238}]}
void set_lamp() {
  JSONVar  doc;
  doc["numberOfLights"] = 1;
  doc["lights"][0]["on"] = on_setting;
  // Pot values are mapped in reverse (because of how the potentiometer is soldered, I wanted increasing values clockwise).
  doc["lights"][0]["brightness"] = map(1023 - brightness_setting,0,1023,0,100);
  doc["lights"][0]["temperature"] = map(1023 - color_setting,0,1023,COLOR_MIN_SETTING,COLOR_MAX_SETTING);
  String payload = JSON.stringify(doc);
  Serial.print("Sending: ");
  Serial.println(payload);
  client_ptr->put("/elgato/lights","applocation/json",payload);
  int statusCode = client_ptr->responseStatusCode();
  String response = client_ptr->responseBody();
  Serial.print("Status code: ");
  Serial.println(statusCode);
  Serial.print("Response: ");
  Serial.println(response);
  // The response confirms the values just set.
  if (statusCode == 200) {
    changed_setting = false;
    parse_response(response);
    set_screen();
  }
}

void setup() {
  // 16s Watchdog.
  wdt_init ( WDT_CONFIG_PER_16K );
  Serial.begin(9600);

  connection_details = _s_connection_details.read();
  if (!connection_details.valid) {
    connection_details.valid = true;
    connection_details.ip[0] = LAMP_IP_1;
    connection_details.ip[1] = LAMP_IP_2;
    connection_details.ip[2] = LAMP_IP_3;
    connection_details.ip[3] = LAMP_IP_4;
    connection_details.port = LAMP_PORT;
    _s_connection_details.write(connection_details);
  }

  sprintf(lamp_ip, "%d.%d.%d.%d",connection_details.ip[0],connection_details.ip[1],connection_details.ip[2],connection_details.ip[3] );
  static HttpClient client = HttpClient(wifi_client, lamp_ip, connection_details.port);
  client.setHttpResponseTimeout(3000);
  client_ptr = &client;
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  // Rotate 180
  display.setRotation(2);
  // Clear the buffer
  display.clearDisplay();
  display.display();

  delay(1000);

  display.setTextSize(1);             // Normal 1:1 pixel scale
  display.setTextColor(SSD1306_WHITE);        // Draw white text
  display.setCursor(0,0);             // Start at top-left corner

  // Pin Settings
  pinMode(colorpot, INPUT);
  pinMode(brightnesspot, INPUT);
  pinMode(button_select, INPUT);
  pinMode(button_left, INPUT);
  pinMode(button_right, INPUT);

  attachInterrupt(digitalPinToInterrupt(button_select) , button_select_isr, RISING);
  attachInterrupt(digitalPinToInterrupt(button_right) , button_right_isr, RISING);
  attachInterrupt(digitalPinToInterrupt(button_left), button_left_isr, RISING);

  ITimer.attachInterruptInterval(1000000, timer_handler);
  
  Serial.print("Remote Client: ");
  Serial.println(lamp_ip);
  Serial.println(connection_details.port);
  Serial.print("Attempting to connect to network: ");
  Serial.println(ssid);
  Serial.println("Connecting.");
  WiFi.setFeedWatchdogFunc(wdt_reset);
  wdt_reset();
  WiFi.begin(ssid, pass);
  delay(10000);
  Serial.println(F("1"));
  wifi_status = WiFi.status();
  need_wifi_connection = false;
  // you're connected now, so print out the data:
  Serial.println(F("You're connected to the network"));
  Serial.println(F("---------------------------------------"));
  wifi_client.setRetry(false);
}

void loop() {
  if (save_request) {
    _s_connection_details.write(connection_details);
    save_request=false;
    need_screen_refresh = true;
  }
  if (need_screen_refresh){
    set_screen();
  }
  while (reset_request) {};
  static long last_query = 0;
  wdt_reset();
  if ((current_display_mode == DISPLAY_NORMAL) && (last_query == 0 || ((millis() - last_query) > 5000))){
    wifi_status = WiFi.status();
    if (need_wifi_connection) {
      WiFi.begin(ssid, pass);
      need_wifi_connection = false;
      return;
    }
    Serial.println(F("Sending request"));
    client_ptr->get("/elgato/lights");
    wdt_reset();
    int statusCode = client_ptr->responseStatusCode();
    String response = client_ptr->responseBody();
    Serial.print("Status code: ");
    Serial.println(statusCode);
    Serial.print("Response: ");
    Serial.println(response);
    // Successful response
    if (statusCode == 200) {
      parse_response(response);
      set_screen();
    }
    last_query = millis();
    wdt_reset();
  }

  // Check for changes in the potentiometer setting.
  int local_color_setting = 0;
  // Average 10 readings to avoid noisy readings.
  for (int i = 0; i < 10; ++i) {
    local_color_setting += analogRead(colorpot);
  }
  local_color_setting /= 10;
  wdt_reset();
  // Same for brightness.
  int local_brightness_setting = 0;
  for (int i = 0; i < 10; ++i) {
    local_brightness_setting += analogRead(brightnesspot);
  }
  wdt_reset();
  local_brightness_setting /= 10;

  // Record local settings until first reply.
  if (on_setting == -1) {
    on_setting = on;
  }
  if (color_setting == -1) {
    color_setting = local_color_setting;
  }
  if (brightness_setting == -1) {
    brightness_setting = local_brightness_setting;
  }

  // Difference between local settings settings and lamp settings.
  int delta_color = (local_color_setting - color_setting);
  int delta_brightness = (local_brightness_setting - brightness_setting);

  // Only react if pot reading  has changed significantly. Avoids flapping.
  if (delta_color > 50 || delta_color < -50) {
    Serial.print("New color: ");
    Serial.print(local_color_setting);
    Serial.print(" delta: ");
    Serial.println(delta_color);
    color_setting = local_color_setting;
    changed_setting = true;
  }

  // Only react if pot reading  has changed significantly. Avoids flapping.
  if (delta_brightness > 50 || delta_brightness < -50) {
    Serial.print("New brightness: ");
    Serial.println(local_brightness_setting);
    Serial.print(" delta: ");
    Serial.println(delta_brightness);
    brightness_setting = local_brightness_setting;
    changed_setting = true;
  }

  // If either of the settings have changed, send a set request.
  if (changed_setting) {
    set_lamp();
  }
  if (need_screen_refresh){
    set_screen();
  }

  wdt_reset();
}
