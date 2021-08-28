#include <memory>
#include <cstdint>
#include <iostream>
#include <httpserver.hpp>
#include "ws2812-rpi.h"
#include <thread>
#include <vector>
#include <deque>
#include <string>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <wiringPi.h>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Options.hpp>

#define PORT 4792
#define PIXELS 256
#define BUTTON_PIN 4

using namespace std;
using namespace httpserver;


int fps_value = 30;
float brightness = 0.1;

float max_delay = 0.4, min_delay = 0.001;
float start = 0, timeout = 1;
bool cleared = true, running = true;
string button_callback;
NeoPixel *pixels = new NeoPixel(PIXELS);
deque<int> pushed_frames;
float pressed_time = 0, last_click_time = 0,
        hold_timeout = 0.5, click_timeout = 0.3;
int click_count = 0;
bool last_hold = false, pressed = false;

int frames_size = 0;
int command = 0;

float get_time() { return pixels->millis() / 1000.0; }

float get_delay() { return 1.0 / fps_value; }

int get_frames() { return frames_size / (PIXELS * 3); }


const shared_ptr<http_response> not_found_custom(const http_request &req) {
    return shared_ptr<string_response>(new string_response("Oops... Not found :(", 404, "text/plain"));
}

const shared_ptr<http_response> not_allowed_custom(const http_request &req) {
    return shared_ptr<string_response>(new string_response("Oops... Not allowed :(", 405, "text/plain"));
}


class index_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        return shared_ptr<http_response>(new string_response("ok"));
    }
};

class shutdown_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        running = false;
        return shared_ptr<http_response>(new string_response("ok"));
    }
};

class brightness_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        int br = min(255, max(0, stoi(req.get_arg("value"))));
        pixels->setBrightness(float(br) / 255.0);
        return shared_ptr<http_response>(new string_response("ok"));
    }
};


class fps_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        fps_value = min(100, max(1, stoi(req.get_arg("value"))));
        return shared_ptr<http_response>(new string_response("ok"));
    }
};

class clear_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        command = 1;
        return shared_ptr<http_response>(new string_response("ok"));
    }
};

class animation_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        if (req.get_arg("clear") == "1")command = 1;
        if (req.get_arg("fps").size()) {
            fps_value = min(100, max(1, stoi(req.get_arg("fps"))));
        }
        deque<int> frames;
        for (int ch:req.get_content()) frames.push_back(ch);
        cleared = false;
        start = 0;
        pushed_frames = frames;
        return shared_ptr<http_response>(new string_response(
                to_string(get_frames()) + "," + to_string(get_delay())));
    }
};

class button_callback_res : public http_resource {
public:
    const shared_ptr<http_response> render(const http_request &req) {
        button_callback = req.get_arg("url");
        return shared_ptr<http_response>(new string_response("ok"));
    }
};


void start_server() {
    webserver ws = create_webserver(PORT)
            .not_found_resource(not_found_custom)
            .method_not_allowed_resource(not_allowed_custom);

    index_res index_res_;
    index_res_.disallow_all();
    index_res_.set_allowing("POST", true);
    index_res_.set_allowing("GET", true);
    ws.register_resource("/", &index_res_);

    animation_res animation_res_;
    animation_res_.disallow_all();
    animation_res_.set_allowing("POST", true);
    animation_res_.set_allowing("GET", true);
    ws.register_resource("/animation", &animation_res_);

    shutdown_res shutdown_res_;
    shutdown_res_.disallow_all();
    shutdown_res_.set_allowing("POST", true);
    shutdown_res_.set_allowing("GET", true);
    ws.register_resource("/shutdown", &shutdown_res_);

    brightness_res brightness_res_;
    brightness_res_.disallow_all();
    brightness_res_.set_allowing("POST", true);
    brightness_res_.set_allowing("GET", true);
    ws.register_resource("/brightness/{value|[0-9]+}", &brightness_res_);

    fps_res speed_res_;
    speed_res_.disallow_all();
    speed_res_.set_allowing("POST", true);
    speed_res_.set_allowing("GET", true);
    ws.register_resource("/fps/{value|[0-9]+}", &speed_res_);

    clear_res clear_res_;
    clear_res_.disallow_all();
    clear_res_.set_allowing("POST", true);
    clear_res_.set_allowing("GET", true);
    ws.register_resource("/clear", &clear_res_);

    button_callback_res button_callback_res_;
    button_callback_res_.disallow_all();
    button_callback_res_.set_allowing("POST", true);
    button_callback_res_.set_allowing("GET", true);
    ws.register_resource("/button_callback", &button_callback_res_);

    cout << "Server started\n";
    ws.start(true);
}


void start_button() {

    wiringPiSetup();
    pinMode(BUTTON_PIN, INPUT);
    pinMode(16, INPUT);
    cout << "Button started\n";
    while (running) {
        bool current_pressed = digitalRead(BUTTON_PIN);
        if (current_pressed && !pressed) {
            pressed_time = get_time();
        }
        if (!current_pressed && pressed && pressed_time) {
            float hold_time = get_time() - pressed_time;
            pressed_time = 0;
            last_hold = hold_time > hold_timeout;
            last_click_time = get_time();
            click_count++;
        }

        bool extra_hold = false;
        if (pressed_time) {
            float hold_time = get_time() - pressed_time;
            if (hold_time > hold_timeout) {
                pressed_time = 0;
                click_count++;
                last_hold = true;
                extra_hold = true;
            }
        }
        if ((extra_hold || get_time() - last_click_time > click_timeout) &&
            !pressed_time &&
            click_count > 0) {
            if (button_callback.size()) {
                string url = button_callback + "?count=" + to_string(click_count) +
                             "&hold=" + to_string(int(last_hold));
                try {
                    curlpp::Cleanup myCleanup;
                    ostringstream os;
                    os << curlpp::options::Url(string(url));
                } catch (curlpp::LibcurlRuntimeError &e) {}

            }
            click_count = 0;
        }
        pressed = current_pressed;
        usleep(0.05 * 1e6);
    }
    cout << "Button exited...\n";
}

void handler(int s) {
    cout << "Exit...\n";
    running = false;
}


int main() {
    signal(SIGINT, handler);

    thread server_thread(start_server);
    usleep(0.2 * 1e6);

    thread button_thread(start_button);
    usleep(0.2 * 1e6);

    deque<int> frames;
    pixels->setBrightness(brightness);
    pixels->clear();
    pixels->show();
    cout << "Display started\n";
    while (running) {
        frames_size = frames.size();
        if (command) {
            switch (command) {
                case 1:
                    frames = deque<int>(0);
                    break;
            }
            command = 0;
        }
        if (pushed_frames.size()) {
            for (int i:pushed_frames)frames.push_back(i);
            pushed_frames = deque<int>(0);
        }
        if (frames.size() >= PIXELS * 3) {
            for (int i = 0; i < PIXELS; i++) {
                pixels->setPixelColor(i, frames[0], frames[1], frames[2]);
                for (int j = 0; j < 3; j++)frames.pop_front();
            }
            pixels->show();
        } else {
            if (!start) {
                start = get_time();
            }
            if (!cleared) {
                if (get_time() - start >= timeout) {
                    pixels->clear();
                    pixels->show();
                    cleared = true;
                }
            }
        }
        float delay = get_delay();
        usleep(delay * 1e6);
    }
    pixels->clear();
    pixels->show();
    delete pixels;
    cout << "Leds cleared\n";

    usleep(0.2 * 1e6);


    return 0;
}
