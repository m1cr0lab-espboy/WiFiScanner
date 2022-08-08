/**
 * ----------------------------------------------------------------------------
 * @file   main.cpp
 * @author Stéphane Calderoni (https://github.com/m1cr0lab)
 * @brief  WiFi network scanner for the ESPboy
 * ----------------------------------------------------------------------------
 */

#include <ESPboy.h>
#include <ESP8266WiFi.h>
#include "font.h"

// ----------------------------------------------------------------------------
// Global constants
// ----------------------------------------------------------------------------

uint8_t constexpr LOCK_ICON_SIZE    = 5;
uint8_t const constexpr LOCK_ICON[] = { 0x70, 0x88, 0x88, 0xf8, 0xf8 };

uint8_t constexpr DBM_ICON_WIDTH   = 13;
uint8_t constexpr DBM_ICON_HEIGHT  = 5;
uint8_t const constexpr DBM_ICON[] = { 0x24, 0x00, 0x2a, 0x50, 0x6c, 0xa8, 0xaa, 0xa8, 0x6c, 0xa8 };

uint8_t  constexpr CHANNELS           = 13;
uint8_t  constexpr MAX_AP_PER_CHANNEL = 4;
uint8_t  constexpr MAX_SSID_LENGTH    = 14;
uint16_t constexpr SCAN_PERIOD_MS     = 3000;
uint8_t  constexpr GUI_HEIGHT         = TFT_HEIGHT >> 1;
uint8_t  constexpr BAR_WIDTH          = 8;
uint8_t  constexpr BAR_GAP            = 1;
uint8_t  constexpr GRAPH_WIDTH        = BAR_WIDTH * CHANNELS + (CHANNELS - 1) * BAR_GAP;
uint8_t  constexpr GRAPH_HEIGHT       = GUI_HEIGHT - (FONT_SIZE + 1);
uint8_t  constexpr BAR_HEIGHT         = (GRAPH_HEIGHT << 1) / 3;
uint8_t  constexpr GRAPH_H_MARGIN     = (TFT_WIDTH - GRAPH_WIDTH) >> 1;
float_t  constexpr GRAPH_ZOOM_X       = 1.f - (2.f / (float)BAR_HEIGHT);
float_t  constexpr GRAPH_ZOOM_Y       = 1.f - (2.f / (float)GRAPH_WIDTH);

uint16_t constexpr COLOR_BLUE         = 0x667f; // hsl(200, 100,  70)
uint16_t constexpr COLOR_ORANGE       = 0xfe6C; // hsl( 40, 100,  70)
uint16_t constexpr COLOR_GREY         = 0xa514; // rgb(160, 160, 160)
uint16_t constexpr COLOR_DARK_GREY    = 0x2104; // rgb( 32,  32,  32)

// ----------------------------------------------------------------------------
// Global variables
// ----------------------------------------------------------------------------

enum class State : uint8_t { INIT, FIRST_SCAN, SCAN };

State state = State::INIT;

struct WiFiAP{
    char    ssid[MAX_SSID_LENGTH + 1];
    int32_t rssi;
    uint8_t quality;
    bool    locked;
};

WiFiAP ap[CHANNELS][MAX_AP_PER_CHANNEL];

bool     scanning = false;
uint8_t  best_quality;
uint8_t  nb_of_devices;
uint8_t  higher_nb_of_devices;
uint8_t  quality[CHANNELS];
uint8_t  devices[CHANNELS];
uint8_t  current_channel;

LGFX_Sprite *gui    = nullptr;
LGFX_Sprite *graph1 = nullptr;
LGFX_Sprite *graph2 = nullptr;
bool         flip   = false;

// ----------------------------------------------------------------------------
// Graphics rendering
// ----------------------------------------------------------------------------

void drawChannels(uint8_t const c = 0xff) {

    char digit[2];
    
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        snprintf(digit, 2, "%X", (i + 1) & 0xf);
        drawString(
            &espboy.tft,
            digit,
            GRAPH_H_MARGIN + i * (BAR_WIDTH + BAR_GAP) + (BAR_WIDTH >> 1),
            TFT_HEIGHT - FONT_SIZE,
            c != 0xff && c == i ? TFT_WHITE : COLOR_GREY,
            Align::CENTER
        );
    }
}

void drawGraph() {

    LGFX_Sprite * const fb1 = flip ? graph2 : graph1;
    LGFX_Sprite * const fb2 = flip ? graph1 : graph2;

    fb1->clear();

    fb2->pushRotateZoomWithAA(
        fb1,
        GRAPH_WIDTH >> 1,
        (GRAPH_HEIGHT >> 1) - 2,
        0,
        GRAPH_ZOOM_X,
        GRAPH_ZOOM_Y
    );

    uint16_t * const fb  = (uint16_t*)fb1->getBuffer();
    uint16_t   const len = GRAPH_WIDTH * GRAPH_HEIGHT;
    uint16_t color;
    uint8_t  r, g, b;

    for (uint16_t i = 0; i < len; ++i) {
        if (fb[i]) {
            // swaps endianness
            color = fb[i] >> 8 | fb[i] << 8;
            // extracts the primary colors
            r = color >> 11;
            g = (color >> 5) & 0x3f;
            b = color & 0x1f;
            // lowers luminance
            r = r > 1 ? r - 1 : 0;
            g = g > 2 ? g - 2 : 0;
            b = b > 1 ? b - 1 : 0;
            // repacks the RGB565 color
            color = r << 11 | g << 5 | b;
            // restores endianness
            fb[i] = color << 8 | color >> 8;
        }
    }

    for (uint8_t i = 0; i < CHANNELS; ++i) {
        uint8_t  const x = i * (BAR_WIDTH + BAR_GAP);
        uint8_t  const q = quality[i];
        uint8_t  const n = devices[i];
        if (n == 0) {
            fb1->drawFastHLine(x, GRAPH_HEIGHT - 1, BAR_WIDTH, TFT_RED);
        } else {
            uint8_t const h = map(n, 0, higher_nb_of_devices, 0, BAR_HEIGHT);
            fb1->drawRect(x, GRAPH_HEIGHT - h, BAR_WIDTH, h, COLOR_DARK_GREY);
            for (uint8_t y = 0; y < h - 1; ++y) {
                fb1->drawFastHLine(
                    x + 1,
                    GRAPH_HEIGHT - 1 - y,
                    BAR_WIDTH - 2,
                    Color::hsv2rgb565(map(y, 0, h - 1, 0, 120 * q / best_quality))
                );
            }
        }
    }

    fb1->pushSprite(GRAPH_H_MARGIN, TFT_HEIGHT - GRAPH_HEIGHT - FONT_SIZE - 1);

    flip = !flip;

}

void drawGUI() {

    gui->clear();

    if (current_channel != 0xff) {

        uint8_t const max_len = 12;
        char text[max_len];

        snprintf(text, max_len, "CH %u", current_channel + 1);
        drawString(gui, text, 0, 0);
        snprintf(text, max_len, "%u/%u AP", devices[current_channel], nb_of_devices);
        drawString(gui, text, TFT_WIDTH - 1, 0, COLOR_GREY, Align::RIGHT);

        uint8_t n = devices[current_channel];

        if (n == 0) {

            uint8_t const y = LINE_HEIGHT + ((GUI_HEIGHT - (LINE_HEIGHT << 1) - (FONT_SIZE + 1)) >> 1);
            drawString(gui, F("No network"),      TFT_WIDTH >> 1, y,               TFT_ORANGE, Align::CENTER);
            drawString(gui, F("on this channel"), TFT_WIDTH >> 1, y + LINE_HEIGHT, TFT_ORANGE, Align::CENTER);

        } else {

            snprintf(text, max_len, "Top %u", n < MAX_AP_PER_CHANNEL ? n : MAX_AP_PER_CHANNEL);
            drawString(gui, text, TFT_WIDTH >> 1, LINE_HEIGHT, TFT_YELLOW, Align::CENTER);

            uint8_t const xr = TFT_WIDTH - LOCK_ICON_SIZE - FONT_SIZE - DBM_ICON_WIDTH;

            for (uint8_t i = 0; i < n; ++i) {

                WiFiAP const * const wap = &ap[current_channel][i];
                uint8_t y = LINE_HEIGHT + (i + 1) * LINE_HEIGHT;

                snprintf(text, max_len, "%i", wap->rssi);
                drawString(gui, wap->ssid, 0, y, COLOR_BLUE);
                drawString(gui, text, xr - 3, y, COLOR_BLUE, Align::RIGHT);

                gui->drawBitmap(
                    xr,
                    y,
                    DBM_ICON,
                    DBM_ICON_WIDTH,
                    DBM_ICON_HEIGHT,
                    COLOR_BLUE
                );

                if (wap->locked) {
                    gui->drawBitmap(
                        TFT_WIDTH - LOCK_ICON_SIZE,
                        y,
                        LOCK_ICON,
                        LOCK_ICON_SIZE,
                        LOCK_ICON_SIZE,
                        COLOR_ORANGE
                    );
                }

            }

        }

    }

    gui->pushSprite(0, 0);

}

// ----------------------------------------------------------------------------
// WiFi network scanning
// ----------------------------------------------------------------------------

void sortNetworks() {

    for (uint8_t c = 0; c < CHANNELS; ++c) {
        uint8_t const n = devices[c];
        if (n > 1) {
            for (uint8_t i = 0; i < n - 1; ++i) {
                for (uint8_t j = 1; j < n; ++j) {
                    if (ap[c][i].rssi < ap[c][j].rssi) {
                        WiFiAP tmp;
                        memcpy(&tmp,      &ap[c][i], sizeof(WiFiAP));
                        memcpy(&ap[c][i], &ap[c][j], sizeof(WiFiAP));
                        memcpy(&ap[c][j], &tmp,      sizeof(WiFiAP));
                    }
                }
            }
        }
    }

}

void parseNetworks(int n) {

    if (n == 0) { scanning = false; return; }

    switch (state) {
        case State::INIT:       state = State::FIRST_SCAN; break;
        case State::FIRST_SCAN: state = State::SCAN;       break;
        default:;
    }

    best_quality         = 0;
    nb_of_devices        = 0;
    higher_nb_of_devices = 0;
    memset(quality, 0, CHANNELS);
    memset(devices, 0, CHANNELS);

    for (uint8_t i = 0; i < n; ++i) {

        uint8_t const c = WiFi.channel(i) - 1;
        uint8_t const l = strlen(WiFi.SSID(i).c_str());
        uint8_t const e = WiFi.encryptionType(i);
        int32_t const r = WiFi.RSSI(i);
        uint8_t const q = (r >= -50) ? 100 : ((r <= -100) ? 0 : (100 + r) << 1);

        if (devices[c] <= MAX_AP_PER_CHANNEL) {

            WiFiAP * const wap = &ap[c][devices[c]];

            strncpy(wap->ssid, WiFi.SSID(i).c_str(), MAX_SSID_LENGTH + 1);
            if (l > MAX_SSID_LENGTH) {
                wap->ssid[MAX_SSID_LENGTH - 1] = 127;
                wap->ssid[MAX_SSID_LENGTH ]    = 0;
            }

            wap->locked  = (e != ENC_TYPE_NONE);
            wap->rssi    = r;
            wap->quality = q;

        }

        if (quality[c]   < q) quality[c]   = q;
        if (best_quality < q) best_quality = q;
        devices[c]++;
        nb_of_devices++;
        if (higher_nb_of_devices < devices[c]) higher_nb_of_devices = devices[c];

    }

    WiFi.scanDelete();
    
    scanning = false;

    sortNetworks();

    drawGraph();

}

void scanNetworks() {

    static uint32_t       last_ms = 0;
           uint32_t const now     = millis();
    
    if (now - last_ms < SCAN_PERIOD_MS) return;

    last_ms = now;

    if (scanning) return;

    scanning = true;
    WiFi.scanNetworksAsync(parseNetworks, true);

}

uint8_t seekBestQualityChannel() {

    uint8_t q = 0, c = 0xff;
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        if (q < quality[i]) {
            q = quality[i];
            c = i;
        }
    }

    return c;
    
}

void seekNextChannel(bool const to_the_right = false) {

    if (current_channel ==  0 && !to_the_right) return;
    if (current_channel == 12 &&  to_the_right) return;

    uint8_t cc = current_channel, c = cc;

    if (to_the_right) {
        while (c < 12 && cc == current_channel) {
            if (quality[++c]) cc = c;
        }
    } else {
        while (c > 0 && cc == current_channel) {
            if (quality[--c]) cc = c;
        }
    }

    if (cc != current_channel) drawChannels(current_channel = cc);

}

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

void setup() {

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    espboy.begin();

    gui    = new LGFX_Sprite(&espboy.tft);
    graph1 = new LGFX_Sprite(&espboy.tft);
    graph2 = new LGFX_Sprite(&espboy.tft);
    gui->createSprite(TFT_WIDTH, GUI_HEIGHT);
    graph1->createSprite(GRAPH_WIDTH, GRAPH_HEIGHT);
    graph2->createSprite(GRAPH_WIDTH, GRAPH_HEIGHT);

    drawString(
        &espboy.tft,
        F("Scanning WiFi networks"),
        TFT_WIDTH >> 1,
        TFT_HEIGHT >> 1,
        TFT_WHITE,
        Align::CENTER
    );

}

// ----------------------------------------------------------------------------
// Updates the user interface
// ----------------------------------------------------------------------------

void update() {

         if (espboy.button.pressed(Button::LEFT))  seekNextChannel();
    else if (espboy.button.pressed(Button::RIGHT)) seekNextChannel(true);

    drawGUI();

}

// ----------------------------------------------------------------------------
// Main program
// ----------------------------------------------------------------------------

void loop() {

    espboy.update();

    switch (state) {

        case State::INIT:
            if (espboy.fading()) return;
            scanNetworks();
            break;

        case State::FIRST_SCAN:
            drawChannels(current_channel = seekBestQualityChannel());
            state = State::SCAN;
            break;

        case State::SCAN:
            scanNetworks();
            update();

    }

}

/*
 * ----------------------------------------------------------------------------
 * ESPboy WiFi Scanner
 * ----------------------------------------------------------------------------
 * Copyright (c) 2022 Stéphane Calderoni (https://github.com/m1cr0lab)
 * 
 *     Based on an original idea from tobozo (https://github.com/tobozo)
 *     => https://twitter.com/TobozoTagada/status/1469018514702974981
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------
 */