#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    fputs("usage: fwmanifest get FILE KEY | model FILE MODEL | validate FILE | compare VERSION VERSION | escape TEXT\n", stream);
}

static int get_value(const char *path, const char *key) {
    struct json_object *root = json_object_from_file(path);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return 1;
    }
    char *copy = strdup(key);
    if (!copy) { json_object_put(root); return 1; }
    struct json_object *value = root;
    char *save = NULL;
    for (char *part = strtok_r(copy, ".", &save); part;
         part = strtok_r(NULL, ".", &save)) {
        struct json_object *next = NULL;
        if (!json_object_is_type(value, json_type_object) ||
            !json_object_object_get_ex(value, part, &next)) {
            free(copy); json_object_put(root); return 1;
        }
        value = next;
    }
    switch (json_object_get_type(value)) {
    case json_type_string: puts(json_object_get_string(value)); break;
    case json_type_int: printf("%lld\n", (long long)json_object_get_int64(value)); break;
    case json_type_boolean: puts(json_object_get_boolean(value) ? "true" : "false"); break;
    default: free(copy); json_object_put(root); return 1;
    }
    free(copy);
    json_object_put(root);
    return 0;
}

static int get_model(const char *path, const char *model) {
    struct json_object *root = json_object_from_file(path);
    struct json_object *models = NULL, *value = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "models", &models) ||
        !json_object_is_type(models, json_type_object) ||
        !json_object_object_get_ex(models, model, &value) ||
        !json_object_is_type(value, json_type_string)) {
        if (root) json_object_put(root);
        return 1;
    }
    puts(json_object_get_string(value));
    json_object_put(root);
    return 0;
}

static int validate(const char *path) {
    struct json_object *root = json_object_from_file(path);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return 1;
    }
    struct json_object *version = NULL;
    int ok = json_object_object_get_ex(root, "version", &version) &&
             json_object_is_type(version, json_type_string) &&
             json_object_get_string_len(version) > 0 &&
             json_object_get_string_len(version) <= 127;
    json_object_put(root);
    return ok ? 0 : 1;
}

static const char *skip_separators(const char *text) {
    while (*text && !isalnum((unsigned char)*text)) text++;
    return text;
}

static int compare_numeric(const char **left, const char **right) {
    const char *a = *left, *b = *right;
    while (*a == '0') a++;
    while (*b == '0') b++;
    const char *ae = a, *be = b;
    while (isdigit((unsigned char)*ae)) ae++;
    while (isdigit((unsigned char)*be)) be++;
    size_t alen = (size_t)(ae - a), blen = (size_t)(be - b);
    int result = 0;
    if (alen != blen) result = alen < blen ? -1 : 1;
    else if (alen) {
        int cmp = strncmp(a, b, alen);
        if (cmp) result = cmp < 0 ? -1 : 1;
    }
    while (isdigit((unsigned char)**left)) (*left)++;
    while (isdigit((unsigned char)**right)) (*right)++;
    return result;
}

static int compare_alpha(const char **left, const char **right) {
    const char *a = *left, *b = *right;
    while (isalpha((unsigned char)*a) && isalpha((unsigned char)*b)) {
        int ac = tolower((unsigned char)*a), bc = tolower((unsigned char)*b);
        if (ac != bc) return ac < bc ? -1 : 1;
        a++; b++;
    }
    int aalpha = isalpha((unsigned char)*a), balpha = isalpha((unsigned char)*b);
    *left = a; *right = b;
    if (aalpha != balpha) return aalpha ? 1 : -1;
    return 0;
}

static int version_compare(const char *left, const char *right) {
    const char *a = left, *b = right;
    for (;;) {
        a = skip_separators(a);
        b = skip_separators(b);
        if (!*a || !*b) break;
        int result;
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
            result = compare_numeric(&a, &b);
        else if (isalpha((unsigned char)*a) && isalpha((unsigned char)*b))
            result = compare_alpha(&a, &b);
        else
            return isdigit((unsigned char)*a) ? 1 : -1;
        if (result) return result;
    }
    a = skip_separators(a);
    b = skip_separators(b);
    if (!*a && !*b) return 0;
    return *a ? 1 : -1;
}

static int escape_string(const char *text) {
    struct json_object *value = json_object_new_string(text ? text : "");
    if (!value) return 1;
    const char *encoded = json_object_to_json_string_ext(value,
                                                         JSON_C_TO_STRING_PLAIN);
    size_t length = encoded ? strlen(encoded) : 0;
    if (length < 2 || encoded[0] != '"' || encoded[length - 1] != '"') {
        json_object_put(value);
        return 1;
    }
    if (length > 2 && fwrite(encoded + 1, 1, length - 2, stdout) != length - 2) {
        json_object_put(value);
        return 1;
    }
    putchar('\n');
    json_object_put(value);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "get")) return get_value(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "model")) return get_model(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "validate")) return validate(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "escape")) return escape_string(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "compare")) {
        printf("%d\n", version_compare(argv[2], argv[3]));
        return 0;
    }
    usage(stderr);
    return 2;
}
