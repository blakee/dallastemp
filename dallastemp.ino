#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <TimeLib.h>

// don't change these constants, override them in conf.h

#define ONE_WIRE_BUS D4  // DS18B20 pin
#define WIFI_AP_NAME "Insert AP Name"
#define WIFI_AP_PASSWORD "Insert AP Password"
#define API_BASE_URL "Insert Base URL"
#define API_DEVICE_NAME "Insert Device Name"
#define API_SEND_EVERY_SECONDS 15
#define DEBUG_OUTPUT true
#define PROBE1_ADDRESS { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#define PROBE2_ADDRESS { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#define TIME_IS_STALE_AFTER_SECONDS 3600

#include "conf.h"

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
#define NTP_LOCAL_PORT 4433
WiFiUDP udp;

unsigned long lastEpoch = 0, lastEpochMillis = 0;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);


// send an NTP request to the time server at the given address
unsigned long sendNTPpacket(IPAddress& address)
{
  Serial.println("sending NTP packet...");
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

unsigned long fetchEpoch() {
  //get a random server from the pool
  IPAddress timeServerIP;
  WiFi.hostByName("time.nist.gov", timeServerIP); 
  sendNTPpacket(timeServerIP); // send an NTP packet to a time server
  // wait to see if a reply is available
  delay(1000);
  
  int cb = udp.parsePacket();
  if (!cb) {
    Serial.println("no NTP packet yet");
    return 0;
  }
  else {
    Serial.print("NTP packet received, length=");
    Serial.println(cb);
    // We've received a packet, read the data from it
    udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

    //the timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, esxtract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
    // combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    Serial.print("Seconds since Jan 1 1900 = " );
    Serial.println(secsSince1900);

    // now convert NTP time into everyday time:
    // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
    const unsigned long seventyYears = 2208988800UL;
    // subtract seventy years:
    unsigned long epoch = secsSince1900 - seventyYears;
    return epoch;
  }
}

String getISO8601() {
  if (lastEpoch == 0 || ((millis() - lastEpochMillis) > ((TIME_IS_STALE_AFTER_SECONDS)*1000) )) {
    lastEpoch = fetchEpoch();
    lastEpochMillis = millis();
  }
  
  if (lastEpoch>0) {
    unsigned long epoch = lastEpoch + ((millis() - lastEpochMillis)/1000);
    int y = year(epoch);
    int m = month(epoch);
    int d = day(epoch);
    String iso8601 = "" + String(year(epoch)) + "-" + zeroPad(month(epoch)) + "-" + zeroPad(day(epoch)) + "T" + zeroPad(hour(epoch)) + ":" + zeroPad(minute(epoch)) + ":" + zeroPad(second(epoch)) + ".000";
    return iso8601;
  } else {
    return "";
  }
}

String zeroPad(int in) {
  String res = "";
  if (in<10) res += "0";
  res += in;
  return res;
}
  
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(DEBUG_OUTPUT);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_AP_NAME, WIFI_AP_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected");

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  Serial.println("Starting UDP");
  udp.begin(NTP_LOCAL_PORT);
  Serial.print("Local port: ");
  Serial.println(udp.localPort());

  byte addr[8];
  Serial.println("Looking for 1-Wire devices...");
  while(oneWire.search(addr)) {
    Serial.print("\nFound 1-Wire device with address: ");
    for(int i = 0; i < 8; i++) {
      Serial.print("0x");
      if (addr[i] < 16) {
        Serial.print('0');
      }
      Serial.print(addr[i], HEX);
      if (i < 7) {
        Serial.print(", ");
      }
    }
  }
  Serial.println("\n1-Wire scan complete");
  oneWire.reset_search();
  DS18B20.begin();
}

unsigned long lastSentMillis = 0;

String fullUrl = String(API_BASE_URL) + "/cloud/api/site/" + String(API_DEVICE_NAME) + "/EMS";
DeviceAddress Probe01 = PROBE1_ADDRESS;
DeviceAddress Probe02 = PROBE2_ADDRESS;

void loop() {
  if ( (millis() - lastSentMillis) < ( (API_SEND_EVERY_SECONDS) * 1000 ) ) {
    return;
  }
  
  String curDate = getISO8601();
  if (curDate == "") {
    delay(500);
    return;
  }
  
  DS18B20.requestTemperatures();

  float temp = DS18B20.getTempF(Probe01);
  float temp2 = DS18B20.getTempF(Probe02);
    
  Serial.print("Temperature: ");
  Serial.println(temp);
  Serial.println(temp2);

  String xml = "<?xml version=\"1.0\"?>\n<LCS timestamp=\"" + curDate + "\" vendorVersion=\"2016.4.8.999\">" + 
    "<data n=\"\"><p n=\"" + String(API_DEVICE_NAME) + "\"><p n=\"IntegrationPoints\"><p n=\"Ahu01\">" +
    "<p n=\"Rat\"><p n=\"in10\"><p n=\"value\" v=\"" + temp + "\"/></p></p>" + 
    "<p n=\"Sat\"><p n=\"in10\"><p n=\"value\" v=\"" + temp2 + "\"/></p></p>" + 
    "</p></p></p></data></LCS>";

  Serial.println(xml);
  
  HTTPClient http;
  http.begin(fullUrl);
  http.addHeader("Content-Type", "text/xml");
  http.sendRequest("PUT", xml);
  http.end();

  lastSentMillis = millis();
}

