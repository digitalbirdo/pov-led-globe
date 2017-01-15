#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include "TMP2.Net.h"

#ifdef ESP8266
extern "C" {
#include "user_interface.h"
#include "osapi.h"
}
#endif

// https://github.com/FastLED/FastLED/wiki
#define FASTLED_ESP8266_RAW_PIN_ORDER
#include "FastLED.h"

#define DISPLAY_Y_SIZE      32
#define DISPLAY_X_SIZE      (DISPLAY_Y_SIZE * 2)
#define CHANNEL_PER_PIXEL   3
#define COLUMN_DATA_MAX     (DISPLAY_Y_SIZE*CHANNEL_PER_PIXEL)
#define DISPLAY_DATA_MAX    (DISPLAY_X_SIZE * DISPLAY_Y_SIZE * CHANNEL_PER_PIXEL)

// Pin definitions
// https://developer.mbed.org/media/uploads/sschocke/xesp8266-pinout_etch_copper_top.png.pagespeed.ic.SiFP37EJYg.png
#define DATA_PIN        0 // GPIO-0
#define CLOCK_PIN       2 // GPIO-2
#define ISR_PIN         3 // GPIO-3 (UART-RX)

//#define ENABLE_WEBSERVER

#define WIFI_SSID      "........"
#define WIFI_PASSWORD  "........"
const char * host =    "ledgobe";

CRGB leds[DISPLAY_Y_SIZE];

#ifdef ENABLE_WEBSERVER
ESP8266WebServer server(80);
#endif

WiFiUDP Udp;

uint16_t framebuffer_len = 0;
unsigned char framebuffer[DISPLAY_DATA_MAX];
uint8_t oldpage =0;

unsigned char incomingPacket[DISPLAY_DATA_MAX+TPM2_NET_HEADER_SIZE+TPM2_FOOTER_SIZE];  // buffer for incoming packets (with some extra space)

volatile boolean syncSignalReceived = false;
uint8_t currentColumn = 0;
volatile boolean displayNextColumn = false;
uint32_t columnDurationUs = 500;
os_timer_t timer;
  
static void ICACHE_FLASH_ATTR tpm2net_recv( unsigned char *data, unsigned short length) ;


#ifdef ENABLE_WEBSERVER
void handleRoot() {
  char temp[400];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;
  
  snprintf ( temp, 400,
    "<html><head><meta http-equiv='refresh' content='5'/>\
    <title>Led Globe</title><style>body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }</style>\
  </head><body><h1>Hello from Led Globe</h1> <p>Uptime: %02d:%02d:%02d</p></body></html>",
    hr, min % 60, sec % 60
  );
  server.send ( 200, "text/html", temp );
}
#endif

void ICACHE_RAM_ATTR columnTimerFunc()
{
  //Serial.println("Timer Call");
  displayNextColumn = true;
}

void setup()
{
  uint8_t i;

  //reset wifi module
  //ESP.eraseConfig();
  //ESP.reset();
  
  system_update_cpu_freq(SYS_CPU_160MHZ);
  
  // Setup Console (TX only, because RX is used for Hall sensor interrupt)
  //Serial.begin(115200,SERIAL_8N1,SERIAL_TX_ONLY);
  Serial.begin(250000,SERIAL_8N1,SERIAL_TX_ONLY);
  Serial.println();

  Serial.println("init Apa102");
  // Defie the connected Leds
  FastLED.addLeds<APA102, DATA_PIN, CLOCK_PIN, BGR>(leds, DISPLAY_Y_SIZE);

  // Limit power usage
  //FastLED.setMaxPowerInVoltsAndMilliamps(5,2000); 

  // Make Leds red to indicate boot
  fill_solid( leds, DISPLAY_Y_SIZE, CRGB::DarkRed );
  FastLED.show();

 
  // Connect Hall Sensor Interrupt
  pinMode(ISR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ISR_PIN), IsrHallSensor, FALLING);


  // Connect to Wifi
  Serial.printf("Connecting to %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected");


  // Open UDP-Port
  Udp.begin(TPM2_NET_PORT);
  Serial.printf("Now listening at IP %s, UDP port %d\n", WiFi.localIP().toString().c_str(), TPM2_NET_PORT);
  

#ifdef ENABLE_WEBSERVER
  // Add a demo Webserver
  server.on("/", handleRoot);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });
  server.begin();
  Serial.println("Webserver started");
#endif

  // Make Leds green to indicate that we are ready
  fill_solid( leds, DISPLAY_Y_SIZE, CRGB::DarkGreen );
  FastLED.show();

  // Timer
  noInterrupts();
  timer1_isr_init();
  timer1_attachInterrupt(columnTimerFunc);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);
  startTimerUs(columnDurationUs);
  interrupts();

   displayNextColumn = true; 
}

void startTimerUs(uint32 us)
{
  //timer0_write(ESP.getCycleCount() + 160*us);
  timer1_write(us*400);
}




void loop()
{
  int packetSize = Udp.parsePacket();
  if (packetSize)
  {
    int len = Udp.read(incomingPacket, 1600);
    if (len > 0 && incomingPacket[0]==TPM2_NET_BLOCK_START_BYTE)
    {
      tpm2net_recv(incomingPacket,len);
    }
  }

  displayLeds();

#ifdef ENABLE_WEBSERVER
  server.handleClient();
#endif
}


// Interrupt service rountine
void IsrHallSensor()
{
  syncSignalReceived =true;
  //Serial.printf("Got Interrupt on Pin %i\n",ISR_PIN);
}

void displayLeds()
{
  uint8_t i;
  if (syncSignalReceived ==true)
  {
    currentColumn = 0;
    syncSignalReceived = false;
    startTimerUs(columnDurationUs);
  }
  
  if(displayNextColumn == true)
  {
    if(currentColumn < DISPLAY_X_SIZE)
    {

      os_memcpy ( &leds[0] ,&framebuffer[DISPLAY_Y_SIZE*3*currentColumn], COLUMN_DATA_MAX);
      currentColumn++;
      displayNextColumn = false;
    }
    else
    {
      fill_solid( leds, DISPLAY_Y_SIZE, CRGB::Black);
      syncSignalReceived=true;
  
    }

    FastLED.show();
    startTimerUs(columnDurationUs);
    yield();


  }
}

static void ICACHE_FLASH_ATTR tpm2net_recv(unsigned char *data, unsigned short length) {

  // header identifier (packet start)
  if ( (data != 0) 
    && (length >= TPM2_NET_HEADER_SIZE)
    && (data[TPM2_BLOCK_START_BYTE_P]==TPM2_NET_BLOCK_START_BYTE) )               
  {   
    uint8_t blocktype = data[TPM2_BLOCK_TYPE_P];                  // block type
    uint16_t framelength = ((uint16_t)data[TPM2_FRAME_SIZE_HIGH_P] << 8) | (uint16_t)data[TPM2_FRAME_SIZE_LOW_P]; // frame length
    uint8_t packagenum = data[TPM2_NET_PACKET_NUM_P];             // packet number 0-255 0x00 = no frame split
    uint8_t numpackages = data[TPM2_NET_PACKET_TOTAL_PACK_NUM_P]; // total packets 1-255
    
    // data command ...
    if (blocktype == TPM2_BLOCK_TYPE_DATA)                                    
    {
      // header end (packet stop)
      if ( (length >= framelength + TPM2_NET_HEADER_SIZE + TPM2_FOOTER_SIZE)
        && (data[TPM2_NET_HEADER_SIZE+framelength]==TPM2_BLOCK_END_BYTE) )            
      {
        
        if (numpackages == 0x01)                                              
        {
          // no frame split found
          os_memcpy ( framebuffer, &data[TPM2_NET_HEADER_SIZE], framelength);               
        }
        else                                                                  
        {
          // frame split is found
          if((oldpage == packagenum-1) || (oldpage == numpackages))
          {
            oldpage = packagenum;
            os_memcpy (&framebuffer[framebuffer_len], &data[TPM2_NET_HEADER_SIZE], framelength);
            framebuffer_len += framelength;

            if (packagenum == numpackages)                                   
            {
              // all packets found
              framebuffer_len = 0;
            }
          }
          else
          {
            // we missed a frame. Start from beginning.
            framebuffer_len = 0;
            Serial.printf("Reset packages: old %i new %i\n", oldpage,packagenum);
            oldpage = numpackages;
          }
        }
      }
    }
  }
}

