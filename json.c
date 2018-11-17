#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "json.h"

static char *read_file(char const *path) {
    char *buffer = NULL;
    size_t length = 0;
    FILE *f = fopen(path, "rb");

    if (f) {
        fseek(f, 0, SEEK_END);
        length = (size_t) ftell(f);
        fseek(f, 0, SEEK_SET);
        buffer = (char *) malloc((length + 1) * sizeof(char));
        if (buffer)
            fread(buffer, sizeof(char), length, f);
        fclose(f);
    }
    if (buffer != NULL)
        buffer[length] = '\0';
    return buffer;
}

int json_parse(const char *json_filename) {
    json.buffer = read_file(json_filename);
    if (json.buffer == NULL)
        return -4;
    jsmn_parser parser;
    jsmn_init(&parser);
    json.token_count = (unsigned int) jsmn_parse(&parser, json.buffer, strlen(json.buffer), NULL, 0);
    json.tokens = malloc((size_t) json.token_count * sizeof(jsmntok_t));
    jsmn_init(&parser);
    int ret = jsmn_parse(&parser, json.buffer, strlen(json.buffer), json.tokens, json.token_count);
    if (ret < 0) {
        free(json.buffer);
        free(json.tokens);
        json.buffer = NULL;
        json.tokens = NULL;
    }
    json.token_count = (unsigned int) (ret > 0 ? ret : 0);
    return ret;
}

char *json_buffer() {
    return json.buffer;
}

jsmntok_t json_token(int index) {
    return json.tokens[index];
}

unsigned int json_token_count() {
    return json.token_count;
}

char *json_err2str(int err) {
    switch (err) {
        case JSMN_ERROR_NOMEM:
            return "Not enough tokens were provided";
        case JSMN_ERROR_INVAL:
            return "Invalid character inside JSON string";
        case JSMN_ERROR_PART:
            return "The string is not a full JSON packet, more bytes expected";
        case -4:
            return "Unable to read the file";
        default:
            return "Unknown error";
    }
}

void json_free() {
    if (json.buffer != NULL)
        free(json.buffer);
    if (json.tokens != NULL)
        free(json.tokens);
}
