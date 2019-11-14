
#define ENABLE_GxEPD2_GFX 0

#include <WiFi.h>
#include <SPIFFS.h>

//Includes for TFT screen
#include <U8g2_for_Adafruit_GFX.h>
#include <Adafruit_ST7789.h> // Hardware-specific library for ST7789

//Includes for temperature
#include <DallasTemperature.h>
#include <OneWire.h>

//Includes for the webserver and captive portal
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPEasyCfg.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

//HW pin definition
#define TFT_CS       5
#define TFT_RST      17
#define TFT_DC       16

#define LED_PIN 22
#define TEMPERATURE_BUS 26
#define BUTTON_A 38
#define BUTTON_B 37
#define BUTTON_C 39
#define EPD_REFRESH 1000

//Display objects
Adafruit_ST7789 display = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

//Temperature measurement objects
OneWire oneWire(TEMPERATURE_BUS);
DallasTemperature sensors(&oneWire);

//Objects for captive portal/MQTT
AsyncWebServer server(80);
ESPEasyCfg captivePortal(&server, "Temperature Monitor");
//Custom application parameters
ESPEasyCfgParameterGroup mqttParamGrp("MQTT");
ESPEasyCfgParameter<String> mqttServer("mqttServer", "MQTT server", "server.local");
ESPEasyCfgParameter<String> mqttUser("mqttUser", "MQTT username", "homeassistant");
ESPEasyCfgParameter<String> mqttPass("mqttPass", "MQTT password", "");
ESPEasyCfgParameter<int> mqttPort("mqttPort", "MQTT port", 1883);
ESPEasyCfgParameter<String> mqttName("mqttName", "MQTT name", "TempMon");

//MQTT objects
WiFiClient espClient;                               // TCP client
PubSubClient client(espClient);                     // MQTT object
const unsigned long postingInterval = 10L * 1000L;  // Delay between updates, in milliseconds
static unsigned long lastPostTime = 0;              // Last time you sent to the server, in milliseconds
static char mqttService[128];                       // Status MQTT service name
uint32_t lastMQTTConAttempt = 0;                    //Last MQTT connection attempt
enum class MQTTConState {Connecting, Connected, Disconnected, NotUsed};
MQTTConState mqttState = MQTTConState::Disconnected;

//Shared variables
float minTemp = 0.0f;
float maxTemp = 0.0f;
float actualTemp = 0.0f;

uint16_t temperatureBox[] = {0,0,0,0};
uint16_t temperatureMinBox[] = {0,0,0,0};
uint16_t temperatureMaxBox[] = {0,0,0,0};
uint16_t infoBox[] = {0,0,0,0};

uint32_t lastUpdate = 0;
float lastDispTemp = 0.0f;
bool updateState = false;
bool previousButtonB = false;
bool showInfo = false;

/**
 * Call back on parameter change
 */
void newState(ESPEasyCfgState state) {
  if(state == ESPEasyCfgState::Reconfigured){
    client.disconnect();
    //Don't use MQTT if server is not filled
    if(mqttServer.getValue().isEmpty()){
      mqttState = MQTTConState::NotUsed;
    }else{
      mqttState = MQTTConState::Connecting;
    }
  }else if(state == ESPEasyCfgState::Connected){
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
  }
  updateState = true;
}

/**
 *  Build the initial TFT screen layout
 **/ 
void initialScreenDisplay()
{
  u8g2Fonts.setFontMode(1);                 // use u8g2 transparent mode (this is default)
  u8g2Fonts.setFontDirection(0);            // left to right (this is default)

  u8g2Fonts.setFont(u8g2_font_inb24_mf);
  uint16_t h = u8g2Fonts.getFontAscent() + u8g2Fonts.getFontDescent() + 10;
  uint16_t separatorPos = h + 10;
  u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
  display.drawFastHLine(0, separatorPos, display.width(), ST77XX_BLUE);
  display.drawFastVLine(display.width()/2, 0, separatorPos, ST77XX_BLUE);
  u8g2Fonts.setCursor(0, h);
  u8g2Fonts.print("Min:");
  u8g2Fonts.setCursor(display.width()/2+5, h);
  u8g2Fonts.print("Max:");

  temperatureMinBox[0] = u8g2Fonts.getUTF8Width("Min:");
  temperatureMinBox[1] = 0;
  temperatureMinBox[2] = u8g2Fonts.getUTF8Width("000");
  temperatureMinBox[3] = separatorPos-7;

  temperatureMaxBox[0] = display.width()/2+5+u8g2Fonts.getUTF8Width("Max:");
  temperatureMaxBox[1] = 0;
  temperatureMaxBox[2] = u8g2Fonts.getUTF8Width("000");
  temperatureMaxBox[3] = separatorPos-7;

  u8g2Fonts.setFont(u8g2_font_inb53_mf);
  uint16_t w = u8g2Fonts.getUTF8Width("°C");
  uint16_t x = display.width()-w-25;
  uint16_t y = separatorPos + u8g2Fonts.getFontAscent() + u8g2Fonts.getFontDescent() + 30;
  u8g2Fonts.setCursor(x, y); // start writing at this position
  u8g2Fonts.print("°C");


  //Save bounds for later
  temperatureBox[0] = 0;
  temperatureBox[1] = separatorPos+10;
  temperatureBox[2] = x;
  temperatureBox[3] = y-temperatureBox[1];

  //Draw a line on bottom of temperature
  display.drawFastHLine(0, y+15, display.width(), ST77XX_BLUE);
  infoBox[0] = 0;
  infoBox[1] = y+16;
  infoBox[2] = display.width();
  infoBox[3] = display.height() - infoBox[1];
}

/**
 * Prints the temperature to a string
 * @param buffer Buffer to save string
 * @param value Value of the temperature
 */
void printTemperature(char* buffer, float value){
  if((value < 10.0f) && (value>0.0f)){
    sprintf(buffer, "%1.2f", value);
  }else if(value>=10.0f){
    if(value<100.0f){
      sprintf(buffer, "%2.1f", value);
    }else{
      sprintf(buffer, "%3f", value);
    }
  }else if(value<-10.0f){
      sprintf(buffer, "%1.1f", value);
  }else if(value>=-10.0f){
      sprintf(buffer, "%2.0f", value);
  }else{
    sprintf(buffer, "%3.0f", value);
  }
}

/**
 * Updates the temperature display
 */
void temperatureDisplay()
{
  u8g2Fonts.setFont(u8g2_font_inb53_mf);
  u8g2Fonts.setBackgroundColor(ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  display.fillRect(temperatureBox[0], temperatureBox[1], temperatureBox[2], temperatureBox[3], ST77XX_BLACK);
  char buffer[10];
  printTemperature(buffer, actualTemp);
  uint16_t w = u8g2Fonts.getUTF8Width(buffer);
  uint16_t x = temperatureBox[2] - w;
  uint16_t y = temperatureBox[1] + temperatureBox[3];
  u8g2Fonts.setCursor(x, y); // start writing at this position
  u8g2Fonts.print(buffer);
}

/**
 * Updates the temperature minimum display
 */
void temperatureMinDisplay(){
  u8g2Fonts.setFont(u8g2_font_inb24_mf);
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setBackgroundColor(ST77XX_BLACK);
  display.fillRect(temperatureMinBox[0], temperatureMinBox[1], temperatureMinBox[2], temperatureMinBox[3], ST77XX_BLACK);
  uint16_t h = u8g2Fonts.getFontAscent() + u8g2Fonts.getFontDescent() + 10;  
  u8g2Fonts.setCursor(temperatureMinBox[0], h);
  char buffer[10];
  sprintf(buffer, "%3d", (int16_t)actualTemp);
  u8g2Fonts.print(buffer);
}

/**
 * Updates the temperature maximum display
 */
void temperatureMaxDisplay(){
  u8g2Fonts.setFont(u8g2_font_inb24_mf);
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setBackgroundColor(ST77XX_BLACK);
  display.fillRect(temperatureMaxBox[0], temperatureMaxBox[1], temperatureMaxBox[2], temperatureMaxBox[3], ST77XX_BLACK);
  uint16_t h = u8g2Fonts.getFontAscent() + u8g2Fonts.getFontDescent() + 10;  
  u8g2Fonts.setCursor(temperatureMaxBox[0], h);
  char buffer[10];
  sprintf(buffer, "%3d", (int16_t)actualTemp);
  u8g2Fonts.print(buffer);
}

/**
 * Formats the MAC address
 * @param str String to append the formatted MAC address
 */
void printMacAddress(String& str){
  //Print MAC address
  byte mac[6];                     // the MAC address of your Wifi shield
  WiFi.macAddress(mac);
  char buffer[20];
  sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
  str += buffer;
}

/**
 * Build and print the information message to
 * TFT display
 */
void printInfoMessages(){
  display.fillRect(infoBox[0], infoBox[1], infoBox[2], infoBox[3], ST77XX_BLACK);
  if(showInfo){
    display.setCursor(infoBox[0], infoBox[1]+5);
    display.setTextSize(2);
    display.setTextWrap(false);
    display.print("MAC : ");
    String mac;
    printMacAddress(mac);
    display.print(mac);
    display.print("\nIP : ");
    display.print(WiFi.localIP().toString());
    display.print("\nMask : ");
    display.print(WiFi.subnetMask().toString());
    display.print("\nGateway : ");
    display.print(WiFi.gatewayIP().toString());
    display.print("\nStatus : ");
    switch(captivePortal.getState()){
      case ESPEasyCfgState::AP:
        display.print("access point");
        break;
      case ESPEasyCfgState::Connected:
        display.print("connected");
        break;
      case ESPEasyCfgState::Connecting:
        display.print("connecting");
        break;
      case ESPEasyCfgState::Reconfigured:
        display.print("reconfigure");
        break;
      case ESPEasyCfgState::WillConnect:
        display.print("will connect");
        break;
    }
    if(mqttState != MQTTConState::NotUsed){
      display.print("\nMQTT state : ");
      switch(mqttState){
        case MQTTConState::Connected:
          display.print("connected");
          break;
        case MQTTConState::Connecting:
          display.print("connecting");
          break;
        case MQTTConState::Disconnected:
          display.print("disconnected");
          break;
        default:
          break;
      }
    }
  }
}

void publishValuesToJSON(String& str){
  StaticJsonDocument<210> root;
  root["temperature"] = actualTemp;
  root["temperatureMin"] = minTemp;
  root["temperatureMax"] = maxTemp;
  serializeJson(root, str);
}

/**
 * MCU setup routine
 */
void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("setup");
  pinMode(BUTTON_A, INPUT);
  pinMode(BUTTON_B, INPUT);
  pinMode(BUTTON_C, INPUT);

   //Temperature initialization
  DeviceAddress sensorDeviceAddress;
  sensors.begin();
  sensors.getAddress(sensorDeviceAddress, 0);
  sensors.setResolution(sensorDeviceAddress, 10);

  //Captive portal initialization
  pinMode(LED_PIN, OUTPUT);
  captivePortal.setLedPin(LED_PIN);
  //Register custom parameters
  mqttPass.setInputType("password");
  mqttParamGrp.add(&mqttServer);
  mqttParamGrp.add(&mqttUser);
  mqttParamGrp.add(&mqttPass);
  mqttParamGrp.add(&mqttPort);
  mqttParamGrp.add(&mqttName);
  captivePortal.addParameterGroup(&mqttParamGrp);
  captivePortal.setStateHandler(newState);
  captivePortal.begin();
  server.begin();

  //MQTT services
  //Build MQTT service name
  snprintf(mqttService, 128, "%s", mqttName.getValue().c_str());
  //Setup MQTT client callbacks and port
  client.setServer(mqttServer.getValue().c_str(), mqttPort.getValue());
  if(mqttServer.getValue().isEmpty()){
    mqttState = MQTTConState::NotUsed;
  }
  //Display initialisation
  display.init(240, 320);
  u8g2Fonts.begin(display);
  display.setRotation(1);
  display.enableDisplay(true);
  display.invertDisplay(false);
  display.fillScreen(ST77XX_BLACK);
  initialScreenDisplay();
  temperatureDisplay();
  printInfoMessages();

  //Serve HTTP pages
  captivePortal.setRootHandler([](AsyncWebServerRequest *request){
      request->redirect("/monitor.html");
    });

  server.on("/values", HTTP_GET, [=](AsyncWebServerRequest *request){
      String json;
      publishValuesToJSON(json);
      AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
      response->addHeader("Access-Control-Allow-Origin", "*");
      request->send(response);
    });
  server.serveStatic("/monitor.html", SPIFFS, "/monitor.html")
        .setCacheControl("public, max-age=31536000").setLastModified("Mon, 04 Mar 2019 07:00:00 GMT");
}

void publishValuesToMQTT(){
  StaticJsonDocument<210> root;
  root["temperature"] = actualTemp;
  root["temperatureMin"] = minTemp;
  root["temperatureMax"] = maxTemp;
  //Publish to MQTT clients
  if(client.connected()){
    String msg;
    serializeJson(root, msg);
    client.publish(mqttService, msg.c_str());
  }
}

void reconnect() {
  //Don't use MQTT if server is not filled
  if(mqttServer.getValue().isEmpty()){
    return;
  }
  // Loop until we're reconnected
  if (!client.connected() && ((millis()-lastMQTTConAttempt)>5000)) {
    mqttState = MQTTConState::Connecting;
    printInfoMessages();    
    IPAddress mqttServerIP;
    int ret = WiFi.hostByName(mqttServer.getValue().c_str(), mqttServerIP);
    if(ret != 1){
      Serial.print("Unable to resolve hostname: ");
      Serial.print(mqttServer.getValue().c_str());
      Serial.println(" try again in 5 seconds");
      lastMQTTConAttempt = millis();
      return;
    }
    Serial.print("Attempting MQTT connection to ");
    Serial.print(mqttServer.getValue().c_str());
    Serial.print(':');
    Serial.print(mqttPort.getValue());
    Serial.print('(');
    Serial.print(mqttServerIP);
    Serial.print(")...");
    // Create a Client ID baased on MAC address
    byte mac[6];                     // the MAC address of your Wifi shield
    WiFi.macAddress(mac);
    String clientId = "Temperature-";
    clientId += String(mac[3], HEX);
    clientId += String(mac[4], HEX);
    clientId += String(mac[5], HEX);
    // Attempt to connect
    client.setServer(mqttServerIP, mqttPort.getValue());
    if((ret == 1) && (client.connect(clientId.c_str(), mqttUser.getValue().c_str(), mqttPass.getValue().c_str()))) {
      Serial.println("connected");
      mqttState = MQTTConState::Connected;
      printInfoMessages();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      client.disconnect();
      mqttState = MQTTConState::Disconnected;
      printInfoMessages();
      lastMQTTConAttempt = millis();
    }
  }
}

/**
 * Arduino loop
 */
void loop()
{
  uint32_t now = millis();
  if(((now-lastUpdate)> EPD_REFRESH)){
    //Time to read the temperature
    if(sensors.requestTemperaturesByIndex(0)){
      float temperature = sensors.getTempCByIndex(0);
      if(temperature < -30.0f){
        return;
      }
      actualTemp = temperature;
      if(((actualTemp<minTemp) && (abs(minTemp-actualTemp) >= 1.0f))
        || (lastUpdate == 0)){
          //First update or min changed enough to be refreshed
          temperatureMinDisplay();
          minTemp = actualTemp;
      }
      if(((actualTemp>maxTemp) && (abs(maxTemp-actualTemp) >= 1.0f))
        || (lastUpdate == 0)){
          //First update or max changed enough to be refreshed
          temperatureMaxDisplay();
          maxTemp = actualTemp;
      }
      if((abs(actualTemp - lastDispTemp) >= 0.1f)
        || (lastUpdate == 0)){
          //First update or temperature changed enough to be refreshed
          temperatureDisplay();
          lastDispTemp = actualTemp;
      }
      lastUpdate = now;
    }
  }
  if(!digitalRead(BUTTON_A)){
    //Reset stats and force new reading now
    Serial.println("Min/Max reset");
    minTemp = 0.0f;
    maxTemp = 0.0f;
    actualTemp = 0.0f;
    lastUpdate = 0;
  }

  //MQTT management
  if (mqttState != MQTTConState::NotUsed){
      if(!client.loop()) {
        //Not connected of problem with updates
        reconnect();
      }else{
        //Ok, we can publish
        if((now-lastPostTime)>postingInterval){
          publishValuesToMQTT();
          lastPostTime = now;
        }
      }
  }

  bool btnState = digitalRead(BUTTON_B);
  if(previousButtonB != btnState){
    previousButtonB = btnState;
    if(!btnState){
      showInfo = !showInfo;
      updateState = true;
    }
  }


  //Update the info message, if needed
  if(updateState){
    printInfoMessages();
    updateState = false;
  }
  yield();
}