#include "wifi_setup.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <stdio.h>

// Small fixed profile list: no passwords appear in status text or serial logs.
static constexpr int MAX_PROFILES = 4;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
static String ssids[MAX_PROFILES];
static String passwords[MAX_PROFILES];
static int profile_count = 0;
static int current_profile = 0;
static uint32_t connect_since = 0;
static bool station_running = false;
static bool portal_active = false;
static uint32_t portal_saved_at = 0;
static WebServer server(80);
static DNSServer dns;

static void log_heap(const char* phase) {
    Serial.printf("Wi-Fi %s: internal heap free=%u largest=%u\n", phase,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void start_profile(int index) {
    if (index < 0 || index >= profile_count) return;
    current_profile = index;
    station_running = true;
    connect_since = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssids[index].c_str(), passwords[index].c_str());
    Serial.printf("Wi-Fi: connecting to profile %d (%s)\n", index + 1, ssids[index].c_str());
    log_heap("STA");
}

static String html_escape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        switch (value[i]) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += value[i]; break;
        }
    }
    return escaped;
}

static void show_form() {
    String page;
    page.reserve(3400);
    page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Clawdmeter Wi-Fi</title><style>body{font:17px system-ui;max-width:480px;margin:30px auto;"
             "padding:0 18px;background:#171720;color:#fff}input,select,button{box-sizing:border-box;width:100%;"
             "padding:12px;margin:8px 0 20px;font:inherit}button{background:#d97757;color:white;border:0}"
             "label{display:block}</style></head><body><h1>Clawdmeter Wi-Fi</h1>"
             "<p>Choose a nearby 2.4 GHz network or enter its name.</p><form method='post' action='/save'>"
             "<label>Nearby networks</label><select id='nearby' onchange='document.getElementById(\"ssid\").value=this.value'>"
             "<option value=''>Select a network</option>");
    const int scanned = WiFi.scanComplete();
    if (scanned >= 0) {
        for (int i = 0; i < scanned && i < 30; ++i) {
            const String ssid = WiFi.SSID(i);
            if (ssid.isEmpty()) continue;
            const String safe = html_escape(ssid);
            page += "<option value=\"" + safe + "\">" + safe + "</option>";
        }
    }
    page += F("</select><label>Network name</label><input id='ssid' name='ssid' maxlength='32' required>"
              "<label>Password</label><input name='password' type='password' maxlength='63' autocomplete='off'>"
              "<button type='submit'>Save and connect</button></form>"
              "<p>Open networks can leave the password blank. Saved networks reconnect automatically.</p>");
    if (scanned == WIFI_SCAN_RUNNING) page += F("<p>Scanning nearby networks; reload this page in a moment.</p>");
    page += F("</body></html>");
    server.send(200, "text/html", page);
}

static void save_profile() {
    const String ssid = server.arg("ssid");
    const String password = server.arg("password");
    if (ssid.isEmpty() || ssid.length() > 32 ||
        (password.length() > 0 && password.length() < 8) || password.length() > 63) {
        server.send(400, "text/plain", "Invalid network name or password length.");
        return;
    }
    int slot = -1;
    for (int i = 0; i < profile_count; ++i)
        if (ssids[i] == ssid) { slot = i; break; }
    if (slot < 0) {
        if (profile_count < MAX_PROFILES) slot = profile_count++;
        else {
            for (int i = 1; i < MAX_PROFILES; ++i) {
                ssids[i - 1] = ssids[i];
                passwords[i - 1] = passwords[i];
            }
            slot = MAX_PROFILES - 1;
        }
    }
    ssids[slot] = ssid;
    passwords[slot] = password;
    Preferences prefs;
    if (!prefs.begin("clawdwifi", false)) {
        server.send(500, "text/plain", "Could not save network.");
        return;
    }
    prefs.putUChar("count", profile_count);
    for (int i = 0; i < profile_count; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "ssid%d", i);
        prefs.putString(key, ssids[i]);
        snprintf(key, sizeof(key), "pass%d", i);
        prefs.putString(key, passwords[i]);
    }
    prefs.end();
    current_profile = slot;
    portal_saved_at = millis();
    server.send(200, "text/html", "<h1>Saved</h1><p>Clawdmeter is connecting. You can close this page.</p>");
    Serial.printf("Wi-Fi: saved profile %d (%s)\n", slot + 1, ssid.c_str());
}

void c6_wifi_init(void) {
    Preferences prefs;
    if (prefs.begin("clawdwifi", true)) {
        const int count = prefs.getUChar("count", 0);
        for (int i = 0; i < count && i < MAX_PROFILES; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "ssid%d", i);
            const String ssid = prefs.getString(key, "");
            snprintf(key, sizeof(key), "pass%d", i);
            const String pass = prefs.getString(key, "");
            if (ssid.length() > 0 && ssid.length() <= 32) {
                ssids[profile_count] = ssid;
                passwords[profile_count++] = pass;
            }
        }
        prefs.end();
    }
    WiFi.persistent(false);
    if (profile_count) start_profile(0);
}

void c6_wifi_start_portal(void) {
    if (portal_active) return;
    station_running = false;
    portal_saved_at = 0;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP("Clawdmeter-Setup", "clawdmeter-setup")) {
        Serial.println("Wi-Fi: setup AP failed");
        return;
    }
    portal_active = true;
    dns.start(53, "*", WiFi.softAPIP());
    static bool routes_added = false;
    if (!routes_added) {
        routes_added = true;
        server.on("/", HTTP_GET, show_form);
        server.on("/save", HTTP_POST, save_profile);
        server.on("/generate_204", HTTP_GET, []() {
            server.sendHeader("Location", "http://192.168.4.1/");
            server.send(302, "text/plain", "");
        });
        server.onNotFound(show_form);
    }
    server.begin();
    WiFi.scanNetworks(true);
    Serial.println("Wi-Fi: setup AP at 192.168.4.1");
    log_heap("AP");
}

void c6_wifi_stop_portal(void) {
    if (!portal_active) return;
    server.stop();
    dns.stop();
    WiFi.scanDelete();
    WiFi.softAPdisconnect(true);
    portal_active = false;
    if (profile_count) start_profile(current_profile);
    else WiFi.mode(WIFI_OFF);
}

bool c6_wifi_portal_active(void) { return portal_active; }

void c6_wifi_tick(void) {
    if (portal_active) {
        dns.processNextRequest();
        server.handleClient();
        if (portal_saved_at && millis() - portal_saved_at > 1500) c6_wifi_stop_portal();
        return;
    }
    if (!station_running || profile_count == 0) return;
    if (WiFi.status() == WL_CONNECTED) return;
    const uint32_t now = millis();
    if (now - connect_since < CONNECT_TIMEOUT_MS) return;
    const int next = (current_profile + 1) % profile_count;
    start_profile(next);
}

void c6_wifi_status(char* out, size_t capacity) {
    if (!capacity) return;
    String known;
    for (int i = 0; i < profile_count; ++i) {
        if (i) known += ", ";
        known += ssids[i].substring(0, 12);
    }
    if (known.isEmpty()) known = "none";
    if (portal_active) {
        snprintf(out, capacity, "Setup AP: Clawdmeter-Setup\nOpen 192.168.4.1 on your phone\nKnown: %s", known.c_str());
    } else if (WiFi.status() == WL_CONNECTED) {
        snprintf(out, capacity, "Wi-Fi: %s\nIP: %s\nKnown: %s",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), known.c_str());
    } else if (profile_count) {
        snprintf(out, capacity, "Wi-Fi: connecting to %s\nKnown: %s", ssids[current_profile].c_str(), known.c_str());
    } else {
        snprintf(out, capacity, "Wi-Fi: not configured\nKnown: none");
    }
}
