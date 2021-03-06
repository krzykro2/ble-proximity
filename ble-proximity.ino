#include <SoftwareSerial.h>
#include <LowPower.h>

SoftwareSerial ble(8, 5); // TX, RX

#define DEBUG 0

#define CLEAN_CYCLE 1000
#define TIMEOUT 1000
#define SHORT_TIMEOUT 50
#define ACTIVE_WAIT_INTERVAL 10
#define CLIENT_SLEEP_TIME SLEEP_8S
#define SERVER_SLEEP_TIME SLEEP_8S
#define BLE_DELAY 1000

//  0  --> 35
// .25 --> 45
//  1m --> 50
//  2m --> 60
//  5m --> 70
//  20m --> 80
//  60m --> 90
// Przekraczanie maksow nie jest zle, bo jest cap.
#define MIN_RSSI 40 
#define MAX_RSSI 95 
#define SMOOTHING 10

const int UNCERTAIN = 0;
const int CONNECTED_AS_SERVER = 1;
const int CONNECTED_AS_CLIENT = 2;
int state = UNCERTAIN;

char buffer[50];

#define N 2
String MACS[] = {
  "D436399B4914",
  "D436399B3A02",
};
String addr;

#define FAIL "FAIL"

int blueLED = 6;
int lampStrength = 0;

void soft_reset() {
  asm volatile ("  jmp 0");
}

void debug(String s) {
  Serial.println(s);
}
void debug(String name, long val) {
  String result = name + ": " + val;
  debug(result);
}
int toInt(String s) {
  int result = 0;
  int currentTen = 1;
  for (int i = s.length() - 1; i >= 0; i--) {
    result += currentTen * (s[i] - '0');
    currentTen *= 10;
  }
  //debug("toInt", result);
  return result;
}

// WHO ARE WE
int zz_which_am_i = -1;
int which_am_i() {
  if (zz_which_am_i == -1) {
    for (int i = 0; i < N; i++) {
      if (addr == MACS[i]) {
        zz_which_am_i = i;
        break;
      }
    }
  }
  return zz_which_am_i;
}
const int my_pair() {
  return which_am_i() + (is_server() ? 1 : -1);
}
boolean is_server() {
  if (state == CONNECTED_AS_SERVER) return true;
  if (which_am_i() == -1) return false;
  return which_am_i() % 2 == 0;
}
boolean is_client() {
  if (state == CONNECTED_AS_CLIENT) return true;
  if (which_am_i() == -1) return false;
  return which_am_i() % 2 == 1;
}
// END OF WHO ARE WE

// SETUP
void setup() {
  pinMode(13, OUTPUT);
  digitalWrite(13, 0);
  pinMode(blueLED, OUTPUT);
  ble.begin(9600);
  Serial.begin(9600);
  refreshFade();
  delay(BLE_DELAY);
  readRSSI();
  check_identity();
  if (which_am_i() == -1) {
    // If we don't know who we are, we can't do much.
    return;
  }
  sendSet("AT+POWE", "2");
  sendSet("AT+IMME", is_server() ? "1" : "0");
  // What happens in connected state. Remote control.
  sendSet("AT+MODE", is_server() ? "0" : "2");
  // LED
  sendSet("AT+PIO1", DEBUG ? "0" : "1");
}
// Can only be called after readRSSI.
bool check_identity() {
  if (state == CONNECTED_AS_SERVER) return false;
  toBle("AT+ADDR?");
  addr = consumeAnswer("OK+ADDR:", 12, TIMEOUT);
  if (addr == FAIL) {
    return false;
  }
  setName();
  sendSet("AT+ROLE", is_server() ? "1" : "0");
  return which_am_i() != -1;
}
void sendSet(String prefix, String value) {
  String result = prefix + value;
  toBle(result);
  consumeAnswer("OK+Set:", value.length(), SHORT_TIMEOUT);
}
void setName() {
  String name = "Z";
  name = name + which_am_i();
  sendSet("AT+NAME", name);
}
// END OF SETUP
// SLEEP
bool sleeping = false;
void wake_up() {
  if (sleeping) {
    // TODO(kkrolak): Ignoring an error here.
    ble_wake();
    sleeping = false;
  }
  readRSSI();
  if (state != CONNECTED_AS_SERVER) {
    // This might change due to sleeping
    sendSet("AT+ROLE", is_server() ? "1" : "0");
  }
}
// You can only go to sleep if you know who you are. Otherwise just reset.
void sleep(period_t duration) {
  if (!is_client() && !is_server()) {
    soft_reset();
  }
  // Scary shit.
  sendSet("AT+ROLE", "0");
  if (!ble_sleep()) {
    return;
  }
  // Don't ask me.
  delay(BLE_DELAY);
  sleeping = true;
  ard_sleep(duration);
}
bool ble_sleep() {
  toBle("AT+SLEEP");
  return consumeAnswer("OK+SLEEP", 0, SHORT_TIMEOUT) != FAIL;
}
bool ble_wake() {
  toBle(
    "Litwo ojczyzno moja, Ty jestes jak zdrowie. Ile cie trzeba cenic ten tylko sie dowie, kto cie straci.");
  // What you know is true.
  delay(BLE_DELAY);
  return consumeAnswer("OK+WAKE", 0, SHORT_TIMEOUT) != FAIL;
}
// END OF SLEEP
// CLIENT
// Can be called by potential server, when we don't know who we are.
void client() {
  String result = consumeAnswer("BS", 3, TIMEOUT);
  if (result != FAIL) {
    state = CONNECTED_AS_CLIENT;
    setSignal(toInt(result));
  } else {
    if (getSignal() == 0) {
      sleep(CLIENT_SLEEP_TIME);
    }
  }
}
// END OF CLIENT

// SERVER
void connect() {
  String command = "AT+CON" + MACS[my_pair()];
  toBle(command);
  String result = consumeAnswer("OK+CONN", 1, TIMEOUT);
  if (result == "A") {
    state = CONNECTED_AS_SERVER;
  } else {
    state = UNCERTAIN;
  }
}
// Can be done under client, will set state to UNCERTAIN.
void readRSSI() {
  toBle("AT+RSSI?");
  String result = consumeAnswer("OK+Get:-", 2, TIMEOUT);
  if (result == FAIL) {
    state = UNCERTAIN;
  } else {
    state = CONNECTED_AS_SERVER;
    int rssi = toInt(result);
    if (rssi < MIN_RSSI) {
      rssi = MIN_RSSI;
    }
    if (rssi > MAX_RSSI) {
      rssi = MAX_RSSI;
    }
    // Receiving any rssi is a signal of at least perceivable 5%.
    int signal = map(-rssi, -MAX_RSSI, -MIN_RSSI, 0, 240) + 15;
    //Serial.println(rssi);
    setSignal(signal);
    sprintf(buffer, "BS%03d", signal);
    toBle(String(buffer));
  }
}
// Can only be called if we are certain that we are the server.
void connectOrSleep() {
  if (state == UNCERTAIN) {
    connect();
  }
  if (state == UNCERTAIN && getSignal() == 0) {
    // If start is still uncertain, better sleep a bit to wait.
    sleep(SERVER_SLEEP_TIME);
  }
}
// END OF SERVER

// COMM
void toBle(String s) {
  debug("Sending to BLE: " + s);
  //ble.print(s);
  for (int i = 0; i < s.length(); i++) {
    ble.print(s[i]);
  }
}

String consumeAnswer(String prefix, int additional, int timeout) {
  if (prefix == "") {
    debug("Cleanup cycle with consumeAnswer");
  } else {
    debug("consumeAnswer with prefix " + prefix);
  }
  String result = "";
  int remaining = timeout;
  for (int i = 0; i < prefix.length() + additional; i++) {
    startTimer();
    char r = activeRead(remaining);
    if (r <= 0) {
      debug(String("activeRead returned error. Returning FAIL. Read so far: ") + result);
      return FAIL;
    }
    //Serial.println(r);
    if (i >= prefix.length()) {
      result = result + r;
    }
    if (i < prefix.length() && r != prefix[i]) {
      debug(String("consumeAnswer encountered invalid prefix, at ") + i + ", dropping:");
      Serial.println(r);
      return FAIL;
    }
    timeout -= stopTimer();
  }
  if (remaining <= 0) {
    debug("consumeAnswer failed timeout. Returning FAIL");
    debug("remaining", remaining);
    return FAIL;
  }
  debug("consumeAnswer returning " + result);
  return result;
}

char activeRead(int timeout) {
  int remaining = timeout;
  while (!ble.available()) {
    if (remaining <= 0) {
      return -1;
    }
    delay(ACTIVE_WAIT_INTERVAL);
    remaining -= ACTIVE_WAIT_INTERVAL;
  }
  return ble.read();
}
// END OF COMM

long long startAt = 0;
void startTimer() {
  startAt = millis();
}
long long stopTimer() {
  return millis() - startAt;
}

// VISUAL
int history[SMOOTHING];
int currHistory = 0;
long long whenFade = 0;
const int FADE_TIME = 2000;
void refreshFade() {
  whenFade = millis() + FADE_TIME;
}
void maybeFade() {
  if (whenFade < millis()) {
    setSignal(0);
  }
}
int setSignal(int s) {
  //debug("new signal", s);
  refreshFade();
  history[currHistory] = s;
  currHistory = (currHistory + 1) % SMOOTHING;
}
int getSignal() {
  int smoothedSignal = 0;
  for (int i = 0; i < SMOOTHING; i++) {
    smoothedSignal += history[i];
  }
  smoothedSignal = smoothedSignal / SMOOTHING;
  //debug("smoothedSignal", smoothedSignal);
  return smoothedSignal;
}
int lamp() {
  //int result = smoothedSignal * (whenReset - millis()) / RESET_TIME;
  int smoothedSignal = getSignal();
  int result = 1.0L * smoothedSignal * smoothedSignal / 255;
  //debug("perception", result);
  return result;
}
// END OF VISUAL

void loop() {
  state = UNCERTAIN;
  if (sleeping) {
    wake_up();
  }
  maybeFade();
  if (DEBUG) {
    passthrough();  
  } else {
    work();
  }
  analogWrite(blueLED, lamp());
}

void work() {
  if (!is_client()) {
    // Only try RSSI, when we are certain we are not the client to save cycles.
    readRSSI();
  }
  if (is_server()) {
    connectOrSleep();
  } else {
    client();
  }
  // Spend the rest of the time cleaning up shit.
  consumeAnswer("", 30, CLEAN_CYCLE);
}

void passthrough() {
  char c;
  String s = "AT+SLEEP";
  if (Serial.available()) {
    c = Serial.read();
    if (c != 'X') {
      ble.print(c);
    }
  }
  if (c == 'X') {
    //toBle("AT+SLEEP");
    sleep(SLEEP_4S);
  } else {
    if (ble.available()) {
      c = ble.read();
      Serial.print(c);
    }
  }
}

void ard_sleep(period_t sleep_time) {
  LowPower.powerDown(sleep_time, ADC_OFF, BOD_OFF);
}

