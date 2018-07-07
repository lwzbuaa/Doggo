// Extreme Mobility Doggo Code

// Notes from cooperative scheduling ChibiOS example
// This code uses cooperative scheduling. Cooperative scheduling
// simplifies multitasking since no preemptive context switches occur.
//
// You must call chYield() or other ChibiOS functions such as chThdSleep
// to force a context switch to other threads.
//
// Insert a delay(100) in loop() to see the effect of not
// using chThdSleep with cooperative scheduling.
//
// Setting CH_TIME_QUANTUM to zero disables the preemption for threads
// with equal priority and the round robin becomes cooperative.
// Note that higher priority threads can still preempt, the kernel
// is always preemptive.

#include "ChRt.h"
#include "Arduino.h"
#include "ODriveArduino.h"

// undefine debug to stop printing debug messages
#define DEBUG

//------------------------------------------------------------------------------
// Initialize objects related to ODrives

// TODO: There's a lot of repetition in this section that's hinting we should
// somehow encapsulate more behavior. We could put the serial references inside
// the ODriveArduino class and put the pos estimate struct in there too

// Make references to Teensy <-> computer serial (aka USB) and the ODrive(s)
HardwareSerial& odrv0Serial = Serial1;
HardwareSerial& odrv1Serial = Serial2;
HardwareSerial& odrv2Serial = Serial3;
HardwareSerial& odrv3Serial = Serial4;

// Make structs to hold motor readings
// TODO: figure out if I want to mimic the ODive struct style or not
struct ODrive {
    struct Axis {
        float pos_estimate = 0; // in counts
        float ENCODER_OFFSET = 0; // in counts, TODO: need to configure this

        // NOTE: abs_pos is the SUM of estiamte and offset
        float abs_pos_estimate = pos_estimate + ENCODER_OFFSET;
    };
    Axis axis0,axis1;
} odrv0, odrv1, odrv2, odrv3;

// ODriveArduino objects
// These objects are responsible for sending commands to the ODrive
ODriveArduino odrv0Interface(odrv0Serial);
ODriveArduino odrv1Interface(odrv1Serial);
ODriveArduino odrv2Interface(odrv2Serial);
ODriveArduino odrv3Interface(odrv3Serial);

//------------------------------------------------------------------------------
// Global variables. These are needed for cross-thread communication!!

struct LegGain {
    float Kp_theta = 0;
    float Kd_theta = 0;

    float Kp_gamma = 0;
    float Kd_gamma = 0;
} leg0;

volatile uint32_t count = 0;
volatile uint32_t maxDelay = 0;

//------------------------------------------------------------------------------
// Helper utilities
// Add support for using "<<" to stream stuff to the usb serial
template<class T> inline Print& operator <<(Print &obj,     T arg) { obj.print(arg);    return obj; }
template<>        inline Print& operator <<(Print &obj, float arg) { obj.print(arg, 4); return obj; }

//------------------------------------------------------------------------------
// E-STOP function
void ESTOP() {
    while(true) {}
}

//------------------------------------------------------------------------------
// IdleThread: increment a counter and records max delay.
// The max delay is the maximum time chThdYield takes up. chThdYield tells the
// OS to run other threads.
// 64 byte stack beyond task switch and interrupt needs.
static THD_WORKING_AREA(waIdleThread, 64);

static THD_FUNCTION(IdleThread, arg) {
    (void)arg;
    while (true) {
        count++;
        uint32_t t = micros();
        // Yield so other threads can run.
        chThdYield();
        t = micros() - t;
        if (t > maxDelay) maxDelay = t;
    }
}

//------------------------------------------------------------------------------
// BlinkThread: Blink the built-in led
// 64 byte stack beyond task switch and interrupt needs.
static THD_WORKING_AREA(waBlinkThread, 64);

static THD_FUNCTION(BlinkThread, arg) {
    (void)arg;
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
        digitalWrite(LED_BUILTIN, HIGH);
        chThdSleepMilliseconds(500);
        digitalWrite(LED_BUILTIN, LOW);
        chThdSleepMilliseconds(500);
    }
}

//------------------------------------------------------------------------------
// PrintDebugThread: Print debug information to computer at fixed rate
// TODO: characterize how much bandwidth it uses
static THD_WORKING_AREA(waPrintDebugThread, 256);
static THD_FUNCTION(PrintDebugThread, arg) {
    (void)arg;
    int count = 0;
    const int FREQ = 10;

    while(true) { // execute at 10hz
        if(count == FREQ) { // print variable name header every 1s
            Serial << "odrv0.axis0.pos_estimate\todrv0.axis1.pos_estimate\n";
            count = 0;
        }
        // Print odrv0 positions
        Serial << odrv0.axis0.pos_estimate << "\t" << odrv0.axis1.pos_estimate << "\n";

        count++;
        chThdSleepMilliseconds(1000/FREQ);
    }
}

//------------------------------------------------------------------------------
// PositionControlThread: Motor position control thread
// Periodically calculates result from PID controller and sends off a new
// dual current command to the ODrive(s)

// TODO: add support for multiple ODrives

static THD_WORKING_AREA(waPositionControlThread, 128);

static THD_FUNCTION(PositionControlThread, arg) {
    (void)arg;

    while(true) {
        // PID loop FOR ONE LEG
        float alpha = (float) odrv0.axis0.abs_pos_estimate;
        float beta = (float) odrv0.axis1.abs_pos_estimate;

        float theta = alpha/2.0 + beta/2.0;
        float gamma = beta/2.0 - alpha/2.0;

        float theta_sp = 0; // TODO take as struct or something
        float gamma_sp = 0; // TODO take as struct or something

        float p_term_theta = leg0.Kp_theta * (theta_sp - theta);
        float d_term_theta = leg0.Kd_theta * (0); // TODO: Add motor velocities to position message from odrive

        float p_term_gamma = leg0.Kp_gamma * (gamma_sp - gamma);
        float d_term_gamma = leg0.Kd_gamma * (0); // TODO: Add motor velocities to position message from odrive

        // TODO: clamp (ie constrain) the outputs to -1.0 to 1.0
        float tau_theta = p_term_theta + d_term_theta;
        float tau_gamma = p_term_gamma + d_term_gamma;

        // TODO: check signs
        float tau_alpha = tau_theta*0.5 - tau_gamma*0.5;
        float tau_beta = tau_theta*0.5 + tau_gamma*0.5;
        // odrv0Interface.SetDualCurrent(tau_alpha, tau_gamma);

#ifdef DEBUG
        // DEBUG only: send two zero current commands
        // NOTE: when odrive is in closed loop position control I doubt
        // current commands will do anything
        Serial << "send tstamp: " << micros() << "\n";
        odrv0Interface.SetDualCurrent(0.0, 0.0);
#endif

        chThdSleepMicroseconds(10000); // execute at 100Hz approximately
    }
}

//------------------------------------------------------------------------------
// SerialThread: receive serial messages from ODrive.
// Pulls bytes from the odrv0 serial buffer (aka Serial1) at a rate of 100khz.
// When a newline char is received, it calls parseEncoderMessage to update the
// struct associated with the serial buffer.

// TODO: add timeout behavior: throw out buffer if certain time has elapsed since
// a new message has started being received

void parsePositionMsg(char* msg, int len);

// 128 byte stack beyond task switch and interrupt needs.
static THD_WORKING_AREA(waSerialThread, 128);

static THD_FUNCTION(SerialThread, arg) {
    (void)arg;

    const int BUFFER_SIZE = 32;
    char msg[BUFFER_SIZE]; // running buffer of received characters
    int msg_idx = 0; // keep track of which index to write to

    odrv0Serial.clear();

    long msg_start = 0;
    long msg_end = 0;
    int loop_iters = 0;

    while (true) {
        loop_iters++;
        while (odrv0Serial.available()) {
#ifdef DEBUG
            if (msg_idx == 0) {
                msg_start = micros();
                loop_iters = 0;
            }
#endif
            // reset buffer TODO deal with consequences of buffer overflow
            if(msg_idx >= BUFFER_SIZE) {
#ifdef DEBUG
                Serial << "Msg buffer exceeded!\n";
#endif
                msg_idx = 0;
            }
            // Read latest byte out of the serial buffer
            char c = odrv0Serial.read();
            // Add the char to our buffer
            msg[msg_idx++] = c;

            // Check if we got stop character, aka newline
            if(c == '\n') {
                parsePositionMsg(msg,msg_idx);
                msg_idx = 0;

#ifdef DEBUG
                msg_end = micros();
                Serial << "rcvd in: " << msg_end - msg_start << " in " << loop_iters << " loops\n";
                // NOTE: As of code 7/7/18, the average receive time was 282us
                // And number of loop executions to get a message was 34
#endif
            }
        }
        // Run the serial checking loop at 100khz by delaying 10us
        // chThdSleepMilliseconds(10);
        // TODO: make this interrupt driven?
        // Yielding here gives other threads a chance to execute
        chThdYield();
    }
}

/**
 * Parse a dual position message and store the result in the odrive struct
 * @param msg char* : message
 * @param len int   : message length
 * 
 * TODO: make it generalizable to other odrives and other odriveInterfaces
 */

void parsePositionMsg(char* msg, int len) {
    // only print the message if DEBUG is defined above
#ifdef DEBUG
    Serial.print("MSG RECEIVED: ");
    for(int i=0; i<len; i++) {
        Serial << (int)msg[i] << " ";
    }
    Serial << "\n";
#endif

    float m0,m1;
    int result = odrv0Interface.ParseDualPosition(msg, len, m0, m1);
    // result: 1 means success, -1 means didn't get proper message
    if (result == 1) {
        // Update raw counts
        odrv0.axis0.pos_estimate = m0;
        odrv0.axis1.pos_estimate = m1;

        // TODO: this calculation of absolute pos is in the wrong function / scope
        // Update absolute positions
        odrv0.axis0.abs_pos_estimate = m0 + odrv0.axis0.ENCODER_OFFSET;
        odrv0.axis1.abs_pos_estimate = m1 + odrv0.axis1.ENCODER_OFFSET;

#ifdef DEBUG
        Serial << "rcv tstamp: " << micros() << "\n";
        // NOTE: As of 7/7/18 code, around 1500us from send to receive
#endif
    } else {
        // TODO put a debug flag somewhere, otherwise printing messages like
        // these will probably screw things up
#ifdef DEBUG
        Serial.println("Parse failed. Wrong message length or bad checksum.");
#endif
    }
}

//------------------------------------------------------------------------------
// Continue setup() after chBegin().
void chSetup() {
    // Checks to make sure you enabled cooperature scheduling
    if (CH_CFG_TIME_QUANTUM) {
        Serial.println("You must set CH_CFG_TIME_QUANTUM zero in");
        Serial.print("src/arm/chconfig_arm.h");
        Serial.println(F(" to enable cooperative scheduling."));
        while (true) {}
    }

    // Create ALL the threads!!
    // This is the most important part of the setup

    // Idle thread
    chThdCreateStatic(waIdleThread, sizeof(waIdleThread),
        NORMALPRIO, IdleThread, NULL);

    // Control threads
    chThdCreateStatic(waPositionControlThread, sizeof(waPositionControlThread),
        NORMALPRIO, PositionControlThread, NULL);

    chThdCreateStatic(waSerialThread, sizeof(waSerialThread),
        NORMALPRIO, SerialThread, NULL);

    // TODO: add sensor polling thread
    // TODO: create gait pattern thread (aka one that coordinates leg by generating leg setpoints)

    // Debug threads
    // chThdCreateStatic(waPrintDebugThread, sizeof(waPrintDebugThread),
    //     NORMALPRIO, PrintDebugThread, NULL);

    chThdCreateStatic(waBlinkThread, sizeof(waBlinkThread),
        NORMALPRIO, BlinkThread, NULL);
}
//------------------------------------------------------------------------------
void setup() {
    #ifndef __arm__
    Serial.println("Must run on Teensy 3.5");
    while(true){}
    #endif

    Serial.begin(115200);
    // Wait for USB Serial.
    while (!Serial) {}

    // Make sure the custom firmware is loaded because the default BAUD is 115200
    odrv0Serial.begin(500000);
    // TODO: figure out if i should wait for serial available... or some indication the odrive is on

    // Start ChibiOS.
    chBegin(chSetup);
    // chBegin() resets stacks and should never return.
    while (true) {} // TODO: what happens if you dont hold here?
}
//------------------------------------------------------------------------------
void loop() {
    while (true) {
        Serial << "Idle thd execs, max micros btn idle: \t";
        Serial << count << "," << maxDelay << "\n";
        count = 0;
        maxDelay = 0;

        // Allow other threads to run for 1 sec.
        chThdSleepMilliseconds(1000);
    }
}