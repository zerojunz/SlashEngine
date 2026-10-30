/*
 * SlashEngine's plugin interface, as Godot's gdextension_interface.h: a plugin is a library built
 * apart from the engine, by any compiler and in any language that calls C, that adds components
 * and systems to the games that list it. It links nothing of the engine's: the engine calls its
 * entry function with get_proc, which gives each of the functions below by name, so a plugin
 * built against one version of this header loads into later engines too. Functions are only ever
 * added; a function whose meaning changes gets a new name.
 *
 *   static SlashRegisterComponent register_component;
 *
 *   SLASH_PLUGIN_EXPORT int my_plugin_entry(SlashGetProc get_proc, SlashPlugin* plugin,
 *                                           SlashPluginInit* init) {
 *       register_component = (SlashRegisterComponent)get_proc("slash_register_component");
 *       ...
 *       return 1;
 *   }
 *
 * The entry function's name is the plugin file's "entry" (slash-cli schema plugin). It returns 1
 * once it registered what it adds, or 0 to refuse to load. Components are C structs the engine
 * stores, saves and loads by their fields; systems are functions the world calls each tick.
 */
#ifndef SLASH_PLUGIN_H
#define SLASH_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This file is C, which clang-tidy's C++ checks would turn into C++. NOLINTBEGIN */

/* The interface's version. An engine loads plugins whose file asks for this version or older. */
#define SLASH_PLUGIN_ABI_VERSION 1

#if defined(_WIN32)
#define SLASH_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SLASH_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* A world: a scene's entities and their components, as the engine runs it. */
typedef struct SlashWorld SlashWorld;
/* The plugin as it registers, given to its entry function. */
typedef struct SlashPlugin SlashPlugin;

/* An entity of a world, valid until it is destroyed. */
typedef uint32_t SlashEntity;
#define SLASH_NO_ENTITY 0xFFFFFFFFu

/* A plugin's component type, as slash_register_component returns it; 0 is none. */
typedef uint32_t SlashComponent;

typedef void (*SlashProc)(void);
/* One of the functions below by its name, or NULL if this engine has none of that name. */
typedef SlashProc (*SlashGetProc)(const char* name);

/* What the entry function says back. */
typedef struct SlashPluginInit {
    /* The engine's SLASH_PLUGIN_ABI_VERSION, set before the entry function runs. */
    uint32_t abi_version;
    /* Passed to deinitialize. */
    void* userdata;
    /* Called once when the engine lets go of the plugin, or NULL. */
    void (*deinitialize)(void* userdata);
} SlashPluginInit;

typedef int (*SlashPluginEntry)(SlashGetProc get_proc, SlashPlugin* plugin, SlashPluginInit* init);

/* The types a component's fields have in its struct, and in scene files. */
typedef enum SlashFieldType {
    SLASH_FIELD_BOOL = 1,   /* uint8_t, 0 or 1; true or false */
    SLASH_FIELD_INT32 = 2,  /* int32_t */
    SLASH_FIELD_INT64 = 3,  /* int64_t */
    SLASH_FIELD_FLOAT = 4,  /* float */
    SLASH_FIELD_DOUBLE = 5, /* double */
    SLASH_FIELD_VEC2 = 6,   /* float[2]; [x, y] */
    SLASH_FIELD_VEC3 = 7,   /* float[3]; [x, y, z] */
    SLASH_FIELD_VEC4 = 8,   /* float[4]; [x, y, z, w], or a color's [r, g, b, a] */
    SLASH_FIELD_STRING = 9, /* char[size], UTF-8 ending in 0; a string of at most size - 1 bytes */
    SLASH_FIELD_ENTITY = 10 /* SlashEntity; the entity's ID, or null */
} SlashFieldType;

typedef struct SlashField {
    const char* name; /* snake_case, as engine components' fields */
    uint32_t type;    /* a SlashFieldType */
    uint32_t offset;  /* offsetof the field in the struct */
    uint32_t size;    /* a string's capacity in bytes; for other types 0, or their size */
    const char* doc;  /* what it is, or NULL */
    uint32_t has_min; /* a number's or each of a vector's numbers' range, when set */
    uint32_t has_max;
    double min;
    double max;
} SlashField;

typedef struct SlashComponentInfo {
    const char* name; /* PascalCase, unique among every component: prefix it with the plugin's */
    const char* doc;  /* one sentence, or NULL */
    uint32_t size;    /* sizeof the struct, at most 4096 */
    uint32_t align;   /* alignof the struct, at most 16 */
    const void* defaults; /* a struct of default values, or NULL for zeros */
    const SlashField* fields;
    uint32_t field_count;
} SlashComponentInfo;

typedef struct SlashSystemInfo {
    const char* name; /* unique among every system */
    const char* doc;  /* one sentence, or NULL */
    int32_t priority; /* lower runs first, as Godot's process_priority; the engine's are listed by
                         engine.describe */
    void (*run)(void* userdata, SlashWorld* world, double tick_seconds);
    void* userdata;
} SlashSystemInfo;

/* Log levels. */
#define SLASH_LOG_DEBUG 0
#define SLASH_LOG_INFO 1
#define SLASH_LOG_WARN 2
#define SLASH_LOG_ERROR 3

/*
 * The functions get_proc gives, by the name in each comment. Text out goes into the caller's
 * buffer as snprintf writes it: the result is the text's length, and the buffer holds as much of
 * it as fits with a terminating 0.
 */

/* "slash_engine_version": the engine's version, such as "0.9.0". */
typedef const char* (*SlashEngineVersion)(void);
/* "slash_log": a line in the engine's log, with fields as a JSON object or NULL. */
typedef void (*SlashLog)(int level, const char* message, const char* fields_json);

/* "slash_register_component": adds a component type; 0 if the info is wrong (logged). */
typedef SlashComponent (*SlashRegisterComponent)(SlashPlugin* plugin,
                                                 const SlashComponentInfo* info);
/* "slash_register_system": adds a system to every world; 0 if the info is wrong (logged). */
typedef int (*SlashRegisterSystem)(SlashPlugin* plugin, const SlashSystemInfo* info);

/* "slash_component_find": a plugin component type by name, any plugin's; 0 if none has it. */
typedef SlashComponent (*SlashComponentFind)(SlashWorld* world, const char* name);
/* "slash_component_get": an entity's component, or NULL if it has none. */
typedef void* (*SlashComponentGet)(SlashWorld* world, SlashEntity entity, SlashComponent component);
/* "slash_component_add": the entity's component, added at its defaults if it had none; NULL if the
 * entity or type doesn't exist. */
typedef void* (*SlashComponentAdd)(SlashWorld* world, SlashEntity entity, SlashComponent component);
/* "slash_component_remove": removes it; 1 if the entity had it. */
typedef int (*SlashComponentRemove)(SlashWorld* world, SlashEntity entity,
                                    SlashComponent component);
/* "slash_component_get_json": any component by name, the engine's too, as a JSON object of its
 * fields; -1 if the entity has none. */
typedef int64_t (*SlashComponentGetJson)(SlashWorld* world, SlashEntity entity, const char* name,
                                         char* buffer, size_t capacity);
/* "slash_component_set_json": sets the fields a JSON object names on any component, adding it
 * if the entity has none; 1 if done, else 0 with the reason in error. */
typedef int (*SlashComponentSetJson)(SlashWorld* world, SlashEntity entity, const char* name,
                                     const char* json, char* error, size_t error_capacity);

/* "slash_entity_create": a new entity with the ID, or a generated one for NULL; SLASH_NO_ENTITY
 * if the ID is taken or not valid. */
typedef SlashEntity (*SlashEntityCreate)(SlashWorld* world, const char* id);
/* "slash_entity_destroy": destroys it and its children. */
typedef void (*SlashEntityDestroy)(SlashWorld* world, SlashEntity entity);
/* "slash_entity_find": the entity with an ID, or SLASH_NO_ENTITY. */
typedef SlashEntity (*SlashEntityFind)(SlashWorld* world, const char* id);
/* "slash_entity_valid": 1 while the entity exists. */
typedef int (*SlashEntityValid)(SlashWorld* world, SlashEntity entity);
/* "slash_entity_id": the entity's ID, or -1 if it doesn't exist. */
typedef int64_t (*SlashEntityId)(SlashWorld* world, SlashEntity entity, char* buffer,
                                 size_t capacity);

/* "slash_world_each": calls visit with each entity that has every one of the components and
 * with them, in the order given. Don't add or remove those components, or create or destroy
 * entities, while it visits; do it after. */
typedef void (*SlashWorldEach)(SlashWorld* world, const SlashComponent* components, uint32_t count,
                               void (*visit)(void* userdata, SlashEntity entity, void** components),
                               void* userdata);
/* "slash_world_emit": a world event for systems, gameplay code and agents, about an entity or
 * SLASH_NO_ENTITY, with data as JSON or NULL. */
typedef void (*SlashWorldEmit)(SlashWorld* world, const char* kind, SlashEntity about,
                               const char* data_json);
/* "slash_world_events": calls visit with each event of the tick before, as systems see them. */
typedef void (*SlashWorldEvents)(SlashWorld* world,
                                 void (*visit)(void* userdata, const char* kind, SlashEntity about,
                                               const char* data_json),
                                 void* userdata);
/* "slash_world_tick": the ticks the world has stepped. */
typedef uint64_t (*SlashWorldTick)(SlashWorld* world);

/* "slash_input_pressed": 1 while an input action is held. */
typedef int (*SlashInputPressed)(SlashWorld* world, const char* action);
/* "slash_input_just_pressed": 1 in the tick an action was pressed. */
typedef int (*SlashInputJustPressed)(SlashWorld* world, const char* action);
/* "slash_input_strength": how far an action is held, 0 to 1. */
typedef float (*SlashInputStrength)(SlashWorld* world, const char* action);

/* NOLINTEND */

#ifdef __cplusplus
}
#endif

#endif /* SLASH_PLUGIN_H */
