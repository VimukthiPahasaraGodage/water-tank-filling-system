#include <WiFi.h>
#include <Preferences.h>

#define pump 27
#define pumpBybassValve 25
#define waterValve 26
#define flowmeter 33
#define waterLevelSenosr 32
#define connectionStatusIndicator 19
#define manualControlIndicator 21

#define SSID "Vimukthi_HAS"
#define PASSWORD "wZtj276QNSyhLabLB2Fkwgwfg2R4beqhxb1S4NhGqZWpveED"

#define flowrateCheckingInterval 10000
#define flowrateNormalizingInterval 30000
#define flowrateMonitorIntervalWhileFilling 20000
#define maxIntervalWithoutSensorModuleConnection 20000
#define maxContinuousWorkingDurationForPump 900000
#define inactiveIntervalAfterStopping 240000

#define flowrateGradient 123.26
#define minimumFlowrate 500 // millilitres per minute 

#define PREFERENCES_NAMESPACE "pref_0"
#define MANUAL_CONTROL_ENABLED_PREFERENCE_KEY "m_c_e"

#define S_TRUE 1
#define S_FALSE 0

#define WATER_LEVEL_REQUEST "level"
#define SERVER_STATUS_FILLING "filling"
#define SERVER_STATUS_STOPPED "stopped"
#define SERVER_STATUS_ERROR "error"
#define SERVER_STATUS_MANUAL "manual_ctrl_server_status"
#define CLIENT_RESET_REQUEST "reset"
#define FILL_REQUEST "fill"
#define STOP_REQUEST "stop"
#define CLIENT_ERROR_STATUS "error"
#define STATUS_REQUEST "status_request_from_sensor_module"
#define CLIENT_RESETTING "resetting"
#define CLIENT_ACK_SERVER_STATUS "status_ack"
#define BROWSER_DISABLE_MANUAL_CTRL "/disable_manual_ctrl"
#define BROWSER_ENABLE_MANUAL_CTRL "/enable_manual_ctrl"
#define BROWSER_RESET_DEVICE "/reset_device"

#define MANUAL_ENABLED_PREFERENCE '1'
#define MANUAL_DISABLED_PREFERENCE '0'

#define TANK_LEVEL_ERROR 10004
#define TANK_FULL 10003
#define TANK_HIGH 10002
#define TANK_MEDIUM 10001
#define TANK_LOW 10000

#define SERVER_ERROR 11003
#define MANUAL_CONTROL 11002
#define TANK_FILLING 11001
#define TANK_NOT_FILLING 11000

#define STATUS_UNDIFINED 9032

WiFiServer server(80);
IPAddress ip(192, 168, 1, 28);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

IPAddress sensorClientIP(192, 168, 1, 27);

TaskHandle_t flowmeterTask;
TaskHandle_t controllerTask;

uint16_t serverStatus = STATUS_UNDIFINED;
uint16_t waterLevel = STATUS_UNDIFINED;

volatile uint8_t flowrate_requested = S_FALSE;
volatile int flowrate = 0;
volatile unsigned long pulse_count = 0;
volatile unsigned long flowrateIndex = 0;

unsigned long checkedFlowrateIndex = 0;
unsigned long monitorFlowrateWhileFillingLastStoppedAt = 0;
unsigned long flowrateCheckingStartTime = 0;
unsigned long flowrateNormalizingStartTime = 0;
unsigned long sensorModuleLastConnectedAt = 0;
unsigned long pumpWorkingStartTime = 0;
unsigned long inactiveTimeAfterStoppingStartTime = 0;
int lastRecordedFlowrate = 0;
uint8_t flowrateCheckingStarted = S_FALSE;
uint8_t sendServerStatus = S_FALSE;
uint8_t waitingFlowNormalizing = S_FALSE;
uint8_t waitingFlowrateValue = S_FALSE;
uint8_t waitingFlowrateValueUsingPump = S_FALSE;
uint8_t requestWaterLevel = S_FALSE;
uint8_t pumpEnabled = S_FALSE;
uint8_t waitingForDeviceReset = S_FALSE;
uint8_t waitingForClientReset = S_FALSE;
uint8_t monitorFlowrateWhileFilling = S_FALSE;
uint8_t firstCommunicationSinceBooting = S_FALSE;
uint8_t inactiveTimeAfterStoppingCounterStarted = S_FALSE;

SemaphoreHandle_t controlSemaphore;
Preferences preferences;

String messageToSensorModule = "";
const String messageSeperator = "@";

void createSemaphore(){
    controlSemaphore = xSemaphoreCreateMutex();
    xSemaphoreGive( ( controlSemaphore) );
}

// Lock the variable indefinietly. ( wait for it to be accessible )
void lockVariables(){
    xSemaphoreTake(controlSemaphore, portMAX_DELAY);
}

// give back the semaphore.
void unlockVariables(){
    xSemaphoreGive(controlSemaphore);
}

void restartServer() {
  ESP.restart();
}

bool checkCorrectWaterFilledInPipe(){
  uint8_t result = digitalRead(waterLevelSenosr);
  if (result == LOW) {
    return true;
  } else {
    return false;
  }
}

void stopFilling(){
  digitalWrite(pump, HIGH);
  digitalWrite(pumpBybassValve, HIGH);
  digitalWrite(waterValve, HIGH);
  pumpEnabled = S_FALSE;
}

void fillWithoutPump() {
  digitalWrite(waterValve, LOW);
  digitalWrite(pumpBybassValve, LOW);
  digitalWrite(pump, HIGH);
  pumpEnabled = S_FALSE;
}

void fillUsingPump() {
  digitalWrite(waterValve, LOW);
  digitalWrite(pumpBybassValve, HIGH);
  digitalWrite(pump, LOW);
  pumpEnabled = S_TRUE;
  pumpWorkingStartTime = millis();
}

void changeServerStatus(uint16_t status){
  if (serverStatus == TANK_FILLING && status == TANK_NOT_FILLING) {
    waitingFlowNormalizing = S_FALSE;
    waitingFlowrateValue = S_FALSE;
    waitingFlowrateValueUsingPump = S_FALSE;
    monitorFlowrateWhileFilling = S_FALSE;
  }

  if (status == TANK_NOT_FILLING) {
    if (inactiveTimeAfterStoppingCounterStarted == S_FALSE) {
      inactiveTimeAfterStoppingStartTime = millis();
      inactiveTimeAfterStoppingCounterStarted = S_TRUE;
    }
  }

  serverStatus = status;
  sendServerStatus = S_TRUE;
  requestWaterLevel = S_TRUE;

  if (serverStatus == MANUAL_CONTROL) {
    digitalWrite(manualControlIndicator, HIGH);
  } else {
    digitalWrite(manualControlIndicator, LOW);
  } 
}

void IRAM_ATTR ISR() {
  pulse_count++;
}

void webpage(WiFiClient browser) {
  browser.println("HTTP/1.1 200 OK");
  browser.println("Content-Type: text/html");
  browser.println("");

  browser.println("<!DOCTYPE HTML><html><head> <meta charset='UTF-8'> <meta content='width=device-width, initial-scale=1.0'> <title>Water Tank Filling System</title> <style> h1 { text-align: center; } #div_main { padding: 3%; margin: 3%; background-color: white; } #manual_btn { background-color: darkmagenta; padding: 0.75%; color: white; font-size: large; margin-right: 5%; } #reset_btn { background-color: red; padding: 0.75%; color: white; font-size: large; } p { font-style: oblique; color: darkblue; font-size: larger; } </style></head><body style='background-color: darkgray;'> <div id='div_main'> <h1>Water Tank Filling System</h1> <hr> <div> ");
  
  if (serverStatus == MANUAL_CONTROL) {
    browser.println("<a href='/disable_manual_ctrl'> <button id='manual_btn'>Disable Manual Controll</button> </a> </div> <hr> ");
  } else {
    browser.println("<a href='/enable_manual_ctrl'> <button id='manual_btn'>Enable Manual Controll</button> </a>");

    if (waitingForDeviceReset == S_FALSE) {
      browser.println("<a href='/reset_device'> <button id='reset_btn'>Reset Device</button> </a> </div> <hr> ");
    } else {
      browser.println("<a href='/reset_device'> <button id='reset_btn'>Resetting...</button> </a> </div> <hr> ");
    }
  }

  if (serverStatus != MANUAL_CONTROL) {
    browser.println("<h2>Operation Status</h2> ");
    if (serverStatus == TANK_NOT_FILLING) { 
      browser.println("<p>Water Tank Filling is Stopped</p> <hr> ");
    }  else if (serverStatus == TANK_FILLING) {
      if (pumpEnabled == S_TRUE) {
        browser.println("<p>Filling the Water Tank using the Pump</p> <hr> ");
      } else {
        browser.println("<p>Filling the Water Tank without using the Pump</p> <hr> ");
      }
    } else if (serverStatus == SERVER_ERROR) {
      browser.println("<p>Unexpected Error Occurred! Please Reset the Device</p> <hr> ");
    } else if (serverStatus == STATUS_UNDIFINED) {
      browser.println("<p>Operation Status Undifined! Please Refresh the Page After Some Time</p> <hr> ");
    }

    browser.println("<h2>Water Level in Tank</h2> ");
    if (waterLevel == TANK_FULL) {
      browser.println("<p>Full</p> <hr> ");
    } else if (waterLevel == TANK_HIGH) {
      browser.println("<p>High</p> <hr> ");
    } else if (waterLevel == TANK_MEDIUM) {
      browser.println("<p>Medium</p> <hr> ");
    } else if (waterLevel == TANK_LOW) {
      browser.println("<p>LOW</p> <hr> ");
    } else if (waterLevel == TANK_LEVEL_ERROR) {
      browser.println("<p>Error Detecting Tank Water Level! Please Check the Problem</p> <hr> ");
    } else if (waterLevel == STATUS_UNDIFINED) {
      browser.println("<p>Water Level Undifined! Please Refresh the Page After Some Time</p> <hr> ");
    }

    browser.println("<h2>Flowrate of Water</h2> ");
    if (serverStatus == TANK_FILLING) {
      browser.println("<p>" + String(lastRecordedFlowrate) + " millilitres per minute</p> <hr> ");
    } else {
      browser.println("<p>0 millilitres per minute</p> <hr> ");
    }
  }

  browser.println("</div></body></html>");
  browser.stop();
}

void initMessageToSend() {
  messageToSensorModule = "";
}

void createMessageToSend(String msg) {
  messageToSensorModule += (msg + messageSeperator);
}

void controllerTaskCode( void * pvParameters ){
  for(;;){
    if (WiFi.status() != WL_CONNECTED) {
      restartServer();
    }

    initMessageToSend();

    WiFiClient client = server.available();
    client.setTimeout(100);
    if (client && client.connected()) {
      sensorModuleLastConnectedAt = millis();
      String msg = client.readStringUntil('\r');
      if (client.remoteIP() == sensorClientIP) {
        if (firstCommunicationSinceBooting == S_TRUE) {
          msg = "";
          firstCommunicationSinceBooting = S_FALSE;
          // Serial.println("Initiating the communication with sensor module!");
        }
        if (msg != "" && msg != "@") {
          // Serial.println("Message from sensor client: " + msg);
          if (msg.indexOf(STATUS_REQUEST) >= 0) {
            sendServerStatus = S_TRUE;
            requestWaterLevel = S_TRUE;
          }
          if (msg.indexOf(FILL_REQUEST) >= 0) {
            if ((millis() - inactiveTimeAfterStoppingStartTime) > inactiveIntervalAfterStopping) {
              changeServerStatus(TANK_FILLING);
              inactiveTimeAfterStoppingCounterStarted = S_FALSE;
            }
            // Serial.println("Fill request from client. Changing the server status to TANK_FILLING");
          }
          if (msg.indexOf(STOP_REQUEST) >= 0) {
            changeServerStatus(TANK_NOT_FILLING);
            // Serial.println("Stop request from client. Changing the server status to TANK_NOT_FILLING");
          }
          if (msg.indexOf(CLIENT_ERROR_STATUS) >= 0) {
            if (serverStatus == TANK_FILLING) {
              changeServerStatus(TANK_NOT_FILLING);
              // Serial.println("Tank water level error. Changing the server status to TANK_NOT_FILLING");
            } else {
              sendServerStatus = S_TRUE;
              requestWaterLevel = S_TRUE;
            }
          }
          if (requestWaterLevel == S_TRUE && msg.indexOf(String(TANK_FULL)) >= 0) {
            waterLevel = TANK_FULL;
            requestWaterLevel = S_FALSE;
            // Serial.println("Tank water level: Full");
          }
          if (requestWaterLevel == S_TRUE && msg.indexOf(String(TANK_HIGH)) >= 0) {
            waterLevel = TANK_HIGH;
            requestWaterLevel = S_FALSE;
            // Serial.println("Tank water level: High");
          }
          if (requestWaterLevel == S_TRUE && msg.indexOf(String(TANK_MEDIUM)) >= 0) {
            waterLevel = TANK_MEDIUM;
            requestWaterLevel = S_FALSE;
            // Serial.println("Tank water level: Medium");
          }
          if (requestWaterLevel == S_TRUE && msg.indexOf(String(TANK_LOW)) >= 0) {
            waterLevel = TANK_LOW;
            requestWaterLevel = S_FALSE;
            // Serial.println("Tank water level: Low");
          }
          if (requestWaterLevel == S_TRUE && msg.indexOf(String(TANK_LEVEL_ERROR)) >= 0) {
            waterLevel = TANK_LEVEL_ERROR;
            requestWaterLevel = S_FALSE;
            // Serial.println("Tank water level: Error");
          }
          if (msg.indexOf(CLIENT_RESETTING) >= 0 && waitingForClientReset == S_TRUE && waitingForDeviceReset == S_TRUE) {
            // Serial.println("Client is resetting! Starting resetting the server");
            restartServer();
          }
          if (msg.indexOf(CLIENT_ACK_SERVER_STATUS) >= 0 && sendServerStatus == S_TRUE) {
            sendServerStatus = S_FALSE;
            // Serial.println("Server status acknowledged by the sensor module");
          }
        }

        if (sendServerStatus == S_TRUE) {
          if (serverStatus == SERVER_ERROR) {
            createMessageToSend(SERVER_STATUS_ERROR);
          } else if (serverStatus == MANUAL_CONTROL) {
            createMessageToSend(SERVER_STATUS_MANUAL);
          } else if (serverStatus == TANK_FILLING) {
            createMessageToSend(SERVER_STATUS_FILLING);
          } else if (serverStatus == TANK_NOT_FILLING) {
            createMessageToSend(SERVER_STATUS_STOPPED);
          } 
        }

        if (requestWaterLevel == S_TRUE) {
          createMessageToSend(WATER_LEVEL_REQUEST);
        }

        if (waitingForDeviceReset) {
          createMessageToSend(CLIENT_RESET_REQUEST);
          waitingForClientReset = S_TRUE;
        }
        
        if (messageToSensorModule != "" && messageToSensorModule != "@") {
          // Serial.println("Message to Sensor Module: " + messageToSensorModule);
        }
        client.println(messageToSensorModule + "\r");
        client.stop();
      } else {
        // Serial.println("Message from browser client: " + msg);
        if (msg.indexOf(BROWSER_RESET_DEVICE) >= 0 && waitingForDeviceReset == S_FALSE) {
          waitingForDeviceReset = S_TRUE;
          // Serial.println("Initiating device resetting...");
        } else if (msg.indexOf(BROWSER_DISABLE_MANUAL_CTRL) >= 0 && serverStatus == MANUAL_CONTROL) {
          changeServerStatus(TANK_NOT_FILLING);
          preferences.putChar(MANUAL_CONTROL_ENABLED_PREFERENCE_KEY, MANUAL_DISABLED_PREFERENCE);
          // Serial.println("Disabling the manual control");
        } else if (msg.indexOf(BROWSER_ENABLE_MANUAL_CTRL) >= 0 && serverStatus != MANUAL_CONTROL) {
          changeServerStatus(MANUAL_CONTROL);
          preferences.putChar(MANUAL_CONTROL_ENABLED_PREFERENCE_KEY, MANUAL_ENABLED_PREFERENCE);
          // Serial.println("Enabling the manual control");
        }

        webpage(client);
      }
    } else {
      if ((millis() - sensorModuleLastConnectedAt) > maxIntervalWithoutSensorModuleConnection) {
        // Serial.println("Sensor module not responding. Restarting the server...");
        restartServer();
      }
    }

    if (serverStatus == TANK_FILLING) {
      // // Serial.println("waitingFlowNormalizing: " + String(waitingFlowNormalizing) + "; waitingFlowrateValue: " + String(waitingFlowrateValue) + "; waitingFlowrateValueUsingPump: " + String(waitingFlowrateValueUsingPump) + "; monitorFlowrateWhileFilling: " + String(monitorFlowrateWhileFilling));
      // // Serial.println("checkedFlowrateIndex: " + String(checkedFlowrateIndex) + "; flowrateIndex: " + String(flowrateIndex) + "; flowrate_requested: " + String(flowrate_requested));
      if (waitingFlowNormalizing == S_FALSE && waitingFlowrateValue == S_FALSE && waitingFlowrateValueUsingPump == S_FALSE && monitorFlowrateWhileFilling == S_FALSE){
        fillWithoutPump();
        waitingFlowNormalizing = S_TRUE;
        flowrateNormalizingStartTime = millis();
        // Serial.println("Server status: Tank Filling; Opening the valves and wait till the flow normalize");
      } else if (waitingFlowNormalizing == S_TRUE && waitingFlowrateValue == S_FALSE && waitingFlowrateValueUsingPump == S_FALSE && monitorFlowrateWhileFilling == S_FALSE) {
        if ((millis() - flowrateNormalizingStartTime) > flowrateNormalizingInterval) {
          waitingFlowNormalizing = S_FALSE;
          waitingFlowrateValue = S_TRUE;
          // Serial.println("Server status: Tank Filling; Flow normalized. Getting the flowrate");
        }
      } else if (waitingFlowNormalizing == S_FALSE && waitingFlowrateValue == S_TRUE && waitingFlowrateValueUsingPump == S_FALSE && monitorFlowrateWhileFilling == S_FALSE) {
        lockVariables();
        if (checkedFlowrateIndex < flowrateIndex  && flowrate_requested == S_FALSE) {
          // Serial.println("Flowrate: " + String(flowrate) + " ml/min");
          if (checkCorrectWaterFilledInPipe()) {
            if (flowrate < minimumFlowrate) {
              fillUsingPump();
              waitingFlowrateValueUsingPump = S_TRUE;
              // Serial.println("Server status: Tank Filling; Flowrate is insufficient. Starting the pump");
            } else {
              monitorFlowrateWhileFilling = S_TRUE;
              // Serial.println("Server status: Tank Filling; Flowrate is sufficient. Filling without using the pump");
            }
          } else {
            changeServerStatus(TANK_NOT_FILLING);
            // Serial.println("Server status: Tank Filling Stopping; Flowrate is insufficient. But the pump cannot be started because the water level is not desirable");
          }
          lastRecordedFlowrate = flowrate;
          checkedFlowrateIndex = flowrateIndex;
          waitingFlowrateValue = S_FALSE;
        } else if (checkedFlowrateIndex == flowrateIndex  && flowrate_requested == S_FALSE) {
          flowrate_requested = S_TRUE;
        }
        unlockVariables();
      } else if (waitingFlowNormalizing == S_FALSE && waitingFlowrateValue == S_FALSE && waitingFlowrateValueUsingPump == S_TRUE && monitorFlowrateWhileFilling == S_FALSE) {
        lockVariables();
        if (checkedFlowrateIndex < flowrateIndex  && flowrate_requested == S_FALSE) {
          // Serial.println("Flowrate: " + String(flowrate) + " ml/min");
          if (flowrate >= minimumFlowrate && checkCorrectWaterFilledInPipe()) {
            monitorFlowrateWhileFilling = S_TRUE;
            // Serial.println("Server status: Tank Filling; Flowrate is sufficient. Filling the tank using the pump");
          } else {
            changeServerStatus(TANK_NOT_FILLING);
            // Serial.println("Server status: Tank Filling Stopping; Flowrate is insufficient even if we use the pump");
          }
          lastRecordedFlowrate = flowrate;
          checkedFlowrateIndex = flowrateIndex;
          waitingFlowrateValueUsingPump = S_FALSE;
        } else if (checkedFlowrateIndex == flowrateIndex  && flowrate_requested == S_FALSE) {
          flowrate_requested = S_TRUE;
        }
        unlockVariables();
      } else if (waitingFlowNormalizing == S_FALSE && waitingFlowrateValue == S_FALSE && waitingFlowrateValueUsingPump == S_FALSE && monitorFlowrateWhileFilling == S_TRUE) {
        lockVariables();
        if (checkedFlowrateIndex < flowrateIndex && flowrate_requested == S_FALSE) {
          // Serial.println("Flowrate: " + String(flowrate) + " ml/min");
          if (flowrate >= minimumFlowrate && checkCorrectWaterFilledInPipe()) {
            if (pumpEnabled == S_TRUE && (millis() - pumpWorkingStartTime) > maxContinuousWorkingDurationForPump) {
              changeServerStatus(TANK_NOT_FILLING);
              // Serial.println("Server status: Tank Filling Stopping; Maximum continuous duration for pump has been reached");
            } else {
              // Serial.println("Server status: Tank Filling; Flowrate is sufficient to keep the water tank filling going");
            }
          } else {
            changeServerStatus(TANK_NOT_FILLING);
            // Serial.println("Server status: Tank Filling Stopping; Flowrate is insufficient to keep filling the tank");
          }
          lastRecordedFlowrate = flowrate;
          checkedFlowrateIndex = flowrateIndex;
          monitorFlowrateWhileFillingLastStoppedAt = millis();
        } else if (checkedFlowrateIndex == flowrateIndex && flowrate_requested == S_FALSE && ((millis() - monitorFlowrateWhileFillingLastStoppedAt) > flowrateMonitorIntervalWhileFilling)) {
          flowrate_requested = S_TRUE;
        }
        unlockVariables();
      } else {
        changeServerStatus(TANK_NOT_FILLING);
      }
    } else if (serverStatus == TANK_NOT_FILLING || serverStatus == MANUAL_CONTROL) {
      stopFilling();
    }
  }
}

void flowmeterTaskCode( void * pvParameters ){
  for(;;){
    lockVariables();
    if (flowrate_requested == S_TRUE && flowrateCheckingStarted == S_FALSE) {
      pulse_count = 0;
      flowrateCheckingStarted = S_TRUE;
      flowrateCheckingStartTime = millis();
      attachInterrupt(flowmeter, ISR, RISING);
    } else if (flowrate_requested == S_TRUE && flowrateCheckingStarted == S_TRUE) {
      unsigned long difference = millis() - flowrateCheckingStartTime;
      if (difference > flowrateCheckingInterval) {
        detachInterrupt(flowmeter);
        float frequency = (pulse_count * 1000) / difference;
        flowrate = (int) (frequency * flowrateGradient);
        flowrateIndex++;
        flowrate_requested = S_FALSE;
        flowrateCheckingStarted = S_FALSE;
      }
    }
    unlockVariables();
  }
}

void setup() {
  pinMode(pump, OUTPUT);
  pinMode(pumpBybassValve, OUTPUT);
  pinMode(waterValve, OUTPUT);
  pinMode(connectionStatusIndicator, OUTPUT);
  pinMode(manualControlIndicator, OUTPUT);

  digitalWrite(pump, HIGH);
  digitalWrite(pumpBybassValve, HIGH);
  digitalWrite(waterValve, HIGH);
  digitalWrite(connectionStatusIndicator, LOW);
  digitalWrite(manualControlIndicator, LOW);

  pinMode(flowmeter, INPUT);
  pinMode(waterLevelSenosr, INPUT);

  Serial.begin(115200);
  delay(1000);

  WiFi.config(ip, gateway, subnet);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    // Serial.println("Connecting to Wi-Fi...");
    digitalWrite(connectionStatusIndicator, HIGH);
    delay(250);
    digitalWrite(connectionStatusIndicator, LOW);
    delay(250);
  }
  digitalWrite(connectionStatusIndicator, HIGH);
  // Serial.println("Connected to Wi-Fi! IP address : " + String(WiFi.localIP()));

  preferences.begin(PREFERENCES_NAMESPACE, false);
  char savedServerStatus = preferences.getChar(MANUAL_CONTROL_ENABLED_PREFERENCE_KEY, MANUAL_DISABLED_PREFERENCE);
  // Serial.println("savedServerStatus: " + String(savedServerStatus));
  if (savedServerStatus == MANUAL_ENABLED_PREFERENCE) {
    changeServerStatus(MANUAL_CONTROL);
  } else {
    changeServerStatus(TANK_NOT_FILLING);
  }
  firstCommunicationSinceBooting = S_TRUE;

  server.begin();

  createSemaphore();

  //create a task that will be executed in the Task1code() function, with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
                    flowmeterTaskCode,   /* Task function. */
                    "flowmeterTask",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &flowmeterTask,      /* Task handle to keep track of created task */
                    1);          /* pin task to core 0 */                  

  //create a task that will be executed in the Task2code() function, with priority 1 and executed on core 1
  xTaskCreatePinnedToCore(
                    controllerTaskCode,   /* Task function. */
                    "controllerTask",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &controllerTask,      /* Task handle to keep track of created task */
                    1);          /* pin task to core 1 */
}

void loop() {
  
}
