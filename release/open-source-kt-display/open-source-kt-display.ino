#include <EEPROM.h>
#include <HardwareSerial.h>
#include <OneButton.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// thermistor defines
#define RT0 10000
#define B 3950
#define VCC 5
#define R 10000

// EEPROM defines
#define EEPROM_SIZE 512

// initialize buttons
OneButton buttonUp(22, true);
OneButton buttonDown(21, true);
OneButton buttonPower(0, true);

// initialize display
TFT_eSPI tft = TFT_eSPI(240, 320);

// initialize serial port
HardwareSerial SerialPort(2);

// initialize byte arrays for serial communication
const int BUFFER_SIZE = 12;
const int BUFFER_SIZE_UP = 13;

byte buf[BUFFER_SIZE];
byte buf_up[BUFFER_SIZE_UP] = {13, 1, 245, 86, 41, 74, 40, 202, 4, 20, 1, 50, 14};  // sample packet generated by the web calculator

// initialize variables
int batteryLevel = 0;
int speed = 0;
int power = 0;
int engineTemp = 0;
int controllerTemp = 0;
int currentGear = 0;

// initialize variables for previous state to prevent unnecessary updates
int previousBatteryLevel = -1;
int previousEngineTemp = -1;
int previousControllerTemp = -1;
int previousGear = -1;

// initialize variables for "legal mode"
int previousGearWalk = 0;
int maxGear = 5;
int initialMaxSpeedB2 = 0;
int initialMaxSpeedB4 = 0;
int limitState = 0;

// gear color - 0: yellow (normal), 1: green ("legal mode"), 2: red (breaking)
int gearColor = 0;

// debug variables
bool testing = false;
int SerialAvailableBits = 0;
int counter = 0;

// initialize variables for thermistor
float RT, VR, ln, tx, t0, VRT;
const int thermistorSamples = 10;

void setup() {
    // initialize thermistor base temperature
    t0 = 25 + 273.15;

    // setup display
    tft.init();
    tft.setRotation(6);
    initialRender();

    // initialize eeprom
    EEPROM.begin(EEPROM_SIZE);

    // save initial max speed from pregenerated packet
    initialMaxSpeedB2 = buf_up[2] & 248;
    initialMaxSpeedB4 = buf_up[4] & 32;

    // attach button functions
    buttonUp.attachClick(increaseGear);
    buttonUp.attachLongPressStart(toggleLimit);
    buttonDown.attachClick(decreaseGear);
    buttonDown.attachLongPressStart(walkMode);
    buttonDown.attachLongPressStop(stopWalkMode);
    buttonPower.attachLongPressStart(toggleTesting);

    // setup serial ports
    Serial.begin(9600);
    SerialPort.begin(9600, SERIAL_8N1, 16, 17);

    // turn on display backlight
    digitalWrite(5, HIGH);
    // get data from eeprom
    limitState = EEPROM.readBool(0);
    currentGear = EEPROM.read(1);

    // set initial "legal mode" max gear and speed
    handleLimit();
}

void loop() {
    long time = millis();  // get current time to make loop run at a constant rate

    // // read packet from controller
    SerialAvailableBits = SerialPort.available();
    if (SerialAvailableBits >= BUFFER_SIZE) {    // check if there are enough available bytes to read
        SerialPort.readBytes(buf, BUFFER_SIZE);  // read bytes to the buf array
        // update variables (current gear and crc)
    } else {
        if (counter > 50) {
            SerialPort.begin(9600, SERIAL_8N1, 16, 17);
            counter = 0;
        }
        counter++;
    }
    bool validPacket = shiftArray(0);

    processPacket(buf);          // process packet from the controller
    getControllerTemperature();  // get controller temperature
    if (testing) {               // render testing screen displaying the raw packet
        handleTestingDisplay();
    } else {
        if (validPacket) {         // if the packet is valid render the display, otherwise skip the render
            handleDisplay(false);  // render normal display without force update
        }
        if (millis() - time < 50) {
            delay(50 - (millis() - time));  // delay to make the loop run at a constant rate
        }
    }
    updateGear(false, gearColor);

    buf_up[1] = currentGear;
    buf_up[5] = calculateUpCRC();

    SerialPort.write(buf_up, BUFFER_SIZE_UP);  // send packet to the controller

    // update buttons
    buttonUp.tick();
    buttonDown.tick();
    buttonPower.tick();

    Serial.print("Time: ");
    Serial.println(millis() - time);
}

// group of functions for dealing with buttons
void increaseGear() {
    if (currentGear < maxGear) {
        currentGear++;
        EEPROM.write(1, currentGear);
        EEPROM.commit();
    }
}
void decreaseGear() {
    if (currentGear > 0) {
        currentGear--;
        EEPROM.write(1, currentGear);
        EEPROM.commit();
    }
}
void walkMode() {
    previousGearWalk = currentGear;
    currentGear = 6;
}
void stopWalkMode() {
    currentGear = previousGearWalk;
}
void toggleLimit() {
    limitState = !limitState;
    EEPROM.writeBool(0, limitState);
    EEPROM.commit();
    handleLimit();
}

// group of functions for dealing with the data
// calculating the crc value for the packet to be sent to the controller
int calculateUpCRC() {
    int crc = 0;
    for (int i = 0; i < BUFFER_SIZE_UP; i++) {
        if (i != 5) {
            crc ^= buf_up[i];
        }
    }
    crc ^= 3;
    return crc;
}
// calculating the crc value for the packet received from the controller
int calculateDownCRC() {
    int crc = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (i != 6 && i != 0) {
            crc ^= buf[i];
        }
    }
    return crc;
}
// getting readables values from the packet
void processPacket(byte buf[]) {
    if (buf[3] + buf[4] <= 0) {
        speed = 0;
    } else {
        speed = round(60000 / (buf[3] * 256 + buf[4]) * 0.1885 * 0.66 * 10);
    }

    if (buf[1] > 16) {
        batteryLevel = 16;
    } else {
        batteryLevel = buf[1];
    }

    power = buf[8] * 13;
    engineTemp = int8_t(buf[9]) + 15;
}
// getting controller temperature
void getControllerTemperature() {
    float sum = 0;
    for (int i = 0; i < thermistorSamples; i++) {
        VRT = (VCC / 1023.00) * analogRead(27);
        VR = VCC - VRT;
        RT = VRT / (VR / R);  // Resistance of RT

        ln = log(RT / RT0);
        tx = (1 / ((ln / B) + (1 / t0)));  // Temperature from thermistor

        tx = tx - 273.15;  // Conversion to Celsius
        sum += TX;
    }
    controllerTemp = sum / thermistorSamples;
}
// function for shifting the packet in case of a bit loss
bool shiftArray(int counter) {
    int crc = calculateDownCRC();
    if (counter == 5) {
        return false;
    }
    if (buf[0] != 65 || buf[6] != crc) {
        byte newBuf[BUFFER_SIZE];
        int shift = 0;
        for (int i = 1; i < BUFFER_SIZE; i++) {
            if (buf[i] == 65) {
                shift = i;
            }
        }
        shift = BUFFER_SIZE - shift;
        for (int i = 0; i < BUFFER_SIZE; i++) {
            if (i < shift) {
                newBuf[i] = buf[BUFFER_SIZE + i - shift];
            } else {
                newBuf[i] = buf[i - shift];
            }
        }
        memcpy(buf, newBuf, sizeof(newBuf));
        crc = calculateDownCRC();
        if (buf[0] != 65 || buf[6] != crc) {
            int currentCounter = counter += 1;
            shiftArray(currentCounter);
        } else {
            return true;
        }
    }
}
// function to handle "legal mode"
void handleLimit() {
    buf_up[2] = buf_up[2] & 7;
    buf_up[4] = buf_up[4] & 223;
    if (limitState) {
        buf_up[2] = buf_up[2] | ((15 & 31) * 8);
        buf_up[4] = buf_up[4] | (15 & 32);
        gearColor = 1;
        updateGear(true, gearColor);
        if (currentGear > 2) {
            currentGear = 2;
        }
        maxGear = 2;
    } else {
        buf_up[2] = buf_up[2] | initialMaxSpeedB2;
        buf_up[4] = buf_up[4] | initialMaxSpeedB4;
        gearColor = 0;
        updateGear(true, gearColor);
        maxGear = 5;
    }
}

// group of functions for dealing with display
// main function for rendering the display
void handleDisplay(bool force) {
    tft.setTextFont(0);
    updateBattery(batteryLevel / 4, force);
    updateEngineTemp(force);
    updateControllerTemp();
    updatePower();
    updateSpeed();
}
// drawing the empty screen with lines and a battery outline
void initialRender() {
    tft.fillScreen(TFT_BLACK);
    // Draw battery
    tft.drawRoundRect(10, 16, 80, 24, 4, TFT_GREEN);
    tft.fillRect(90, 23, 3, 10, TFT_GREEN);
    tft.fillRoundRect(92, 23, 3, 10, 2, TFT_GREEN);

    tft.drawFastHLine(0, 54, 240, TFT_WHITE);
    tft.drawFastHLine(0, 248, 240, TFT_WHITE);
}
// drawing the battery level bars
void updateBattery(int bars, bool force) {
    if (previousBatteryLevel != batteryLevel || force) {
        tft.fillRect(10, 16, 100, 25, TFT_BLACK);
        tft.drawRoundRect(10, 16, 80, 24, 4, TFT_GREEN);
        tft.fillRect(90, 23, 3, 10, TFT_GREEN);
        tft.fillRoundRect(92, 23, 3, 10, 2, TFT_GREEN);
        for (int i = 0; i < bars; i++) {
            int x = i * 19 + 13;
            tft.fillRoundRect(x, 19, 17, 18, 2, TFT_GREEN);
        }
        previousBatteryLevel = batteryLevel;
    }
}
// drawing the current speed
void updateSpeed() {
    if (speed < 1000) {  // make sure the speed is always 3 digits or less
        tft.setTextColor(TFT_WHITE, 0);
        tft.setCursor(40, 86);
        tft.setTextFont(8);
        tft.setTextSize(1);
        int a = 0;
        int b = 0;
        if (speed > 10) {
            a = speed / 10;
            b = speed - (a * 10);
        }
        if (a < 10) {
            tft.print(0);
            tft.print(a);
        } else {
            tft.print(a);
        }
        tft.setTextFont(6);
        tft.setTextSize(1);
        tft.setCursor(145, 124);
        tft.print(".");
        tft.print(b);
    }
}
// drawing the current gear
void updateGear(bool force, int color) {
    if (previousGear != currentGear || force) {
        tft.setTextFont(7);
        tft.setTextSize(1);
        if (color == 1) {
            tft.setTextColor(TFT_GREEN, 0);
        } else {
            tft.setTextColor(TFT_YELLOW, 0);
        }
        tft.setCursor(108, 180);
        tft.print(currentGear);
        previousGear = currentGear;
    }
}
// drawing the current engine temperature
void updateEngineTemp(bool force) {
    if (previousEngineTemp != engineTemp || force) {
        tft.setTextSize(2);
        tft.setCursor(16, 265);
        tft.setTextColor(TFT_WHITE, 0);
        tft.print("MTP:");
        tft.setTextColor(TFT_YELLOW, 0);
        if (engineTemp < 10) {
            tft.print("  ");
            tft.print(engineTemp);
        } else if (engineTemp < 100) {
            tft.print(" ");
            tft.print(engineTemp);
        } else {
            tft.print(engineTemp);
        }
        tft.print("C");
        previousEngineTemp = engineTemp;
    }
}
// drawing the current controller temperature
void updateControllerTemp() {
    tft.setTextSize(2);
    tft.setCursor(140, 265);
    tft.setTextColor(TFT_WHITE, 0);
    tft.print("CTP:");
    tft.setTextColor(TFT_YELLOW, 0);
    tft.print(controllerTemp);
    tft.print("C");
    previousControllerTemp = controllerTemp;
}
// drawing the current power draw <to do: make the power more readable (update every 10 times for example)>
void updatePower() {
    tft.setTextSize(2);
    tft.setCursor(16, 290);
    tft.setTextColor(TFT_WHITE, 0);
    tft.print("MP:");
    tft.setTextColor(TFT_YELLOW, 0);
    if (power < 10) {
        tft.print("   ");
    } else if (power < 100) {
        tft.print("  ");
    } else if (power < 1000) {
        tft.print(" ");
    }
    tft.print(power);
    tft.println("W");
}

// group of functions to handle the test mode
void toggleTesting() {
    testing = !testing;
    clearDisplay();
    Serial.println("Supcio");
}
void handleTestingDisplay() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(0);
    tft.setTextSize(2);
    tft.setCursor(0, 60);
    tft.setTextColor(TFT_WHITE, 0);
    tft.print("Voltage: ");
    long voltage = analogRead(32);
    if (voltage < 10) {
        tft.print("   ");
    } else if (voltage < 100) {
        tft.print("  ");
    } else if (voltage < 1000) {
        tft.print(" ");
    }
    tft.println(voltage);
    tft.print("Bits: ");
    tft.println(SerialAvailableBits);
    for (int i = 0; i < 12; i++) {
        tft.print("B");
        tft.print(i);
        tft.print(": ");
        tft.println(buf[i]);
    }
    delay(100);
}
void clearDisplay() {
    initialRender();
    handleDisplay(true);
}