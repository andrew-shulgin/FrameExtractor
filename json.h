#ifndef FRAME_EXTRACTOR_JSON_H
#define FRAME_EXTRACTOR_JSON_H

#include "jsmn.h"

struct {
    char *buffer;
    jsmntok_t *tokens;
    unsigned int token_count;
} static json;

int json_parse(const char *json_filename);
char *json_buffer();
jsmntok_t json_token(int index);
unsigned int json_token_count();
char *json_err2str(int err);
void json_free();

#endif //FRAME_EXTRACTOR_JSON_H
