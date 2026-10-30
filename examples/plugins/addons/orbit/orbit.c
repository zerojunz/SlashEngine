/*
 * A plugin written in C against slash_plugin.h alone, as any plugin is: an Orbit component that
 * moves its entity's Transform2D round a circle, and the system that does it. The plugins example
 * loads it as a library; tests/test_plugin.cpp loads it as one and links it in.
 */
#include "slash/plugin/slash_plugin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct Orbit {
    float center[2];
    float radius;
    float speed; /* radians a second; ui_accept reverses it */
    float angle;
    int32_t laps;
    uint8_t active;
    char label[24];
    SlashEntity notify; /* the entity lap events are about, or none for this one */
} Orbit;

static SlashLog slash_log;
static SlashComponentGet component_get;
static SlashComponentSetJson component_set_json;
static SlashEntityValid entity_valid;
static SlashWorldEach world_each;
static SlashWorldEmit world_emit;
static SlashInputJustPressed input_just_pressed;
static SlashComponent orbit;

static int load(SlashGetProc get_proc, const char* name, void* into) {
    SlashProc proc = get_proc(name);
    memcpy(into, &proc, sizeof proc);
    return proc != NULL;
}

typedef struct Tick {
    SlashWorld* world;
    double seconds;
    int reverse;
} Tick;

static void move(void* userdata, SlashEntity entity, void** components) {
    Tick* tick = (Tick*)userdata;
    Orbit* it = (Orbit*)components[0];
    char json[96];
    if (!it->active) {
        return;
    }
    if (tick->reverse) {
        it->speed = -it->speed;
    }
    it->angle += (float)(it->speed * tick->seconds);
    if (it->angle >= 6.2831853f || it->angle <= -6.2831853f) {
        it->angle = fmodf(it->angle, 6.2831853f);
        it->laps += 1;
        snprintf(json, sizeof json, "{\"laps\": %d, \"label\": \"%s\"}", (int)it->laps, it->label);
        world_emit(tick->world, "lap",
                   entity_valid(tick->world, it->notify) ? it->notify : entity, json);
    }
    snprintf(json, sizeof json, "{\"position\": [%.3f, %.3f]}",
             (double)(it->center[0] + it->radius * cosf(it->angle)),
             (double)(it->center[1] + it->radius * sinf(it->angle)));
    component_set_json(tick->world, entity, "Transform2D", json, NULL, 0);
}

static void run(void* userdata, SlashWorld* world, double seconds) {
    Tick tick = {world, seconds, input_just_pressed(world, "ui_accept")};
    (void)userdata;
    world_each(world, &orbit, 1, move, &tick);
}

static void deinitialize(void* userdata) {
    (void)userdata;
    slash_log(SLASH_LOG_INFO, "orbit plugin unloaded", NULL);
}

SLASH_PLUGIN_EXPORT int orbit_plugin_entry(SlashGetProc get_proc, SlashPlugin* plugin,
                                            SlashPluginInit* init) {
    static const Orbit defaults = {{0.0f, 0.0f}, 32.0f, 1.0f, 0.0f, 0, 1, "", SLASH_NO_ENTITY};
    static const SlashField fields[] = {
        {"center", SLASH_FIELD_VEC2, offsetof(Orbit, center), 0, "What it circles.", 0, 0, 0, 0},
        {"radius", SLASH_FIELD_FLOAT, offsetof(Orbit, radius), 0, "How far from the center, in pixels.", 1, 0, 0, 0},
        {"speed", SLASH_FIELD_FLOAT, offsetof(Orbit, speed), 0, "Radians a second; negative goes clockwise.", 0, 0, 0, 0},
        {"angle", SLASH_FIELD_FLOAT, offsetof(Orbit, angle), 0, "Where it is, in radians.", 0, 0, 0, 0},
        {"laps", SLASH_FIELD_INT32, offsetof(Orbit, laps), 0, "Laps finished.", 1, 0, 0, 0},
        {"active", SLASH_FIELD_BOOL, offsetof(Orbit, active), 0, "Whether it moves.", 0, 0, 0, 0},
        {"label", SLASH_FIELD_STRING, offsetof(Orbit, label), 24, "Named in lap events.", 0, 0, 0, 0},
        {"notify", SLASH_FIELD_ENTITY, offsetof(Orbit, notify), 0, "The entity lap events are about.", 0, 0, 0, 0},
    };
    static const SlashComponentInfo info = {
        "Orbit", "Moves the entity round a circle.", sizeof(Orbit),
        _Alignof(Orbit), &defaults, fields, sizeof fields / sizeof fields[0]};
    SlashRegisterComponent register_component;
    SlashRegisterSystem register_system;
    SlashSystemInfo system = {"orbit", "Moves Orbit entities round their circles.", 100, run, NULL};
    if (!load(get_proc, "slash_log", &slash_log) ||
        !load(get_proc, "slash_register_component", &register_component) ||
        !load(get_proc, "slash_register_system", &register_system) ||
        !load(get_proc, "slash_component_get", &component_get) ||
        !load(get_proc, "slash_component_set_json", &component_set_json) ||
        !load(get_proc, "slash_entity_valid", &entity_valid) ||
        !load(get_proc, "slash_world_each", &world_each) ||
        !load(get_proc, "slash_world_emit", &world_emit) ||
        !load(get_proc, "slash_input_just_pressed", &input_just_pressed)) {
        return 0;
    }
    orbit = register_component(plugin, &info);
    if (orbit == 0 || !register_system(plugin, &system)) {
        return 0;
    }
    init->deinitialize = deinitialize;
    return 1;
}
