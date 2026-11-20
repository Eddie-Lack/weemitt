/* A small JSON reader for the config files. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum JsonType { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct Json {
    JsonType type;
    double number;      /* JSON_NUMBER; JSON_BOOL: 0 or 1 */
    char *string;       /* JSON_STRING */
    struct Json *items; /* JSON_ARRAY and JSON_OBJECT: count values */
    char **keys;        /* JSON_OBJECT: count keys */
    int count;
} Json;

/* Parses text. Returns NULL with a message in err (with the line) when it is not JSON. */
Json *json_parse(const char *text, char *err, size_t errSize);
void json_free(Json *j);

/* The value of a key of an object, or NULL. */
const Json *json_get(const Json *object, const char *key);
/* A key's value as a number or a string, or the default when the key is missing or of another type. */
double json_number(const Json *object, const char *key, double def);
const char *json_string(const Json *object, const char *key, const char *def);

#endif
