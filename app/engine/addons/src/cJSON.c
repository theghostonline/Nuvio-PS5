/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#include "cJSON.h"

/* define our own boolean type */
#define true 1
#define false 0

typedef struct {
    const unsigned char *json;
    size_t position;
} parse_buffer;

static void *cJSON_malloc(size_t size) { return malloc(size); }
static void cJSON_free(void *object) { free(object); }

static cJSON_Hooks global_hooks = { cJSON_malloc, cJSON_free };

void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL) {
        global_hooks.malloc_fn = malloc;
        global_hooks.free_fn = free;
        return;
    }
    global_hooks.malloc_fn = (hooks->malloc_fn != NULL) ? hooks->malloc_fn : malloc;
    global_hooks.free_fn = (hooks->free_fn != NULL) ? hooks->free_fn : free;
}

static cJSON *cJSON_New_Item(void)
{
    cJSON* node = (cJSON*)global_hooks.malloc_fn(sizeof(cJSON));
    if (node) {
        memset(node, '\0', sizeof(cJSON));
    }
    return node;
}

void cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL) {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL)) {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL)) {
            global_hooks.free_fn(item->valuestring);
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL)) {
            global_hooks.free_fn(item->string);
        }
        global_hooks.free_fn(item);
        item = next;
    }
}

static unsigned char *cJSON_strdup(const unsigned char *string)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL) return NULL;
    length = strlen((const char*)string) + sizeof("");
    copy = (unsigned char*)global_hooks.malloc_fn(length);
    if (copy == NULL) return NULL;
    memcpy(copy, string, length);
    return copy;
}

static parse_buffer *buffer_skip_whitespace(parse_buffer *buffer)
{
    if ((buffer == NULL) || (buffer->json == NULL)) return NULL;
    while (buffer->json[buffer->position] && (buffer->json[buffer->position] <= 32)) {
        buffer->position++;
    }
    return buffer;
}

static cJSON_bool parse_value(cJSON * const item, parse_buffer * const buffer);

static cJSON_bool parse_string(cJSON * const item, parse_buffer * const buffer)
{
    const unsigned char *input = buffer->json + buffer->position;
    const unsigned char *input_end = input;
    size_t len = 0;
    unsigned char *output = NULL;

    if (*input != '\"') return false;
    input++;
    input_end = input;

    while (*input_end && (*input_end != '\"')) {
        if (*input_end == '\\') {
            input_end++;
            if (*input_end == '\0') return false;
        }
        input_end++;
    }
    if (*input_end != '\"') return false;

    len = (size_t)(input_end - input);
    output = (unsigned char*)global_hooks.malloc_fn(len + 1);
    if (output == NULL) return false;

    {
        size_t i = 0, j = 0;
        for (i = 0; i < len; i++) {
            if (input[i] == '\\' && i + 1 < len) {
                i++;
                switch (input[i]) {
                    case 'b': output[j++] = '\b'; break;
                    case 'f': output[j++] = '\f'; break;
                    case 'n': output[j++] = '\n'; break;
                    case 'r': output[j++] = '\r'; break;
                    case 't': output[j++] = '\t'; break;
                    case '\"': output[j++] = '\"'; break;
                    case '\\': output[j++] = '\\'; break;
                    case '/': output[j++] = '/'; break;
                    default: output[j++] = input[i]; break;
                }
            } else {
                output[j++] = input[i];
            }
        }
        output[j] = '\0';
    }

    item->valuestring = (char*)output;
    item->type = cJSON_String;
    buffer->position = (size_t)(input_end - buffer->json) + 1;
    return true;
}

static cJSON_bool parse_number(cJSON * const item, parse_buffer * const buffer)
{
    double number = 0;
    unsigned char *after_end = NULL;
    const unsigned char *input = buffer->json + buffer->position;

    number = strtod((const char*)input, (char**)&after_end);
    if (input == after_end) return false;

    item->valuedouble = number;
    if (number >= INT_MAX) item->valueint = INT_MAX;
    else if (number <= (double)INT_MIN) item->valueint = INT_MIN;
    else item->valueint = (int)number;

    item->type = cJSON_Number;
    buffer->position = (size_t)(after_end - buffer->json);
    return true;
}

static cJSON_bool parse_array(cJSON * const item, parse_buffer * const buffer)
{
    cJSON *head = NULL;
    cJSON *current_item = NULL;

    if (buffer->json[buffer->position] != '[') return false;
    buffer->position++;
    buffer_skip_whitespace(buffer);

    if (buffer->json[buffer->position] == ']') {
        item->type = cJSON_Array;
        buffer->position++;
        return true;
    }

    item->type = cJSON_Array;
    while (buffer->json[buffer->position]) {
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL) goto fail;

        if (head == NULL) {
            head = current_item = new_item;
        } else {
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        if (!parse_value(current_item, buffer)) goto fail;
        buffer_skip_whitespace(buffer);

        if (buffer->json[buffer->position] == ',') {
            buffer->position++;
            buffer_skip_whitespace(buffer);
        } else if (buffer->json[buffer->position] == ']') {
            buffer->position++;
            item->child = head;
            return true;
        } else {
            goto fail;
        }
    }

fail:
    if (head != NULL) cJSON_Delete(head);
    return false;
}

static cJSON_bool parse_object(cJSON * const item, parse_buffer * const buffer)
{
    cJSON *head = NULL;
    cJSON *current_item = NULL;

    if (buffer->json[buffer->position] != '{') return false;
    buffer->position++;
    buffer_skip_whitespace(buffer);

    if (buffer->json[buffer->position] == '}') {
        item->type = cJSON_Object;
        buffer->position++;
        return true;
    }

    item->type = cJSON_Object;
    while (buffer->json[buffer->position]) {
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL) goto fail;

        if (head == NULL) {
            head = current_item = new_item;
        } else {
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        if (!parse_string(current_item, buffer)) goto fail;
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        buffer_skip_whitespace(buffer);
        if (buffer->json[buffer->position] != ':') goto fail;
        buffer->position++;
        buffer_skip_whitespace(buffer);

        if (!parse_value(current_item, buffer)) goto fail;
        buffer_skip_whitespace(buffer);

        if (buffer->json[buffer->position] == ',') {
            buffer->position++;
            buffer_skip_whitespace(buffer);
        } else if (buffer->json[buffer->position] == '}') {
            buffer->position++;
            item->child = head;
            return true;
        } else {
            goto fail;
        }
    }

fail:
    if (head != NULL) cJSON_Delete(head);
    return false;
}

static cJSON_bool parse_value(cJSON * const item, parse_buffer * const buffer)
{
    buffer_skip_whitespace(buffer);
    if (buffer->json[buffer->position] == '\0') return false;

    if (strncmp((const char*)(buffer->json + buffer->position), "null", 4) == 0) {
        item->type = cJSON_NULL;
        buffer->position += 4;
        return true;
    }
    if (strncmp((const char*)(buffer->json + buffer->position), "true", 4) == 0) {
        item->type = cJSON_True;
        item->valueint = 1;
        buffer->position += 4;
        return true;
    }
    if (strncmp((const char*)(buffer->json + buffer->position), "false", 5) == 0) {
        item->type = cJSON_False;
        item->valueint = 0;
        buffer->position += 5;
        return true;
    }
    if (buffer->json[buffer->position] == '\"') {
        return parse_string(item, buffer);
    }
    if ((buffer->json[buffer->position] == '-') || (buffer->json[buffer->position] >= '0' && buffer->json[buffer->position] <= '9')) {
        return parse_number(item, buffer);
    }
    if (buffer->json[buffer->position] == '[') {
        return parse_array(item, buffer);
    }
    if (buffer->json[buffer->position] == '{') {
        return parse_object(item, buffer);
    }

    return false;
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    parse_buffer buffer = { 0, 0 };
    cJSON *item = NULL;

    if (value == NULL || buffer_length == 0) return NULL;
    buffer.json = (const unsigned char*)value;
    buffer.position = 0;

    item = cJSON_New_Item();
    if (item == NULL) return NULL;

    if (!parse_value(item, &buffer)) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_Parse(const char *value)
{
    return cJSON_ParseWithLength(value, value ? strlen(value) : 0);
}

int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;
    if (array == NULL) return 0;
    child = array->child;
    while (child != NULL) {
        size++;
        child = child->next;
    }
    return (int)size;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *current_child = NULL;
    if (array == NULL || index < 0) return NULL;
    current_child = array->child;
    while ((current_child != NULL) && (index > 0)) {
        index--;
        current_child = current_child->next;
    }
    return current_child;
}

static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL)) return 1;
    if (string1 == string2) return 0;
    for(; tolower(*string1) == tolower(*string2); (void)string1++, string2++) {
        if (*string1 == '\0') return 0;
    }
    return tolower(*string1) - tolower(*string2);
}

cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    cJSON *current_element = NULL;
    if ((object == NULL) || (string == NULL)) return NULL;
    current_element = object->child;
    while ((current_element != NULL) && (current_element->string != NULL) && (case_insensitive_strcmp((const unsigned char*)current_element->string, (const unsigned char*)string) != 0)) {
        current_element = current_element->next;
    }
    return current_element;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    cJSON *current_element = NULL;
    if ((object == NULL) || (string == NULL)) return NULL;
    current_element = object->child;
    while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(current_element->string, string) != 0)) {
        current_element = current_element->next;
    }
    return current_element;
}

cJSON_bool cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return (cJSON_GetObjectItem(object, string) != NULL);
}

cJSON_bool cJSON_IsInvalid(const cJSON * const item) { return (item == NULL) || (item->type == cJSON_Invalid); }
cJSON_bool cJSON_IsFalse(const cJSON * const item)   { return (item != NULL) && ((item->type & 0xFF) == cJSON_False); }
cJSON_bool cJSON_IsTrue(const cJSON * const item)    { return (item != NULL) && ((item->type & 0xFF) == cJSON_True); }
cJSON_bool cJSON_IsBool(const cJSON * const item)    { return (item != NULL) && (((item->type & 0xFF) == cJSON_True) || ((item->type & 0xFF) == cJSON_False)); }
cJSON_bool cJSON_IsNull(const cJSON * const item)    { return (item != NULL) && ((item->type & 0xFF) == cJSON_NULL); }
cJSON_bool cJSON_IsNumber(const cJSON * const item)  { return (item != NULL) && ((item->type & 0xFF) == cJSON_Number); }
cJSON_bool cJSON_IsString(const cJSON * const item)  { return (item != NULL) && ((item->type & 0xFF) == cJSON_String); }
cJSON_bool cJSON_IsArray(const cJSON * const item)   { return (item != NULL) && ((item->type & 0xFF) == cJSON_Array); }
cJSON_bool cJSON_IsObject(const cJSON * const item)  { return (item != NULL) && ((item->type & 0xFF) == cJSON_Object); }
cJSON_bool cJSON_IsRaw(const cJSON * const item)     { return (item != NULL) && ((item->type & 0xFF) == cJSON_Raw); }

const char *cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item)) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item)) return (double)NAN;
    return item->valuedouble;
}

cJSON *cJSON_CreateNull(void)   { cJSON *item = cJSON_New_Item(); if (item) item->type = cJSON_NULL; return item; }
cJSON *cJSON_CreateTrue(void)   { cJSON *item = cJSON_New_Item(); if (item) { item->type = cJSON_True; item->valueint = 1; } return item; }
cJSON *cJSON_CreateFalse(void)  { cJSON *item = cJSON_New_Item(); if (item) { item->type = cJSON_False; item->valueint = 0; } return item; }
cJSON *cJSON_CreateBool(cJSON_bool b) { return b ? cJSON_CreateTrue() : cJSON_CreateFalse(); }
cJSON *cJSON_CreateNumber(double num) { cJSON *item = cJSON_New_Item(); if (item) { item->type = cJSON_Number; item->valuedouble = num; item->valueint = (int)num; } return item; }
cJSON *cJSON_CreateString(const char *string) { cJSON *item = cJSON_New_Item(); if (item) { item->type = cJSON_String; item->valuestring = (char*)cJSON_strdup((const unsigned char*)string); if (!item->valuestring) { cJSON_Delete(item); return NULL; } } return item; }
cJSON *cJSON_CreateArray(void)  { cJSON *item = cJSON_New_Item(); if (item) item->type = cJSON_Array; return item; }
cJSON *cJSON_CreateObject(void) { cJSON *item = cJSON_New_Item(); if (item) item->type = cJSON_Object; return item; }

cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;
    if (item == NULL || array == NULL) return false;
    child = array->child;
    if (child == NULL) {
        array->child = item;
    } else {
        while (child->next) child = child->next;
        child->next = item;
        item->prev = child;
    }
    return true;
}

cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (item == NULL || object == NULL || string == NULL) return false;
    if (item->string) global_hooks.free_fn(item->string);
    item->string = (char*)cJSON_strdup((const unsigned char*)string);
    if (item->string == NULL) return false;
    return cJSON_AddItemToArray(object, item);
}

cJSON_bool cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *title = cJSON_CreateString(string);
    if (cJSON_AddItemToObject(object, name, title)) return true;
    cJSON_Delete(title);
    return false;
}

cJSON_bool cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *num = cJSON_CreateNumber(number);
    if (cJSON_AddItemToObject(object, name, num)) return true;
    cJSON_Delete(num);
    return false;
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    /* Minimal printer for client requests */
    char *out = (char*)global_hooks.malloc_fn(1024);
    if (!out) return NULL;
    if (cJSON_IsObject(item)) {
        strcpy(out, "{");
        cJSON *c = item->child;
        while (c) {
            char temp[256];
            if (cJSON_IsString(c)) {
                snprintf(temp, sizeof(temp), "\"%s\":\"%s\"", c->string, c->valuestring ? c->valuestring : "");
            } else if (cJSON_IsNumber(c)) {
                snprintf(temp, sizeof(temp), "\"%s\":%d", c->string, c->valueint);
            } else {
                snprintf(temp, sizeof(temp), "\"%s\":null", c->string);
            }
            strcat(out, temp);
            if (c->next) strcat(out, ",");
            c = c->next;
        }
        strcat(out, "}");
    } else {
        strcpy(out, "{}");
    }
    return out;
}

char *cJSON_Print(const cJSON *item)
{
    return cJSON_PrintUnformatted(item);
}
