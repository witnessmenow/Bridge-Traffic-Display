/*******************************************************************
 *  A project to light up leds based on the current traffic       
 *  conditions on the Golden Gate Bridge.                          
 *  Traffic data is being sourced from Google Maps
 *  
 *  Main Hardware:
 *  - ESP8266
 *  - Neopixels
 *                                                                 
 *  Written by Brian Lough                                         
 *******************************************************************/

// ----------------------------
// Standard Libraries
// ----------------------------

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include "FS.h"

// ----------------------------
// Additional libraries - each one of these will need to be installed.
// ----------------------------

#include <WiFiManager.h>
// For configuring the Wifi credentials without re-programing 
// Availalbe on library manager (WiFiManager)
// https://github.com/tzapu/WiFiManager

#include <GoogleMapsApi.h>
// For accessing Google Maps Api
// Availalbe on library manager (GoogleMapsApi)
// https://github.com/witnessmenow/arduino-google-maps-api

#include <ArduinoJson.h>
// For parsing the response from google maps and for the config file
// Available on the library manager (ArduinoJson)
// https://github.com/bblanchon/ArduinoJson

#include <DoubleResetDetector.h>
// For entering Config mode by pressing reset twice
// Not yet available on the library manager
// Go to the github page and there is a download button
// Click Download as zip, and add to Arduino IDE(Sketch->Include Library-> Add .zip library)
// https://github.com/datacute/DoubleResetDetector

#include <Adafruit_NeoPixel.h>
// For controlling the Addressable LEDs
// Available on the library manager (Adafruit Neopixel)
// https://github.com/adafruit/Adafruit_NeoPixel

#include <NTPClient.h>
// For keeping the time, incase we want to do anything based on time
// Available on the library manager (NTPClient)
// https://github.com/arduino-libraries/NTPClient


// The name of the config file stored on SPIFFS, it will create this if it doesn't exist
#define BRIDGE_CONFIG_FILE "bridge.config"

#define GREEN_COLOUR_INDEX 0
#define YELLOW_COLOUR_INDEX 1
#define RED_COLOUR_INDEX 2

// ----------------------------
// Change the following to adapt for you
// ----------------------------

// Pin that your addressable LEDS are connected to (0 is D3 on a Wemos)
#define LED_PIN 0

// Total number of addressable LEDs connected
#define NUMBER_OF_LEDS 20

// Set between 0 and 255, 255 being the brigthest
#define BRIGTHNESS 16

// Server to get the time off
// See here for a list: http://www.pool.ntp.org/en/
const char timeServer[] = "us.pool.ntp.org";

// If the travel time is longer than normal + MEDIUM_TRAFFIC_THRESHOLD, light the route ORANGE
// Value is in seconds
#define MEDIUM_TRAFFIC_THRESHOLD 60

// If the travel time is longer than normal + BAD_TRAFFIC_THRESHOLD, light the route RED
// Value is in seconds (5 * 60 = 300)
#define BAD_TRAFFIC_THRESHOLD 300

// Default Traffic-Matrix API key, you can set this if you want or put it in using the WiFiManager
char apiKey[45] = "";

//Free Google Maps Api only allows for 2500 "elements" a day
unsigned long delayBetweenApiCalls = 1000 * 60; // 1 mins

unsigned long delayBetweenLedChange = 1000 * 10; // 10 seconds


//Where journey should start and end
String origin = "37.8292957,-122.2943918"; // Oakland Side
String destination = "37.7888397,-122.3879729"; // San Fran

// You also may need to change the colours in the getRouteColour method.
// The leds I used seemed to have the Red colour and Green colour swapped in comparison 
// to the library's documentation.

// ----------------------------
// End of area you need to change
// ----------------------------

Adafruit_NeoPixel leds = Adafruit_NeoPixel(NUMBER_OF_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiUDP ntpUDP;

//Probably will need to change the offset for Pacific time
NTPClient timeClient(ntpUDP, timeServer, 3600, 60000);

// Number of seconds after reset during which a 
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

WiFiClientSecure client;
GoogleMapsApi *mapsApi;

unsigned long api_due_time = 0;

unsigned long led_due_time = 0;

uint32_t colour;

int colourIndex = 0;

// flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());

  //We might want to indicate on the LEDS somehow that we are in config mode
  
  drd.stop();
}

void setup() {

  Serial.begin(115200);

  leds.begin(); // This initializes the NeoPixel library.
  leds.setBrightness(BRIGTHNESS);
  unLightAllLeds();

  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  loadConfig();

  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Adding an additional config on the WIFI manager webpage for the API Key
  WiFiManagerParameter customApiKey("apiKey", "API Key", apiKey, 50);
  wifiManager.addParameter(&customApiKey);

  if (drd.detectDoubleReset()) {
    Serial.println("Double Reset Detected");
    wifiManager.startConfigPortal("BayDisplayConf", "thepassword");
  } else {
    Serial.println("No Double Reset Detected");
    wifiManager.autoConnect("BayDisplayConf", "thepassword");
  }

  strcpy(apiKey, customApiKey.getValue());

  if (shouldSaveConfig) {
    saveConfig();
  }

  mapsApi = new GoogleMapsApi(apiKey, client);

  colour = leds.Color(255, 0, 0);

  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  IPAddress ip = WiFi.localIP();
  Serial.println(ip);
  timeClient.begin();
  drd.stop();
}

bool loadConfig() {
  File configFile = SPIFFS.open(BRIDGE_CONFIG_FILE, "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }

  strcpy(apiKey, json["mapsApiKey"]);
  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["mapsApiKey"] = apiKey;

  File configFile = SPIFFS.open(BRIDGE_CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);
  return true;
}

uint32_t getColour(int durationTraffic_value, int duration_value) {

  int difference = durationTraffic_value - duration_value;

  if(difference > BAD_TRAFFIC_THRESHOLD) {
    Serial.println("Setitng Colour to red");
    colourIndex = RED_COLOUR_INDEX;
    return leds.Color(0, 255, 0); // Red
  } else if ( difference > MEDIUM_TRAFFIC_THRESHOLD ) {
    Serial.println("Setitng Colour to yellow");
    colourIndex = YELLOW_COLOUR_INDEX;
    return leds.Color(60, 180, 0); // Yellow
  }
  Serial.println("Setitng Colour to Green");
  colourIndex = GREEN_COLOUR_INDEX;
  return leds.Color(255, 0, 0); //Green
}

void unLightAllLeds() {
  for(int i=0; i< NUMBER_OF_LEDS; i++) {
    leds.setPixelColor(i, leds.Color(0, 0, 0));
  }
  leds.show();
}

void setAllLeds(uint32_t col) {
  for(int i=0; i< NUMBER_OF_LEDS; i++) {
    leds.setPixelColor(i, col);
  }
}

void lightLeds(uint32_t col) {
  unLightAllLeds();
  for (int j = 1; j <= NUMBER_OF_LEDS; j++) {
    for (int i = 0; i < j; i++) {
      leds.setPixelColor(i, col);
    }
    leds.show();
    delay(100);
  }

  // having the double for loop and the leds.show and delay inside the outer one
  // is to create the one after another effect, that i think is quite nice
  // its a good way of indicating which way it is checking traffic and also
  // when it refreshed. We may need to change this based on how the LEDS are wired.
  // comment out leds.show() and the delay from above and uncomment the line below to remove that feature
  //leds.show()
}

void lightLedsForwards(uint32_t newColour, uint32_t oldColour){
  for (int j = 1; j <= NUMBER_OF_LEDS; j++) {
    setAllLeds(oldColour);
    for (int i = 0; i < j; i++) {
      leds.setPixelColor(i, newColour);
    }
    leds.show();
    delay(200);
  }
}

void lightLedsBackwards(uint32_t newColour, uint32_t oldColour){
  for (int j = NUMBER_OF_LEDS - 1; j > 0 ; j--) {
    setAllLeds(oldColour);
    for (int i = NUMBER_OF_LEDS - 1; i >= j; i--) {
      leds.setPixelColor(i, newColour);
    }
    leds.show();
    delay(200);
  }
}

void twinkleLed(){
  uint32_t lighterColour = leds.Color(0, 0, 0);
  switch(colourIndex){
    case GREEN_COLOUR_INDEX:
      {
        lighterColour = leds.Color(80, 0, 0);
      }
      break;
    case YELLOW_COLOUR_INDEX:
      {
        lighterColour = leds.Color(15, 45, 0);
      }
      break;
     
    case RED_COLOUR_INDEX:
      {
        lighterColour = leds.Color(0, 80, 0);
      }
      break;
  }

  lightLedsForwards(lighterColour, colour);
  delay(600);
  lightLedsBackwards(colour, lighterColour);
}

bool checkGoogleMaps() {
  Serial.println("Getting traffic for " + origin + " to " + destination);
    String responseString = mapsApi->distanceMatrix(origin, destination, "now");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& response = jsonBuffer.parseObject(responseString);
    if (response.success()) {
      if (response.containsKey("rows")) {
        JsonObject& element = response["rows"][0]["elements"][0];
        String status = element["status"];
        if(status == "OK") {

          int durationInSeconds = element["duration"]["value"];
          int durationInTrafficInSeconds = element["duration_in_traffic"]["value"];
          colour = getColour(durationInTrafficInSeconds, durationInSeconds);
          Serial.println("Duration In Traffic:  " + durationInTrafficInSeconds);
          return true;

        }
        else {
          Serial.println("Got an error status: " + status);
          return false;
        }
      } else {
        Serial.println("Reponse did not contain rows");
        return false;
      }
    } else {
      if(responseString == ""){
        Serial.println("No response, probably timed out");
      } else {
        Serial.println("Failed to parse Json. Response:");
        Serial.println(responseString);
      }

      return false;
    }

    return false;
}


void loop() {
  unsigned long timeNow = millis();
  if (timeNow > api_due_time)  {
    Serial.println("Checking maps");
    if (checkGoogleMaps()) {
      lightLeds(colour);
    } 
    api_due_time = timeNow + delayBetweenApiCalls;
    led_due_time = timeNow + delayBetweenLedChange;
  }
  timeNow = millis();
  if (timeNow > led_due_time)  {
    Serial.println("Chaning LED");
    twinkleLed();
    led_due_time = timeNow + delayBetweenLedChange;
  }
}
