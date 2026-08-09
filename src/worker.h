// ---------------------------------------------------------------------------
// worker.h — background network worker (LedFx REST off the render loop)
//
// All LedFx REST I/O runs on a FreeRTOS task pinned to core 0 so the LVGL
// render/touch loop on core 1 never blocks on the network. The UI and the
// worker communicate only through two queues:
//   - the UI enqueues a Request (worker_submit)
//   - the worker performs the HTTP call and posts a Result
//   - a UI-side timer drains Results (worker_poll) and updates widgets
// The worker never touches LVGL; the UI never touches the network client.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include "ledfx.h"   // SceneInfo / VirtualInfo

enum ReqType : uint8_t {
    REQ_CONNECT,             // (re)connect WiFi + login, report status
    REQ_FETCH_SCENES,
    REQ_FETCH_VIRTUALS,
    REQ_ACTIVATE_SCENE,      // id
    REQ_DEACTIVATE_SCENE,    // id
    REQ_SET_VIRTUAL_ACTIVE,  // id + flag
    REQ_RANDOMIZE_VIRTUAL,   // id
    REQ_CLEAR_ALL,
    REQ_PAUSE_ALL,           // flag
    REQ_APPLY_GLOBAL,        // payload = json body
    REQ_SET_GRADIENT,        // payload = preset name
    REQ_SET_BRIGHTNESS,      // arg = 0..100 (global_brightness %)
};

// Fixed buffers (no heap-owning members) so the struct is safe to copy by
// value through a FreeRTOS queue. Ids are LedFx slugs; payloads are small.
struct Request {
    ReqType type;
    bool    flag;
    int     arg;
    char    id[48];
    char    payload[224];
};

enum ResType : uint8_t {
    RES_SCENES,    // data = SceneInfo[count], ownership passes to the UI
    RES_VIRTUALS,  // data = VirtualInfo[count], ownership passes to the UI
    RES_ACTION,    // a command finished; msg + status for the status line
    RES_CONN,      // connection state changed; connected + msg
};

struct Result {
    ResType type;
    int     status;      // HTTP code (200 == ok) or 0
    void   *data;        // SceneInfo* / VirtualInfo* (delete[] after use)
    int     count;
    bool    connected;   // for RES_CONN
    char    msg[64];
    GlobalsState globals;  // valid only on RES_VIRTUALS
};

// Create the queues + task. Call once in setup(), after g_config is loaded and
// g_ledfx.set_base_url() has been set.
void worker_init();

// UI -> worker. Returns false if the worker isn't running or the queue is full.
bool worker_submit(const Request &req);

// worker -> UI. Non-blocking; returns false when no result is pending.
bool worker_poll(Result &out);

// Small Request builders for the common shapes.
Request req_simple(ReqType t);
Request req_id(ReqType t, const String &id, bool flag = false);
Request req_payload(ReqType t, const String &payload);
Request req_arg(ReqType t, int arg);
// For requests that only carry a bool (e.g. REQ_PAUSE_ALL). Prefer this over
// req_id() with an empty id — the empty-id call site is misleading.
Request req_flag(ReqType t, bool flag);
