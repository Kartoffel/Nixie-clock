/** Arduino source code
 * Nixie clock
 * Author: Niek Blankers <niek@niekproductions.com>
 *
 * 6 Digit IN-14 Nixie clock
 * Uses Arduino Pro Micro with HV5812, DS3234.
 */
#include <SPI.h>
#include <avr/eeprom.h>

// #define DEBUG

// SPI pins
const int   csPin           =   8;
const int   strobePin       =   A0;
const int   dataPin         =   16; // mega32u4 MOSI - PB2
const int   clkPin          =   15; // mega32u4 SCK - PB1

// Anode driver, used for dimming
const int   anodes          =   5;
const int   minBrightness   =   50;
const int   maxBrightness   =   100;
int         lastBrightness  =   minBrightness;

// Light sensor settings
const int   ldrPin          =   A1;
const int   ldrDark         =   80;
const int   ldrBright       =   830;

const int   frontButtons    =   A2;
const int   rearButtons     =   A3;

// Analog values for frontButtons
const int   al1ButtonVal    =   0;
const int   upButtonVal     =   132;
const int   downButtonVal   =   320;
const int   al2ButtonVal    =   487;
// Analog values for rearButtons
const int   setButtonVal    =   502;
const int   modeButtonVal   =   0;
// The analog readings can differ this much from the values above
const int   buttonRange     =   10;

const int   al1Button       =   1;
const int   upButton        =   2;
const int   downButton      =   3;
const int   al2Button       =   4;
const int   setButton       =   5;
const int   modeButton      =   6;
// Debounce time for buttons (ms)
const int   buttonDebounce  =   200;

const int   decimalPoint    =   6;

// Alarm indicator bulbs
const int   al1Light        =   4;
const int   al2Light        =   7;

const int   buzzerPin       =   2;

// Number of seconds after which to switch between date and time
const int   switchInterval  =   5;

const int   fadeDuration    =   150;

int         date[3]         =   { 1, 1, 0 };
int         time[3]         =   { 0, 0, 0 };
int         prevTime[3]     =   { 0, 0, 0 };

struct alarmSettings_t{
    bool    al1Enabled      =   false;
    bool    al2Enabled      =   false;

    int     al1Time[2]      =   { 0, 0 };
    int     al2Time[2]      =   { 0, 0 };
} alarmSettings;

// The number of steps between the output being on and off
const int   dimFactor       =   100;
// Factor that's used for true dimming (exponential)
// From https://diarmuid.ie/blog/pwm-exponential-led-fading-on-arduino-or-other-platforms/
float       R;

// Data to send to shift register, MSB first
const short numbers[10] = {
    0b1111111110,
    0b0111111111,
    0b1011111111,
    0b1101111111,
    0b1110111111,
    0b1111011111,
    0b1111101111,
    0b1111110111,
    0b1111111011,
    0b1111111101
};

const short blank                   = 0b1111111111;

// Operating modes
const int   MODE_DISPLAY_TIME       =   0;
const int   MODE_DISPLAY_DATETIME   =   1;
const int   MODE_SET                =   2;
// Volatile - modified in interrupt function as well as loop
volatile int mode                   =   MODE_DISPLAY_TIME;

const int   day             =   0;
const int   month           =   1;
const int   year            =   2;

const int   hours           =   0;
const int   minutes         =   1;
const int   seconds         =   2;

// Enter cathode cleaning mode
volatile bool cathodeClean  =   false;

const int   fadeSteps      =   50;

void setup(){
    setupNixies();
    clearDisplay();

    RTC_init();

    // DS3234 SQW output
    attachInterrupt(0, updateTime, FALLING);

    pinMode(ldrPin, INPUT);

    pinMode(buzzerPin, OUTPUT);
    digitalWrite(buzzerPin, LOW);

    loadAlarmSettings();

    // Calculate the R factor for exponential dimming
    R = dimFactor * log10(2) / log10(255);

#ifdef DEBUG
    Serial.begin(9600);
    // Necessary for Leonardo
    while(!Serial);
#endif
}

void updateTime(){
    start_SPI();
    readDateTime();
    stop_SPI();

    // Prevent cathode poisoning by randomly cycling through digits for 3 seconds
    cathodeClean = (time[minutes] % 20) == 0 && time[seconds] < 3 && mode != MODE_SET;

    if(!cathodeClean)
        updateDisplay();

    memcpy(prevTime, time, sizeof(time));
}

void updateDisplay(){
    switch(mode){
        case MODE_DISPLAY_TIME:
            // Display time
            fadeTo(time, prevTime, fadeDuration);
            digitalWrite(decimalPoint, LOW);
            break;
        case MODE_DISPLAY_DATETIME:
            // Display time and date
            // TODO: use state variable
            if((time[seconds] / switchInterval) % 2 == 0){
                if((float) time[seconds] / switchInterval == 0.0)
                    displayTime(time);
                else
                    fadeTo(time, prevTime, fadeDuration);
                digitalWrite(decimalPoint, LOW);
            }else{
                displayTime(date);
                digitalWrite(decimalPoint, !(time[seconds] % 2));
            }
            break;
    }
}

void loop(){
    updateBrightness();

    handleButtons();

    handleAlarms();

    while(cathodeClean){
        cycleDigits();
        delay( 100 );
    }

    delay(15);
}

void saveAlarmSettings(){
    eeprom_write_block((const void*)&alarmSettings, (void*)0, sizeof(alarmSettings));
}

void loadAlarmSettings(){
    eeprom_read_block((void*)&alarmSettings, (void*)0, sizeof(alarmSettings));

    restrictAlarm(alarmSettings.al1Time);
    restrictAlarm(alarmSettings.al2Time);

    digitalWrite(al1Light, alarmSettings.al1Enabled);
    digitalWrite(al2Light, alarmSettings.al2Enabled);
}

void handleAlarms(){
    if( alarmSettings.al1Enabled && time[hours] == alarmSettings.al1Time[hours] && time[minutes] == alarmSettings.al1Time[minutes] && time[seconds] == 0 ){
        while(alarmSettings.al1Enabled){
            digitalWrite(buzzerPin, millis() % 1000 < 500);
            handleButtons();
        }
        delay(1000);
    }

    if( alarmSettings.al2Enabled && time[hours] == alarmSettings.al2Time[hours] && time[minutes] == alarmSettings.al2Time[minutes] && time[seconds] == 0 ){
        while(alarmSettings.al2Enabled){
            digitalWrite(buzzerPin, millis() % 1000 < 500);
            handleButtons();
        }
        delay(1000);
    }

    digitalWrite(buzzerPin, LOW);
}

void handleButtons(){
    switch( buttonPressed() ){
        case setButton:
            if(buttonIsHeld(setButton))
                changeDateTime();
            break;
        case modeButton:
            if(mode == MODE_DISPLAY_DATETIME)
                mode = MODE_DISPLAY_TIME;
            else if(mode == MODE_DISPLAY_TIME)
                mode = MODE_DISPLAY_DATETIME;
            delay(buttonDebounce);
            break;
        case al1Button:
            if( buttonIsHeld( al1Button ) ){
                setAlarm(1);
            }else{
                if ( alarmSettings.al1Enabled )
                    disableAlarm(1);
                else
                    enableAlarm(1);
            }
            break;
        case al2Button:
            if( buttonIsHeld( al2Button ) ){
                setAlarm(2);
            }else{
                if ( alarmSettings.al2Enabled )
                    disableAlarm(2);
                else
                    enableAlarm(2);
            }
            break;
    }
}

bool buttonIsHeld(int button){
    int buttonHeldFor = 0;
    while(buttonPressed() == button){
        // Increase by 20 since readAvgAnalog takes ~10ms
        buttonHeldFor += 20;
        delay(10);
        if(buttonHeldFor > 800)
            return true;
    }
    return false;
}

void enableAlarm(int alarm){
    int prevMode = mode;
    mode = MODE_SET;

    int timeDisplay[3] = { 0, 0, 100};

    if( alarm == 1 ){
        memcpy(timeDisplay, alarmSettings.al1Time, sizeof(alarmSettings.al1Time));
        alarmSettings.al1Enabled = true;
        digitalWrite(al1Light, HIGH);
    }else{
        memcpy(timeDisplay, alarmSettings.al2Time, sizeof(alarmSettings.al2Time));
        alarmSettings.al2Enabled = true;
        digitalWrite(al2Light, HIGH);
    }

    displayTime(timeDisplay);
    saveAlarmSettings();

    delay(1000);

    displayTime(time);
    mode = prevMode;
}

void disableAlarm(int alarm){
    if( alarm == 1 ){
        alarmSettings.al1Enabled = false;
        digitalWrite(al1Light, LOW);
    }else{
        alarmSettings.al2Enabled = false;
        digitalWrite(al2Light, LOW);
    }
    saveAlarmSettings();
}

void setAlarm(int alarm){
    int prevMode = mode;
    mode = MODE_SET;

    int newAlarm[2];

    if( alarm == 1 )
        memcpy(newAlarm, alarmSettings.al1Time, sizeof(alarmSettings.al1Time));
    else
        memcpy(newAlarm, alarmSettings.al2Time, sizeof(alarmSettings.al2Time));

    int timeDisplay[3] = { 0, 0, 100 };
    memcpy(timeDisplay, newAlarm, sizeof(newAlarm));
    displayTime(timeDisplay);

    while( buttonPressed() == al1Button || buttonPressed() == al2Button){};

    for(int i=0; i<2; i++){
        while(buttonPressed() != al1Button && buttonPressed() != al2Button){
            int activeButton = buttonPressed();
            int toAdd = 0;

            if(activeButton == upButton)
                toAdd = 1;
            else if(activeButton == downButton)
                toAdd = -1;

            newAlarm[i] += toAdd;
            restrictAlarm(newAlarm);
            memcpy(timeDisplay, newAlarm, sizeof(newAlarm));
            if(millis() % 1000 < 500)
                timeDisplay[i] = 100;

            displayTime(timeDisplay);
            delay( abs(toAdd) * buttonDebounce );
        }
        delay(buttonDebounce);
    }

    if( alarm == 1 ){
        memcpy(alarmSettings.al1Time, newAlarm, sizeof(newAlarm));
        alarmSettings.al1Enabled = true;
        digitalWrite(al1Light, HIGH);
    }else{
        memcpy(alarmSettings.al2Time, newAlarm, sizeof(newAlarm));
        alarmSettings.al2Enabled = true;
        digitalWrite(al2Light, HIGH);
    }
    saveAlarmSettings();
    displayTime(time);
    mode = prevMode;
}

void restrictAlarm(int newAlarm[2]){
    if(newAlarm[hours] > 23) newAlarm[hours] = 0;
    else if(newAlarm[hours] < 0) newAlarm[hours] = 23;

    if(newAlarm[minutes] > 59) newAlarm[minutes] = 0;
    else if(newAlarm[minutes] < 0) newAlarm[minutes] = 59;
}

void disableInterrupts(){
    detachInterrupt(0);
}

void enableInterrupts(){
    attachInterrupt(0, updateTime, FALLING);
}

void changeDateTime(){
    disableInterrupts();

    int newDate[3];
    int newTime[3];

    memcpy(newDate, date, sizeof(date));
    memcpy(newTime, time, sizeof(time));

    int prevMode = mode;
    mode = MODE_SET;

    digitalWrite(decimalPoint, HIGH);
    displayTime(newDate);

    while(buttonPressed() == setButton){};

    for(int i=0; i<6; i++){
        while(buttonPressed() != setButton){
            int activeButton = buttonPressed();
            int toAdd = 0;

            if(activeButton == upButton){
                toAdd = 1;
            }else if(activeButton == downButton){
                toAdd = -1;
            }

            int timeDisplay[3];

            if( i < 3 ){
                newDate[i] += toAdd;
                restrictDate(newDate);

                digitalWrite(decimalPoint, HIGH);

                memcpy(timeDisplay, newDate, sizeof(newDate));
                if(millis() % 1000 < 500)
                    // Blank digit
                    timeDisplay[i] = 100;
            }else{
                newTime[i - 3] += toAdd;
                restrictTime(newTime);

                digitalWrite(decimalPoint, LOW);

                memcpy(timeDisplay, newTime, sizeof(newTime));
                if(millis() % 1000 < 500)
                    timeDisplay[i-3] = 100;

            }

            displayTime(timeDisplay);

            delay( abs(toAdd) * buttonDebounce );
        }
        delay(buttonDebounce);
    }

    start_SPI();
    setDateTime(newDate, newTime);
    stop_SPI();

    mode = prevMode;

    enableInterrupts();
}

void restrictTime(int newTime[3]){
    if(newTime[hours] > 23) newTime[hours] = 0;
    else if(newTime[hours] < 0) newTime[hours] = 23;

    if(newTime[minutes] > 59) newTime[minutes] = 0;
    else if(newTime[minutes] < 0) newTime[minutes] = 59;

    if(newTime[seconds] > 59) newTime[seconds] = 0;
    else if(newTime[seconds] < 0) newTime[seconds] = 59;
}

void restrictDate(int newDate[3]){
    if(newDate[day] > 31) newDate[day] = 1;
    else if(newDate[day] < 1) newDate[day] = 31;

    if(newDate[month] > 12) newDate[month] = 1;
    else if(newDate[month] < 1) newDate[month] = 12;

    if(newDate[year] > 99) newDate[year] = 0;
    else if(newDate[year] < 0) newDate[year] = 99;
}

int buttonPressed(){
    int frontReading = readAvgAnalog(frontButtons, 25, 0);

    if( isInRange(frontReading, al1ButtonVal, buttonRange) )
        return al1Button;
    else if( isInRange(frontReading, upButtonVal, buttonRange) )
        return upButton;
    else if( isInRange(frontReading, downButtonVal, buttonRange) )
        return downButton;
    else if( isInRange(frontReading, al2ButtonVal, buttonRange) )
        return al2Button;

    int rearReading = readAvgAnalog(rearButtons, 5, 2);

    if( isInRange(rearReading, modeButtonVal, buttonRange) )
        return modeButton;
    else if( isInRange(rearReading, setButtonVal, buttonRange) )
        return setButton;

    return 0;
}

bool isInRange(int value, int compare, int range){
    return value >= (compare - range) && value <= (compare + range);
}

int readAvgAnalog(int pin, byte numReadings, int readingDelay){
    int readingsTotal = 0;

    for(int i = 0; i < numReadings; i++){
        readingsTotal += analogRead(pin);
        delay(readingDelay);
    }

    return readingsTotal / numReadings;
}

void updateBrightness(){
    int brightness = map(readAvgAnalog(ldrPin,5,2), ldrDark, ldrBright, minBrightness, maxBrightness);
    int brightnessPercent = constrain(brightness, minBrightness, maxBrightness);

    if(brightnessPercent > lastBrightness){
        brightnessPercent = lastBrightness + 1;
    }else if(brightnessPercent < lastBrightness){
        brightnessPercent = lastBrightness - 1;
    }

    lastBrightness = brightnessPercent;

    // Exponential dimming
    brightness = pow (2, (brightnessPercent / R)) - 1;

    // Set the anode driver output to the calculated brightness
    analogWrite(anodes, brightness);
}

void setBrightness(int brightness){
    analogWrite(anodes, brightness);
}

void fadeTo(int* time, int* prevTime, int duration){
    float phaseDuration = 1000 * duration / fadeSteps;

    for(int phase = 1; phase <= fadeSteps; phase++){
        displayTime(prevTime);
        delayMicroseconds((phaseDuration * (fadeSteps-phase)) / fadeSteps + 1);
        displayTime(time);
        delayMicroseconds(phaseDuration * phase / fadeSteps);
    }
}

void displayTime(int* time){
    for(int i = 2; i >= 0; i--){
        if(time[i] > 99){
            shift10(dataPin, clkPin, blank);
            shift10(dataPin, clkPin, blank);
        }else{
            shift10(dataPin, clkPin, numbers[(int) time[i] % 10]);
            shift10(dataPin, clkPin, numbers[(int) time[i] / 10]);
        }
    }

    digitalWrite(strobePin, HIGH);
    digitalWrite(strobePin, LOW);
}

void clearDisplay(){
    int clearTime[3] = { 100, 100, 100 };
    displayTime(clearTime);
}

void cycleDigits(){
        int randomDigits[3] = {
            random(0,99),
            random(0,99),
            random(0,99)
        };
        displayTime(randomDigits);
}

// MSB first
void shift10(uint8_t dataPin, uint8_t clockPin, short val){
    for (int i = 0; i < 10; i++){
        digitalWrite(dataPin, !!(val & (1 << (9 - i))));

        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, LOW);
    }
}

void setupNixies(){
    pinMode(dataPin, OUTPUT);
    pinMode(al1Light, OUTPUT);
    pinMode(al2Light, OUTPUT);

    digitalWrite(al1Light, LOW);
    digitalWrite(al2Light, LOW);

    setBrightness(150);

    pinMode(clkPin, OUTPUT);
    pinMode(strobePin, OUTPUT);
    pinMode(anodes, OUTPUT);
    pinMode(decimalPoint, OUTPUT);

    digitalWrite(strobePin, LOW);
    analogWrite(anodes, 0);
}

void stop_SPI(){
    SPI.end();
}

void start_SPI(){
    SPI.begin();
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE1);
}

void RTC_init(){
    pinMode(csPin,OUTPUT); // chip select
    // start the SPI library:
    SPI.begin();
    SPI.setBitOrder(MSBFIRST);
    SPI.setDataMode(SPI_MODE1); // both mode 1 & 3 should work
    //set control register
    digitalWrite(csPin, LOW);
    SPI.transfer(0x8E);
    SPI.transfer(0x60); //60= disable Oscillator and Battery SQ wave @1hz, temp compensation, Alarms disabled
    digitalWrite(csPin, HIGH);
    delay(10);
}

void setDateTime(int* date, int* time){
    int TimeDate [7]={time[seconds],time[minutes],time[hours],0,date[day],date[month],date[year]};
    for(int i = 0; i < 7; i++){
        if(i == 3)
            i++;
        int b = TimeDate[i] / 10;
        int a = TimeDate[i]-b * 10;
        if(i == 2){
            if (b == 2)
                b = B00000010;
            else if (b == 1)
                b = B00000001;
        }
        TimeDate[i] = a + (b<<4);

        digitalWrite(csPin, LOW);
        SPI.transfer(i + 0x80);
        SPI.transfer(TimeDate[i]);
        digitalWrite(csPin, HIGH);
    }
}

void readDateTime(){
    int TimeDate [7]; //second,minute,hour,null,day,month,year
    for(int i=0; i < 7; i++){
        if(i == 3)
            i = 4;
        digitalWrite(csPin, LOW);
        SPI.transfer(i+0x00);
        unsigned int n = SPI.transfer(0x00);
        digitalWrite(csPin, HIGH);
        int a=n & B00001111;
        if(i==2){
            int b=(n & B00110000)>>4; //24 hour mode
            if(b==B00000010)
                b=20;
            else if(b==B00000001)
                b=10;
            TimeDate[i]=a+b;
        }
        else if(i==4){
            int b=(n & B00110000)>>4;
            TimeDate[i]=a+b*10;
        }
        else if(i==5){
            int b=(n & B00010000)>>4;
            TimeDate[i]=a+b*10;
        }
        else if(i==6){
            int b=(n & B11110000)>>4;
            TimeDate[i]=a+b*10;
        }
        else{
            int b=(n & B01110000)>>4;
            TimeDate[i]=a+b*10;
        }
    }
    date[day] = TimeDate[4];
    date[month] = TimeDate[5];
    date[year] = TimeDate[6];
    time[hours] = TimeDate[2];
    time[minutes] = TimeDate[1];
    time[seconds] = TimeDate[0];
}
