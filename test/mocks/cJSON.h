#pragma once
// Mock cJSON.h for host testing
// Real implementations are provided by test files that need them
typedef struct cJSON {
    struct cJSON *next;
    char *string;
    char *valuestring;
} cJSON;

cJSON* cJSON_CreateObject(void);
void cJSON_AddStringToObject(cJSON *obj, const char *name, const char *value);
char* cJSON_PrintUnformatted(cJSON *obj);
void cJSON_Delete(cJSON *obj);
