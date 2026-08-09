// ---------------------------------------------------------------------------
// worker.cpp — background network worker task
//
// See worker.h for the threading contract. The task owns the WiFi/LedFx link:
// it connects, re-authenticates (via Net), auto-reconnects WiFi when idle, and
// turns each queued Request into HTTP calls, posting Results back to the UI.
// ---------------------------------------------------------------------------
#include "worker.h"
#include "net.h"
#include "config.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

extern Config g_config;  // defined in main.cpp

static QueueHandle_t s_req_q = nullptr;
static QueueHandle_t s_res_q = nullptr;

static const int      QUEUE_LEN   = 12;
static const uint32_t WORKER_STACK = 12288;  // HTTP client + JSON parse headroom
static const TickType_t IDLE_TICKS = pdMS_TO_TICKS(3000);  // maintenance cadence

// ---- Result posting (worker side) -----------------------------------------
static void post_action(int status, const char *msg) {
    Result r{};
    r.type = RES_ACTION;
    r.status = status;
    strncpy(r.msg, msg, sizeof(r.msg) - 1);
    xQueueSend(s_res_q, &r, 0);
}

static void post_conn(bool connected, const char *msg) {
    Result r{};
    r.type = RES_CONN;
    r.connected = connected;
    strncpy(r.msg, msg, sizeof(r.msg) - 1);
    xQueueSend(s_res_q, &r, 0);
}

// Post a data array, transferring ownership. If the queue can't take it, free
// the array here so it never leaks.
static void post_data(ResType type, int status, void *data, int count, const char *err) {
    Result r{};
    r.type = type;
    r.status = status;
    r.data = data;
    r.count = count;
    if (status != 200 && err) strncpy(r.msg, err, sizeof(r.msg) - 1);
    if (xQueueSend(s_res_q, &r, 0) != pdTRUE) {
        if (type == RES_SCENES) delete[] static_cast<SceneInfo *>(data);
        else                    delete[] static_cast<VirtualInfo *>(data);
    }
}

// ---- Connection management -------------------------------------------------
// Ensure the link is usable. "Connected" means WiFi is up: LedFx authentication
// is OPTIONAL (mainline LedFx has no login endpoint), so we only try to log in
// when the user supplied credentials, and never block the API on it. A real
// 401/403 from a data endpoint is surfaced later by the fetch itself.
static bool ensure_connected() {
    if (!net.wifi_connected()) {
        Serial.printf("[net] WiFi connecting to \"%s\"…\n", g_config.wifi_ssid.c_str());
        if (!net.connect_wifi(g_config.wifi_ssid, g_config.wifi_pass)) {
            Serial.println("[net] WiFi connect failed");
            post_conn(false, "WiFi: connecting…");
            return false;
        }
        Serial.print("[net] WiFi up, IP ");
        Serial.println(WiFi.localIP());
    }

    bool want_auth = g_config.ledfx_user.length() || g_config.ledfx_pass.length();
    if (want_auth && !net.has_token()) {
        Serial.println("[net] attempting LedFx login…");
        if (g_ledfx.connect(g_config.ledfx_user, g_config.ledfx_pass))
            Serial.println("[net] LedFx login ok");
        else
            Serial.println("[net] LedFx login failed — continuing without a token");
    }

    post_conn(true, "Connected");
    return true;
}

// Fetch helpers that post the fresh list back to the UI.
static void do_fetch_scenes() {
    SceneInfo *arr = nullptr;
    int n = 0;
    int code = g_ledfx.fetch_scenes(arr, n);
    Serial.printf("[net] GET /api/scenes -> HTTP %d, %d scenes\n", code, n);
    post_data(RES_SCENES, code, arr, n, "Failed to fetch scenes");
}

static void do_fetch_virtuals() {
    VirtualInfo *arr = nullptr;
    int n = 0;
    GlobalsState g;
    int code = g_ledfx.fetch_virtuals(arr, n, &g);
    Serial.printf("[net] GET /api/virtuals -> HTTP %d, %d virtuals\n", code, n);
    // Post the list plus the global state (pause/brightness/mirror/flip).
    Result r{};
    r.type = RES_VIRTUALS;
    r.status = code;
    r.data = arr;
    r.count = n;
    r.globals = g;
    if (code != 200) strncpy(r.msg, "Failed to fetch virtuals", sizeof(r.msg) - 1);
    if (xQueueSend(s_res_q, &r, 0) != pdTRUE) delete[] arr;  // avoid leak if full
}

// ---- Request handling ------------------------------------------------------
static void handle(const Request &req) {
    if (!ensure_connected()) {
        // Can't do anything useful without a link; RES_CONN already told the UI.
        return;
    }

    switch (req.type) {
        case REQ_CONNECT:
            // ensure_connected() (above) already logged + reported success.
            Serial.printf("[net] base_url=\"%s\", auth=%s\n",
                          g_config.ledfx_url.c_str(),
                          (g_config.ledfx_user.length() || g_config.ledfx_pass.length())
                              ? "yes" : "none");
            break;

        case REQ_FETCH_SCENES:
            do_fetch_scenes();
            break;

        case REQ_FETCH_VIRTUALS:
            do_fetch_virtuals();
            break;

        case REQ_ACTIVATE_SCENE: {
            int c = g_ledfx.activate_scene(req.id);
            post_action(c, c == 200 ? "Scene activated" : "Activate failed");
            do_fetch_scenes();  // reflect new active state
            break;
        }
        case REQ_DEACTIVATE_SCENE: {
            int c = g_ledfx.deactivate_scene(req.id);
            post_action(c, c == 200 ? "Scene deactivated" : "Deactivate failed");
            do_fetch_scenes();
            break;
        }
        case REQ_SET_VIRTUAL_ACTIVE: {
            int c = g_ledfx.set_virtual_active(req.id, req.flag);
            post_action(c, c == 200 ? (req.flag ? "Virtual on" : "Virtual off")
                                    : "Toggle failed");
            do_fetch_virtuals();
            break;
        }
        case REQ_RANDOMIZE_VIRTUAL: {
            int c = g_ledfx.randomize_virtual(req.id);
            post_action(c, c == 200 ? "Randomized" : "Randomize failed");
            do_fetch_virtuals();
            break;
        }
        case REQ_CLEAR_ALL: {
            int c = g_ledfx.clear_all_effects();
            post_action(c, c == 200 ? "Cleared all effects" : "Clear failed");
            do_fetch_virtuals();
            break;
        }
        case REQ_PAUSE_ALL: {
            int c = g_ledfx.pause_all(req.flag);
            post_action(c, c == 200 ? (req.flag ? "Paused all" : "Resumed all")
                                    : "Pause failed");
            do_fetch_virtuals();
            break;
        }
        case REQ_APPLY_GLOBAL: {
            int c = g_ledfx.apply_global(req.payload);
            post_action(c, c == 200 ? "Applied" : "Apply failed");
            break;
        }
        case REQ_SET_GRADIENT: {
            int c = g_ledfx.set_gradient(req.payload);
            post_action(c, c == 200 ? "Gradient set" : "Gradient failed");
            break;
        }
    }
}

static void worker_task(void *) {
    for (;;) {
        Request req;
        if (xQueueReceive(s_req_q, &req, IDLE_TICKS) == pdTRUE) {
            handle(req);
        } else {
            // Idle: keep the link healthy so it self-heals after a WiFi drop.
            if (!net.wifi_connected()) ensure_connected();
        }
    }
}

// ---- Public API ------------------------------------------------------------
void worker_init() {
    if (s_req_q) return;  // already initialised
    s_req_q = xQueueCreate(QUEUE_LEN, sizeof(Request));
    s_res_q = xQueueCreate(QUEUE_LEN, sizeof(Result));
    xTaskCreatePinnedToCore(worker_task, "ledfx_net", WORKER_STACK, nullptr, 1, nullptr, 0);
}

bool worker_submit(const Request &req) {
    if (!s_req_q) return false;
    return xQueueSend(s_req_q, &req, 0) == pdTRUE;
}

bool worker_poll(Result &out) {
    if (!s_res_q) return false;
    return xQueueReceive(s_res_q, &out, 0) == pdTRUE;
}

// ---- Request builders ------------------------------------------------------
Request req_simple(ReqType t) {
    Request r{};
    r.type = t;
    return r;
}

Request req_id(ReqType t, const String &id, bool flag) {
    Request r{};
    r.type = t;
    r.flag = flag;
    strncpy(r.id, id.c_str(), sizeof(r.id) - 1);
    return r;
}

Request req_payload(ReqType t, const String &payload) {
    Request r{};
    r.type = t;
    strncpy(r.payload, payload.c_str(), sizeof(r.payload) - 1);
    return r;
}
