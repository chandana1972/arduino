#include <Servo.h>
#include <TimeAlarms.h>
#include <Time.h>
#include <config_rest.h>
#include <rest_server.h>
#include <SPI.h>
#include <Ethernet.h>
#include <limits.h>

#include "feedservo.h"
#include "ntp.h"

byte ENET_MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
byte ENET_IP[] = { 192, 168, 2, 2 };


#define FEED_INTERVAL_HOURS 12
#define NUM_FEEDS 2
time_t feedTime = AlarmHMS(6, 0, 0); // Feed at 6am, 6pm
time_t feedDelta = AlarmHMS(FEED_INTERVAL_HOURS, 0, 0);
AlarmId feedAlarm[NUM_FEEDS];

EthernetServer server(80);
RestServer restServer = RestServer(Serial);

void set_feed_timer() {
    for (int i = 0; i < NUM_FEEDS; i++)
        Alarm.free(feedAlarm[i]);

    for (int i = 0; i < NUM_FEEDS; i++) {
        feedAlarm[i] = Alarm.alarmRepeat((time_t)(feedTime + i*feedDelta), FeedServo::feed_trigger);
        Serial.print("New feed alarm: " + String(i) + " / ");
        Serial.println((int)feedAlarm[i]);
    }
}

struct ResourceInteraction {
    virtual int read() = 0;
    virtual void write(int v) = 0;
    //resource_description_t desc;
};

struct FeedNowInteraction : ResourceInteraction {
    //FeedNowInteraction() { desc = (resource_description_t){"feed_now", true, {0, 1}}; }
    virtual int read() { return 0; }
    virtual void write(int v) { if (v) { FeedServo::feed_now(); } }
};

struct ServoNeutralInteraction : ResourceInteraction {
    //ServoNeutralInteraction() { desc = (resource_description_t){"servo_neutral", true, {0, 180}}; }
    virtual int read() { return FeedServo::servoNeutral; }
    virtual void write(int v) {
        FeedServo::servoNeutral = v;
        FeedServo::neutral();
    }
};

struct FeedAMMinInteraction : ResourceInteraction {
    // 12AM-12PM = 720 minutes
    //FeedAMMinInteraction() { desc = (resource_description_t){"feed_am_min", true, {0, 720-1}}; }
    virtual int read() { return (int)(feedTime / 60); }
    virtual void write(int v) {
        feedTime = (time_t)(v*60);
        set_feed_timer();
    }
};

#define RESOURCE_COUNT 3

ResourceInteraction *resource_interactions[RESOURCE_COUNT] = {
    new FeedNowInteraction(),
    new ServoNeutralInteraction(),
    new FeedAMMinInteraction()
};

void setup_server() {
    server.begin();

    Serial.println("Started server");

    resource_description_t resources[RESOURCE_COUNT] = {
        {"feed_now", true, {0, 1}},
        {"servo_neutral", true, {0, 180}},
        {"feed_am_min", true, {0, 720-1}} // 12AM-12PM = 720 minutes
    };
    restServer.register_resources(resources, RESOURCE_COUNT);
    // restServer.set_post_with_get(true);

    Serial.print(F("Registered resources with server: "));
    Serial.print(String(restServer.get_server_state()));
    Serial.println("!");
}

void setup_alarm() {
    set_feed_timer();
}

void setup_ethernet() {
    // Sprinkle some magic pixie dust. (disable SD SPI to fix bugs)
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH);

    Ethernet.begin(ENET_MAC, ENET_IP);
    // Disable w5100 SPI
    digitalWrite(10, HIGH);
}

void setup()
{
    // Serial port
    Serial.begin(9600);
    while (!Serial)
        ;

    // Ethernet
    Serial.println(F("Setting up ethernet"));
    setup_ethernet();

    // NTP
    Serial.println(F("Setting up NTP"));
    NTP::setup(Serial);

    // REST server
    setup_server();

    // Feeder servo
    Serial.println(F("Setting up FeedServo"));
    FeedServo::setup();

    // Setup the alarm
    setup_alarm();
}

void handle_resources(RestServer &serv) {
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        if (serv.resource_updated(i)) {
            resource_interactions[i]->write(serv.resource_get_state(i));
        }
        if (serv.resource_requested(i)) {
            serv.resource_set_state(i, resource_interactions[i]->read());
        }
    }
}

void handle_server() {
    EthernetClient client = server.available();
    if (client) {
        while (client.connected()) {
            if (restServer.handle_requests(client)) {
                handle_resources(restServer);
                restServer.respond();
            }

            if (restServer.handle_response(client))
                break;

        }
        Alarm.delay(1);
        client.stop();
    }
}

void loop() {
    handle_server();
    Alarm.delay(0); // Service any alarms
}
