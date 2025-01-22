#include <WiFi.h>

#define lowLevelSensor 34
#define refillInitiationLevelSensor 35
#define maximumLevelSensor 32

#define connectionStatusIndicator 21

#define waterLevelCheckInterval 3000
#define inactiveIntervalAfterStopping 180000

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

#define S_TRUE 1
#define S_FALSE 0

#define REQUESTING_WATER_LEVEL "level"
#define SERVER_STATUS_FILLING "filling"
#define SERVER_STATUS_STOPPED "stopped"
#define SERVER_STATUS_ERROR "error"
#define SERVER_STATUS_MANUAL "manual_ctrl_server_status"
#define RESET_REQUEST "reset"
#define SEND_FILL_REQUEST "fill"
#define SEND_STOP_REQUEST "stop"
#define SEND_ERROR_STATUS "error"
#define SEND_STATUS_REQUEST "status_request_from_sensor_module"
#define CLIENT_RESETTING "resetting"
#define SERVER_STATUS_ACK "status_ack"

#define SSID "Vimukthi_HAS"
#define PASSWORD "wZtj276QNSyhLabLB2Fkwgwfg2R4beqhxb1S4NhGqZWpveED"

WiFiClient server;
IPAddress serverIP(192, 168, 1, 28); // IP Address of the master module
const uint16_t port = 80;

IPAddress ip(192, 168, 1, 27);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

SemaphoreHandle_t waterLevelSemaphore;
void createSemaphore(){
    waterLevelSemaphore = xSemaphoreCreateMutex();
    xSemaphoreGive( ( waterLevelSemaphore) );
}

// Lock the variable indefinietly. ( wait for it to be accessible )
void lockVariables(){
    xSemaphoreTake(waterLevelSemaphore, portMAX_DELAY);
}

// give back the semaphore.
void unlockVariables(){
    xSemaphoreGive(waterLevelSemaphore);
}

TaskHandle_t communicationTask;
TaskHandle_t waterLevelCheckingTask;

// 0 - water level is below lowest sensor level
// 1 - water level is between the lowest and the middle sensor levels
// 2 - water level is between the upper and lowest sensor levels
// 3 - water level is in the upper sensor level(tank is filled)
// 4 - error
volatile uint16_t waterLevel = STATUS_UNDIFINED;  
volatile unsigned long waterLevelCheckIndex = 0;

uint16_t previousSentWaterLevel = STATUS_UNDIFINED;

// 0 - tank is not filling
// 1 - tank is filling
// 2 - error
uint16_t serverStatus = STATUS_UNDIFINED;

unsigned long previousWaterLevelCheck = 0;
unsigned long sentWaterLevelCheckIndex = 0;
unsigned long inactiveTimeAfterStoppingStartTime = 0;

String messageToServer = "";
String messageSeperator = "@";

uint8_t waitingForResettingInitializing = S_FALSE;
uint8_t inactiveTimeAfterStoppingCounterStarted = S_FALSE;

void initMessageToSend() {
  messageToServer = "";
}

void createMessageToSend(String msg) {
  messageToServer += (msg + messageSeperator);
}

void setup() {
  pinMode (lowLevelSensor, INPUT);
  pinMode (refillInitiationLevelSensor, INPUT);
  pinMode (maximumLevelSensor, INPUT);

  pinMode(connectionStatusIndicator, OUTPUT);

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

  createSemaphore();

  //create a task that will be executed in the Task1code() function, with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
                    communicationTaskCode,   /* Task function. */
                    "communicationTask",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &communicationTask,      /* Task handle to keep track of created task */
                    1);          /* pin task to core 0 */                  

  //create a task that will be executed in the Task2code() function, with priority 1 and executed on core 1
  xTaskCreatePinnedToCore(
                    waterLevelCheckingTaskCode,   /* Task function. */
                    "waterLevelCheckingTask",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &waterLevelCheckingTask,      /* Task handle to keep track of created task */
                    1);          /* pin task to core 1 */
}

void communicationTaskCode( void * pvParameters ){
  for(;;){
    if (WiFi.status() != WL_CONNECTED) {
      restartClient();
    }

    if (server.connect(serverIP, port)) {
      if (messageToServer != "" && messageToServer != "@") {
        // Serial.println("Message to Server: " + messageToServer);
      }
      server.println(messageToServer + "\r");

      initMessageToSend();

      if (waitingForResettingInitializing) {
        delay(5);
        restartClient();
      }

      String msg = server.readStringUntil('\r');
      server.flush();
      if (msg != "" && msg != "@"){
        // Serial.println("Message from server: " + msg);
        if (msg.indexOf(REQUESTING_WATER_LEVEL) >= 0 && waterLevelCheckIndex > sentWaterLevelCheckIndex) {
          lockVariables();
          createMessageToSend(String(waterLevel));
          sentWaterLevelCheckIndex = waterLevelCheckIndex;
          previousSentWaterLevel = waterLevel;
          // Serial.println("Sent the water level to server: " + String(waterLevel));
          unlockVariables();
        }
        if (msg.indexOf(SERVER_STATUS_FILLING) >= 0) {
          serverStatus = TANK_FILLING;
          createMessageToSend(SERVER_STATUS_ACK);
          // Serial.println("Server status: Filling the water tank");
        }
        if (msg.indexOf(SERVER_STATUS_STOPPED) >= 0) {
          serverStatus = TANK_NOT_FILLING;
          createMessageToSend(SERVER_STATUS_ACK);
          if (inactiveTimeAfterStoppingCounterStarted == S_FALSE) {
            inactiveTimeAfterStoppingStartTime = millis();
            inactiveTimeAfterStoppingCounterStarted = S_TRUE;
          }
          // Serial.println("Server status: Not filling the water tank");
        }
        if (msg.indexOf(SERVER_STATUS_ERROR) >= 0) {
          serverStatus = SERVER_ERROR;
          createMessageToSend(SERVER_STATUS_ACK);
          // Serial.println("Server status: Error");
        }
        if (msg.indexOf(SERVER_STATUS_MANUAL) >= 0) {
          serverStatus = MANUAL_CONTROL;
          createMessageToSend(SERVER_STATUS_ACK);
          // Serial.println("Server status: Manual Controll Enabled");
        }
        if (msg.indexOf(RESET_REQUEST) >= 0) {
          createMessageToSend(CLIENT_RESETTING);
          waitingForResettingInitializing = S_TRUE;
          // Serial.println("Inform server that the tank sensor module is resetting...");
        }
      }

      if (serverStatus == STATUS_UNDIFINED) {
        createMessageToSend(SEND_STATUS_REQUEST);
      }

      lockVariables();
      // Serial.println("serverStatus: " + String(serverStatus));
      if (serverStatus == TANK_NOT_FILLING && waterLevel < TANK_HIGH) {
        if ((millis() - inactiveTimeAfterStoppingStartTime) > inactiveIntervalAfterStopping) {
          createMessageToSend(SEND_FILL_REQUEST);
          inactiveTimeAfterStoppingCounterStarted = S_FALSE;
          // Serial.println("Sending fill request to server");
        }
      } else if (serverStatus == TANK_FILLING && waterLevel == TANK_FULL) {
        createMessageToSend(SEND_STOP_REQUEST);
        // Serial.println("Sending stop request to server");
      } else if ((serverStatus == TANK_FILLING || serverStatus == TANK_NOT_FILLING) && waterLevel == TANK_LEVEL_ERROR) {
        createMessageToSend(SEND_ERROR_STATUS);
        // Serial.println("Sending water level error status to server");
      }

      if ((serverStatus == TANK_FILLING || serverStatus == TANK_NOT_FILLING) && previousSentWaterLevel != waterLevel) {
        createMessageToSend(SEND_STATUS_REQUEST);
      }
      unlockVariables();
    }
  }
}

void restartClient() {
  ESP.restart();
}

void waterLevelCheckingTaskCode( void * pvParameters ){
  for(;;){
    if ((millis() - previousWaterLevelCheck) > waterLevelCheckInterval) {
      lockVariables();
      uint8_t lowLevelSensorOutput = digitalRead(lowLevelSensor);
      uint8_t refillInitiationLevelSensorOutput = digitalRead(refillInitiationLevelSensor);
      uint8_t maximumLevelSensorOutput = digitalRead(maximumLevelSensor);
      if (lowLevelSensorOutput == HIGH && refillInitiationLevelSensorOutput == HIGH && maximumLevelSensorOutput == HIGH) {
        waterLevel = TANK_LOW;
      } else if (lowLevelSensorOutput == LOW && refillInitiationLevelSensorOutput == HIGH && maximumLevelSensorOutput == HIGH) {
        waterLevel = TANK_MEDIUM;
      } else if (lowLevelSensorOutput == LOW && refillInitiationLevelSensorOutput == LOW && maximumLevelSensorOutput == HIGH) {
        waterLevel = TANK_HIGH;
      } else if (lowLevelSensorOutput == LOW && refillInitiationLevelSensorOutput == LOW && maximumLevelSensorOutput == LOW) {
        waterLevel = TANK_FULL;
      } else {
        waterLevel = TANK_LEVEL_ERROR;
      }
      // Serial.println("Water Level: " + String(waterLevel));
      //Serial.println("lowLevelSensorOutput: " + String(lowLevelSensorOutput) + "; refillInitiationLevelSensorOutput: " + String(refillInitiationLevelSensorOutput) + " maximumLevelSensorOutput: " + String(maximumLevelSensorOutput));
      previousWaterLevelCheck = millis();
      waterLevelCheckIndex++;
      unlockVariables();
    }
  }
}

void loop() {

}
