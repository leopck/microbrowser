/*
   1_Inkplate_WiFi_HTTP example for e-radionica.com Inkplate 6
   For this example you will need USB cable, Inkplate 6 and stable WiFi Internet connection
   Select "Inkplate 6(ESP32)" from Tools -> Board menu.
   Don't have "Inkplate 6(ESP32)" option? Follow our tutorial and add it: 
   https://e-radionica.com/en/blog/add-inkplate-6-to-arduino-ide/

   This example will show you how to connect to WiFi network, get data from Internet and display that data on epaper.
   This example is NOT on to how to parse HTML data from Internet - it will just print HTML on the screen. 

   In quotation marks you will need write your WiFi SSID and WiFi password in order to connect to your WiFi network.

   Want to learn more about Inkplate? Visit www.inkplate.io
   Looking to get support? Write on our forums: http://forum.e-radionica.com/en/
   15 July 2020 by e-radionica.com
*/

#include "litehtml.h"
#include "container_esp32_inkplate.h"
#include "Inkplate.h"               //Include Inkplate library to the sketch
#include <WiFi.h>                   //Include ESP32 WiFi library to our sketch
#include <HTTPClient.h>             //Include HTTP library to this sketch

#define ssid ""                     //Name of the WiFi network (SSID) that you want to connect Inkplate to
#define pass ""                     //Password of that WiFi network

Inkplate display(INKPLATE_1BIT);    //Create an object on Inkplate library and also set library into 1 Bit mode (Monochrome)
// Inkplate d1;

void setup() {
  container_esp32_inkplate handle(&display);                //Initialize litehtml Container for Inkplate ESP32. Call ONLY ONCE.
  int n = WiFi.scanNetworks();                        //Start searching WiFi networks and put the nubmer of found WiFi networks in variable n
  if (n == 0) {                                       //If you did not find any network, show the message and stop the program.
    display.print("No WiFi networks found!");
    display.partialUpdate();
    while (true);
  } else {
    if (n > 10) n = 0;                                //If you did find, print name (SSID), encryption and signal strength of first 10 networks
    for (int i = 0; i < n; i++) {
      display.print(WiFi.SSID(i));
      display.print((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 'O' : '*');
      display.print('\n');
      display.print(WiFi.RSSI(i), DEC);
    }
    display.partialUpdate();                          //(Partial) refresh thescreen
  }

  display.clearDisplay();                             //Clear everything in frame buffer
  display.setCursor(0,0);                             //Set print cursor to new position
  display.print("Connecting to ");                    //Print the name of WiFi network
  display.print(ssid);
  WiFi.begin(ssid, pass);                             //Try to connect to WiFi network
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);                                      //While it is connecting to network, display dot every second, just to know that Inkplate is alive.
    display.print('.');
    display.partialUpdate();
  }
  display.print("connected");                         //If it's connected, notify user
  display.partialUpdate();

  HTTPClient http;
  if(http.begin("https://tomotranslations.com/2020/07/24/this-hero-is-invincible-but-too-cautious-volume-3-chapter-144-part-1/")) {   //Now try to connect to some web page (in this example www.example.com. And yes, this is a valid Web page :))
    if(http.GET()>0) {                                //If connection was successful, try to read content of the Web page and display it on screen
      String htmlText;
      htmlText = http.getString();
      display.setTextSize(2);                         //Set smaller text size, so everything can fit on screen
      display.clearDisplay();
      display.setCursor(0, 0);
      //Sample of how to implement container https://github.com/litehtml/litehtml/blob/1d4c9391619c092fecb71227f4910f7a3757767c/containers/linux/container_linux.cpp
      //Sample of how to implement createFromString https://github.com/litehtml/litebrowser-linux/blob/master/src/html_widget.cpp#L125
      display.print(htmlText);
      display.display();
    }
  }
}

void loop() {
  //Nothing
}
