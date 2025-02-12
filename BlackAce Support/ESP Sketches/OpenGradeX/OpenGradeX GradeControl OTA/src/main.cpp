/*
  * UDP Grade Control Module Code
  * For OpenGradeX   ONLY WORKS WITH OPENGRADEX NOT REGULAR OPENGRADE
  * 4 Feb 2022, Black Ace 
  * Like all Arduino code - copied from somewhere else
  * So don't claim it as your own
  *
  * Huge Thanks to Brian Tischler For doing all the legwork to make projects like this possible  
  * Check out his Git-Hub https://github.com/farmerbriantee   
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <EEPROM.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>
#include <SPI.h>
#include <Wire.h>

///
/// BUILD VERSION
///
const char *version = "1.4.2.0";

// Function STUBS for Platform IO

// GRADECONTROL
bool SetupGradeControlModule();
bool SetAutoState();
void SetOutput();
void SetValveLimits();

// UDP
bool SetupUdp();
bool SendUdpData(int _header);
bool RecvUdpData();

// WIFI
void ConnectToOGX();
void ReconnectToOGX();

// OTA
void CheckForUpdate();
void checkEEPROM();


/// UDP Variables
WiFiUDP UdpGradeControl;  // Creation of wifi UdpGradeControl instance
const char *ssid = {"OGX"};
#define EEPROM_SIZE 80

char packetBuffer[1460];
uint16_t openGradePort = 9999; //OpenGrade Server Port
uint16_t gradeControlPort = 7777; // GradeControl  Port
uint16_t antennaPort = 8888; // Antenna Port
uint16_t senderPort;
///Ip Addresses
IPAddress openGradeIP(192,168,1,226);   //OpenGradeX Server
IPAddress gradeControlIP(192,168,1,229);   // GradeControl Module IP
IPAddress antennaIP(192,168,1,225);   // Antenna Module IP
IPAddress gatewayIP(192,168,1,1);   // what we want the sp 32 IPAddress to be
IPAddress Subnet(255, 255, 255, 0);
IPAddress Dns(8,8,8,8);
IPAddress senderIP;

///////////////////////PINS///////////////////////
#define SERIAL_BAUD 115200
#define SCL_PIN 22      // I2C SCL PIN
#define SDA_PIN 21      // I2C SCL PIN
#define RXD2 16  // Diagnostic RX
#define TXD2 17 // Diagnostic TX
#define CONST_180_DIVIDED_BY_PI 57.2957795130823
#define DEBUG Serial
#define RTK Serial1

//UDP HEADERS
#define DATA_HEADER 10001
#define SETTINGS_HEADER 10002
#define GPS_HEADER 10003
#define IMU_HEADER 10004
#define RESET_HEADER 10100
#define SYSTEM_HEADER 10101
#define WIFI_HEADER 10102

// Valve Definitions
#define VALVE_FLOAT 2048
#define CNH 0
#define DEERE 1
#define DANFOSS 2
#define CNH_MIN .11
#define CNH_MAX .89
#define DEERE_MIN .11
#define DEERE_MAX .89
#define DANFOSS_MIN .26
#define DANFOSS_MAX .74

/////////// PID SETTINGS////////////
float Kp=45; //Mine was 38
float Ki=.02; //Mine was 0.02
float Kd=3100; //Mine was 2800
float delta_setpoint = 0;  

/////////////PID VALUES/////////////
float PID_p, PID_i, PID_d, PID_total;
float delta_previous_error, delta_error;

/////////////IMU///////////////
char *OG_data[1460];
int16_t dataSize = sizeof(OG_data);


///////////// Com Bytes///////////////
struct Setup {
  uint8_t b_Ki = 0;
  uint8_t b_Kp = 0; 
  uint8_t b_Kd = 0; 
  uint8_t b_retDeadband = 0;
  uint8_t b_extDeadband = 0;
  uint8_t b_valveType = 0;
};  Setup gradeConfig;          //6 bytes


//EEPROM
int16_t EEread = 0;
#define EEP_Ident 0x5417 // Change this number to reset and reload default parameters To EEPROM


//byte b_Ki, b_Kp, b_Kd;
int b_autoState = 0, b_deltaDir = 0, b_cutDelta = 0;
byte b_bladeOffsetOut = 0;
//byte b_retDeadband = 25;
//byte b_extDeadband = 75;
//byte b_valveType = 255;   // 0= CNH    1= Deere     2= Danfoss
byte b_deadband = 0;


/////////////// CNH Valve /////////////////////////
uint16_t analogOutput1 = VALVE_FLOAT; //send to MCP4725
uint16_t analogOutput2 = VALVE_FLOAT; //send to MCP4725
int cut1 = -1;
double voltage = 0; // diagnostic Voltage
double voltage2 = 0;
int retDeadband = 1845;
int extDeadband = 2250;
int retMin = (0.11 * 4096);   //450.56  CNH 
int extMax = (0.89 * 4096);   //3645
bool isAutoActive = false;
bool isCutting = false;

//loop time variables in milliseconds
const u16_t LOOP_TIME = 50; //20hz  
const u16_t LOOP_TIME2 = 30000; //.033HZ    
const u16_t LOOP_TIME3 = 500; //2HZ  

unsigned long lastTime = LOOP_TIME;
unsigned long lastTime2 = LOOP_TIME2;
unsigned long lastTime3 = LOOP_TIME3;  
unsigned long currentTime = 0; 

//Communication with OpenGradeX
bool isOGXConnected = false;
int header = 0, tempHeader = 0;
unsigned long watchdogTimer = 0;   //make sure we are talking to OGX
const int OGXTimeout = 15;      


///////////////////////Initalize Objects///////////////////////
// I2C
TwoWire esp = TwoWire(5); 
// DAC's
Adafruit_MCP4725 Dac1 = Adafruit_MCP4725();
Adafruit_MCP4725 Dac2 = Adafruit_MCP4725();

void setup()
{ 
  checkEEPROM();
  ConnectToOGX();  
  SetupGradeControlModule();  
  CheckForUpdate(); 
 
}
  

void loop(){  //Loop triggers every 50 msec (20hz) and sends back offsets Pid ect

  currentTime = millis();  
  ArduinoOTA.handle();
  SetOutput();  // Run PID Controller
  SetAutoState();  // Set Flags
  RecvUdpData();  // Read Udp Data if Available  
 
  
  if (currentTime - lastTime >= LOOP_TIME) // 10 HZ
  {  
    watchdogTimer++;
    lastTime = currentTime;

    if (watchdogTimer > OGXTimeout){
      isOGXConnected = false;
      digitalWrite(BUILTIN_LED, LOW);
    }    
    else{
      isOGXConnected = true;
      digitalWrite(BUILTIN_LED, HIGH); // make sure connected to OGX   Time
    } 
    (watchdogTimer > OGXTimeout*5000)? watchdogTimer = 50 : watchdogTimer; // Prevent overflow

    SendUdpData(DATA_HEADER);  // Send Data To OpenGradeX    
  }
  
  if (currentTime - lastTime2 >= LOOP_TIME2){ // .33 HZ
    lastTime2 = currentTime;
    ReconnectToOGX();
    SendUdpData(SYSTEM_HEADER);  // Send System info to OenGradeX
  }
  
  if (currentTime - lastTime3 >= LOOP_TIME3){ // 2 HZ
    lastTime3 = currentTime;
  }
}

////////////////
//GRADECONTROL// 
////////////////

bool SetupGradeControlModule()
{

  pinMode(BUILTIN_LED, OUTPUT);  // Initialize the BUILTIN_LED pin as an output
  esp.begin(SDA_PIN , SCL_PIN);
  //set the baud rate
  DEBUG.begin(SERIAL_BAUD);    

  digitalWrite(2, HIGH); delay(500); digitalWrite(2, LOW); delay(500); digitalWrite(2, HIGH); delay(500);
  digitalWrite(2, LOW); delay(500); digitalWrite(2, HIGH); delay(500); digitalWrite(2, LOW); delay(500);

  Dac1.begin(0x62, &esp);
  Dac2.begin(0x63, &esp);

  SetupUdp(); 

  Dac1.setVoltage(VALVE_FLOAT, false);
  Dac2.setVoltage(VALVE_FLOAT, false); 

   

  return true;

}

bool SetAutoState(){
  if (b_deltaDir == 3){
    isCutting = false;
  }
  else{
    isCutting = true;
  }
  
  if (b_autoState == 1){
    isAutoActive = true;
    return true;
  }
  else {
    isAutoActive = false;
    return false;
    
  }     
}

void SetOutput()
{
  if (isAutoActive && isCutting && isOGXConnected){    
    analogOutput1 = VALVE_FLOAT;  
  
    if (b_deltaDir == 0){
      cut1 = -(int)b_cutDelta;
    }  
    else {
      cut1 = (int)b_cutDelta;
    }

    delta_error = (delta_setpoint) - cut1;

    PID_p = Kp * delta_error;// calculate the P errror  
    
    PID_d = Kd*((delta_error - delta_previous_error)/LOOP_TIME);// calculate the d error
    
    if(-b_deadband < delta_error && delta_error < b_deadband){  // 3 cm deadband for i
      PID_i = PID_i + (Ki * delta_error);//calculate the i error
    }
    else{
      PID_i = 0;
    }

    PID_total = PID_p + PID_i + PID_d;

    if (PID_total >  2300) PID_total = 2300;      
    if (PID_total <  -2300) PID_total = -2300;

    if (b_deltaDir == 1){ // Delta is Positive need to lower IMP RETRACT
      analogOutput1 = map(PID_total, 0.0, -2300, retDeadband , retMin);
    }
    else if (b_deltaDir == 0){// Delta is Negative need to raise IMP
      analogOutput1 = map(PID_total,  0.0, 2300, extDeadband, extMax);
    }
    
    if (analogOutput1 >= extMax) analogOutput1 = extMax; // do not exceed 4096
    if (analogOutput1 <= retMin) analogOutput1 = retMin; // do not write negative numbers 
    
    
    if (b_cutDelta < b_deadband){
      analogOutput1 = VALVE_FLOAT;
      voltage = ((double)VALVE_FLOAT/4096) * 5.0;
      voltage2 =((double)VALVE_FLOAT/4096) * 5.0;
    }
    else
    {
      Dac1.setVoltage(analogOutput1, false);  
      voltage = ((double)analogOutput1/4096) * 5.0;
      voltage2 =((double)analogOutput2/4096) * 5.0;
    }
    
    
    
    delta_previous_error = delta_error;
  }
  else{
    
    analogOutput1 = VALVE_FLOAT;
    analogOutput2 = VALVE_FLOAT;
    Dac1.setVoltage(analogOutput1, false);
    Dac2.setVoltage(analogOutput2, false);
    voltage = ((double)analogOutput1/4096) * 5.0;    
    voltage2 =((double)analogOutput2/4096) * 5.0;
                              
  }  
    
}

void SetValveLimits(){

  switch(gradeConfig.b_valveType) {
  
    case CNH:
      retDeadband = VALVE_FLOAT - ((gradeConfig.b_retDeadband/200.0)*4096);
      extDeadband = VALVE_FLOAT + ((gradeConfig.b_extDeadband/200.0)*4096);    
      retMin = (CNH_MIN * 4096);
      extMax = (CNH_MAX * 4096);
      break;
      
    case DEERE:
      retDeadband = VALVE_FLOAT - ((gradeConfig.b_retDeadband/200.0)*4096);
      extDeadband = VALVE_FLOAT + ((gradeConfig.b_extDeadband/200.0)*4096);
      retMin = (DEERE_MIN * 4096);
      extMax = (DEERE_MIN * 4096);
      break;
      
    case DANFOSS:
      retDeadband = VALVE_FLOAT - ((gradeConfig.b_retDeadband/200.0)*4096);
      extDeadband = VALVE_FLOAT + ((gradeConfig.b_extDeadband/200.0)*4096);
      retMin = (DANFOSS_MIN * 4096);
      extMax = (DANFOSS_MAX * 4096);
      break;

    default:
      retDeadband = VALVE_FLOAT - ((gradeConfig.b_retDeadband/200.0)*4096);
      extDeadband = VALVE_FLOAT + ((gradeConfig.b_extDeadband/200.0)*4096);    
      retMin = (CNH_MIN * 4096);
      extMax = (CNH_MAX * 4096);
      
  }
}

///////
//UDP// 
///////

bool SetupUdp(){ 
  
  UdpGradeControl.begin(gradeControlIP, gradeControlPort); //  this UDP address and port  
  return true;
}

bool SendUdpData(int _header)
{ 
  switch (_header){
    case DATA_HEADER:
      //SENDING
      UdpGradeControl.beginPacket(openGradeIP,openGradePort);   //Initiate transmission of data
      UdpGradeControl.print(_header);
      UdpGradeControl.print(",");
      UdpGradeControl.print(b_autoState);
      UdpGradeControl.print(",");    
      UdpGradeControl.print(voltage);
      UdpGradeControl.print(",");    
      UdpGradeControl.print(voltage2);     
      UdpGradeControl.endPacket();  // Close communication        
      break;

    case SETTINGS_HEADER:
    
        break;

    case GPS_HEADER:
        
        break;

    case IMU_HEADER:
        
        break;

    case RESET_HEADER:
        break;

    case SYSTEM_HEADER:
      UdpGradeControl.beginPacket(openGradeIP,openGradePort);   //Initiate transmission of data
      UdpGradeControl.print(_header);
      UdpGradeControl.print(",");
      UdpGradeControl.print(255);
      UdpGradeControl.print(",");
      UdpGradeControl.print(version);     
      UdpGradeControl.endPacket();  // Close communication

      DEBUG.printf("Version sent V%s", version);       
      DEBUG.println();
        break;


    default:
        break; 
  }

  return true;
}

bool RecvUdpData()
{ 
  
  char *strings[1460];
  char *ptr = NULL;  

  //RECEPTION
  int packetSize = UdpGradeControl.parsePacket();   // Size of packet to receive
  
  if (packetSize) {       // If we received a package
    //reset watchdog
    watchdogTimer = 0;
    
    senderIP = UdpGradeControl.remoteIP();  //Sent from IP
    senderPort = UdpGradeControl.remotePort();  //Sent from IP
    //DEBUG.printf("Message Received from IP-> %s Port-> %u ", senderIP.toString(), senderPort);
    
    UdpGradeControl.read(packetBuffer, sizeof(packetBuffer));  

    byte index = 0;
    ptr = strtok(packetBuffer, ",");  // takes a list of delimiters    
    
    while(ptr != NULL)
    {
      strings[index] = ptr;      
      index++;
      ptr = strtok(NULL, ",");  // takes a list of delimiters
    }
    
    for(int n = 0; n < index; n++)
    { 
      OG_data[n] = strings[n];        
    }
    
    // convert to int to read couldnt read PTR fro some rsn   
    header = atoi(OG_data[0]);         
      
    switch (header)
    {
      case DATA_HEADER:
        DEBUG.println("DATA");
        b_deltaDir =  atoi(OG_data[1]);   // Cut Delta Dir
        b_autoState =  atoi(OG_data[2]);    // Cut Delta 
        b_cutDelta =  atoi(OG_data[3]);   // Auto State
        //b_deltaDir =  atoi(OG_data[1]);   // Cut Delta Dir
        //b_autoState =  atoi(OG_data[2]);    // Cut Delta 
        //b_cutDelta =  atoi(OG_data[3]);   // Auto State
        return true;
      break;


      case SETTINGS_HEADER:
        DEBUG.println("SETTINGS FOUND!");      
        gradeConfig.b_Kp = atoi(OG_data[1]);
        gradeConfig.b_Ki = atoi(OG_data[2]);
        gradeConfig.b_Kd = atoi(OG_data[3]);

        gradeConfig.b_retDeadband = atoi(OG_data[4]);
        gradeConfig.b_extDeadband = atoi(OG_data[5]);
        gradeConfig.b_valveType = atoi(OG_data[6]);






        Kp = double(gradeConfig.b_Kp);
        Ki = double(gradeConfig.b_Ki / 100);
        Kd = double(gradeConfig.b_Kp * 100);  

        //store in EEPROM
       

        //b_Kp = atoi(OG_data[1]);
        //b_Ki = atoi(OG_data[2]);
        //b_Kd = atoi(OG_data[3]);
        //b_retDeadband = atoi(OG_data[4]);
        //b_extDeadband = atoi(OG_data[5]);
        //b_valveType = atoi(OG_data[6]);        
        //Kp = double(b_Kp);
        //Ki = double(b_Ki / 100);
        //Kd = double(b_Kp * 100);  
        SetValveLimits(); 
        EEPROM.put(10, gradeConfig);
        EEPROM.commit();
        return true;
      break;

      case SYSTEM_HEADER:     
        
        if (atoi(OG_data[1]) != 0){
          SendUdpData(SYSTEM_HEADER);
        }
      break;

      case RESET_HEADER:
        ESP.restart(); 
        return true;
      break;

      default:
      break;
    }  

    return true;
  }
  return false;
}

////////
//WIFI//
////////
void ConnectToOGX() {
  WiFi.mode(WIFI_STA);
  WiFi.config(gradeControlIP , gatewayIP, Subnet);
  WiFi.begin(ssid);
  DEBUG.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    DEBUG.print('.');
    delay(1000);
  }
  DEBUG.println(WiFi.localIP());
}

void ReconnectToOGX(){
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG.print(millis());
    DEBUG.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();   
  }
}

void checkEEPROM(){

  EEPROM.begin(EEPROM_SIZE);  
  EEPROM.get(0, EEread);            // read identifier
  if (EEread != EEP_Ident)   // check on first start and write EEPROM
  {
    EEPROM.put(0, EEP_Ident);
    EEPROM.put(10,  gradeConfig);    
    EEPROM.commit();    
  }
  else
  {
    EEPROM.get(10, gradeConfig);     // read the Settings    
  }
}
///OTA

void CheckForUpdate(){
  
  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname("OGX_GradeControl");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");  
  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      DEBUG.println("Start updating " + type);
    })
    .onEnd([]() {
      DEBUG.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      DEBUG.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      DEBUG.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();

  DEBUG.println("Ready");
  DEBUG.print("IP address: ");
  DEBUG.println(WiFi.localIP());

}