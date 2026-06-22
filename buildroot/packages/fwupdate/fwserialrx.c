#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROTOCOL "PMOSUART/1"
#define LINE_MAX_BYTES 8192
#define NAME_MAX_BYTES 127
#define HASH_HEX_BYTES 64
#define DEFAULT_IDLE_TIMEOUT 120
#define DEFAULT_MAX_FIRMWARE (16U * 1024U * 1024U)
#define DEFAULT_MAX_MANIFEST (1024U * 1024U)

struct object_state {
    const char *kind;
    const char *destination;
    char part_path[512];
    char name[NAME_MAX_BYTES + 1];
    char expected_hash[HASH_HEX_BYTES + 1];
    uint64_t expected_size;
    uint64_t received;
    uint32_t next_sequence;
    uint32_t last_sequence;
    uint32_t last_crc;
    int fd;
    bool started;
    bool complete;
};

struct options {
    const char *firmware_path;
    const char *manifest_path;
    const char *metadata_path;
    unsigned int idle_timeout;
    uint64_t max_firmware;
    uint64_t max_manifest;
};

static struct object_state *active_object;
static struct object_state firmware_object;
static struct object_state manifest_object;

static void emit_error(const char *code, const char *message) {
    printf(PROTOCOL " ERROR %s %s\n", code ? code : "protocol",
           message ? message : "transfer failed");
    fflush(stdout);
}

static void cleanup_object(struct object_state *object) {
    if (!object) return;
    if (object->fd >= 0) {
        close(object->fd);
        object->fd = -1;
    }
    if (object->part_path[0]) unlink(object->part_path);
}

static void cleanup_all(void) {
    cleanup_object(&firmware_object);
    cleanup_object(&manifest_object);
}

static bool valid_hash(const char *value) {
    if (!value || strlen(value) != HASH_HEX_BYTES) return false;
    for (size_t i = 0; i < HASH_HEX_BYTES; ++i)
        if (!isxdigit((unsigned char)value[i])) return false;
    return true;
}

static void lowercase_hash(char output[HASH_HEX_BYTES + 1], const char *input) {
    for (size_t i = 0; i < HASH_HEX_BYTES; ++i)
        output[i] = (char)tolower((unsigned char)input[i]);
    output[HASH_HEX_BYTES] = '\0';
}

static bool valid_name(const char *value) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length > NAME_MAX_BYTES || !strcmp(value, ".") ||
        !strcmp(value, "..")) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool parse_u64(const char *value, uint64_t *output) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value ? value : "", &end, 10);
    if (errno || !end || *end || parsed == 0) return false;
    *output = (uint64_t)parsed;
    return true;
}

static bool parse_u32(const char *value, uint32_t *output) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value ? value : "", &end, 10);
    if (errno || !end || *end || parsed > UINT32_MAX) return false;
    *output = (uint32_t)parsed;
    return true;
}

static bool parse_hex_u32(const char *value, uint32_t *output) {
    char *end = NULL;
    if (!value || strlen(value) != 8) return false;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 16);
    if (errno || !end || *end || parsed > UINT32_MAX) return false;
    *output = (uint32_t)parsed;
    return true;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t length) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                               (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ UINT32_C(0xffffffff);
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int decode_base64(const char *input, unsigned char *output,
                         size_t output_capacity, size_t *output_length) {
    size_t length = strlen(input);
    if (!length || (length % 4) != 0) return -1;
    size_t produced = 0;
    for (size_t offset = 0; offset < length; offset += 4) {
        int values[4];
        unsigned int padding = 0;
        for (unsigned int i = 0; i < 4; ++i) {
            unsigned char c = (unsigned char)input[offset + i];
            if (c == '=') {
                if (i < 2 || offset + 4 != length) return -1;
                values[i] = 0;
                ++padding;
            } else {
                if (padding) return -1;
                values[i] = base64_value(c);
                if (values[i] < 0) return -1;
            }
        }
        if (padding > 2) return -1;
        if (padding == 1 && input[offset + 3] != '=') return -1;
        if (padding == 2 &&
            !(input[offset + 2] == '=' && input[offset + 3] == '=')) return -1;
        uint32_t word = ((uint32_t)values[0] << 18) |
                        ((uint32_t)values[1] << 12) |
                        ((uint32_t)values[2] << 6) |
                        (uint32_t)values[3];
        size_t bytes = 3 - padding;
        if (produced + bytes > output_capacity) return -1;
        output[produced++] = (unsigned char)(word >> 16);
        if (bytes > 1) output[produced++] = (unsigned char)(word >> 8);
        if (bytes > 2) output[produced++] = (unsigned char)word;
    }
    *output_length = produced;
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t length) {
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        data += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int sha256_file(const char *path, char output[HASH_HEX_BYTES + 1]) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return -1;
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]); close(descriptors[1]); return -1;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[1]);
        execlp("sha256sum", "sha256sum", path, (char *)NULL);
        _exit(127);
    }
    close(descriptors[1]);
    size_t received = 0;
    while (received < HASH_HEX_BYTES) {
        ssize_t count = read(descriptors[0], output + received,
                             HASH_HEX_BYTES - received);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(descriptors[0]); waitpid(child, NULL, 0); return -1;
        }
        if (count == 0) break;
        received += (size_t)count;
    }
    close(descriptors[0]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || received != HASH_HEX_BYTES) return -1;
    output[HASH_HEX_BYTES] = '\0';
    if (!valid_hash(output)) return -1;
    for (size_t i = 0; i < HASH_HEX_BYTES; ++i)
        output[i] = (char)tolower((unsigned char)output[i]);
    return 0;
}

static int metadata_write(const struct options *options) {
    char temporary[512];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", options->metadata_path) >=
        (int)sizeof(temporary)) return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    char buffer[1024];
    int length = snprintf(buffer, sizeof(buffer),
        "protocol=1\nfirmware_name=%s\nfirmware_bytes=%llu\n"
        "firmware_sha256=%s\nmanifest_received=%d\n"
        "manifest_name=%s\nmanifest_bytes=%llu\nmanifest_sha256=%s\n",
        firmware_object.name,
        (unsigned long long)firmware_object.expected_size,
        firmware_object.expected_hash,
        manifest_object.complete ? 1 : 0,
        manifest_object.complete ? manifest_object.name : "",
        (unsigned long long)(manifest_object.complete ? manifest_object.expected_size : 0),
        manifest_object.complete ? manifest_object.expected_hash : "");
    if (length < 0 || (size_t)length >= sizeof(buffer) ||
        write_all(fd, (const unsigned char *)buffer, (size_t)length) != 0 ||
        fsync(fd) != 0 || close(fd) != 0 || rename(temporary, options->metadata_path) != 0) {
        close(fd); unlink(temporary); return -1;
    }
    return 0;
}

static struct object_state *object_for_kind(const char *kind) {
    if (!strcmp(kind, "firmware")) return &firmware_object;
    if (!strcmp(kind, "manifest")) return &manifest_object;
    return NULL;
}

static int object_begin(struct object_state *object, const char *size_text,
                        const char *hash, const char *name,
                        uint64_t maximum_size) {
    uint64_t size = 0;
    if (!object || object->started || object->complete || active_object ||
        !parse_u64(size_text, &size) || size > maximum_size ||
        !valid_hash(hash) || !valid_name(name)) return -1;
    if (snprintf(object->part_path, sizeof(object->part_path), "%s.part",
                 object->destination) >= (int)sizeof(object->part_path)) return -1;
    unlink(object->part_path);
    object->fd = open(object->part_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (object->fd < 0) return -1;
    object->expected_size = size;
    object->received = 0;
    object->next_sequence = 0;
    object->last_sequence = UINT32_MAX;
    object->last_crc = 0;
    lowercase_hash(object->expected_hash, hash);
    snprintf(object->name, sizeof(object->name), "%s", name);
    object->started = true;
    active_object = object;
    return 0;
}

static int object_data(struct object_state *object, const char *sequence_text,
                       const char *crc_text, const char *payload) {
    uint32_t sequence = 0, declared_crc = 0;
    if (!object || object != active_object || object->fd < 0 ||
        !parse_u32(sequence_text, &sequence) ||
        !parse_hex_u32(crc_text, &declared_crc)) return -1;
    if (sequence + 1 == object->next_sequence &&
        object->last_sequence == sequence && object->last_crc == declared_crc) {
        printf(PROTOCOL " ACK %s %u\n", object->kind, sequence);
        fflush(stdout);
        return 1;
    }
    if (sequence != object->next_sequence) return -1;
    size_t encoded_length = strlen(payload);
    if (!encoded_length || encoded_length > 4096) return -1;
    unsigned char decoded[3072];
    size_t decoded_length = 0;
    if (decode_base64(payload, decoded, sizeof(decoded), &decoded_length) != 0 ||
        decoded_length == 0 || object->received + decoded_length > object->expected_size ||
        crc32_bytes(decoded, decoded_length) != declared_crc ||
        write_all(object->fd, decoded, decoded_length) != 0) return -1;
    object->received += decoded_length;
    object->last_sequence = sequence;
    object->last_crc = declared_crc;
    ++object->next_sequence;
    printf(PROTOCOL " ACK %s %u\n", object->kind, sequence);
    fflush(stdout);
    return 0;
}

static int object_end(struct object_state *object, const char *frames_text) {
    uint32_t frames = 0;
    if (!object || object != active_object || object->fd < 0 ||
        !parse_u32(frames_text, &frames) || frames != object->next_sequence ||
        object->received != object->expected_size || fsync(object->fd) != 0 ||
        close(object->fd) != 0) return -1;
    object->fd = -1;
    char actual_hash[HASH_HEX_BYTES + 1];
    if (sha256_file(object->part_path, actual_hash) != 0 ||
        strcmp(actual_hash, object->expected_hash) ||
        rename(object->part_path, object->destination) != 0) return -1;
    object->part_path[0] = '\0';
    object->complete = true;
    active_object = NULL;
    printf(PROTOCOL " OBJECT-OK %s %llu %s\n", object->kind,
           (unsigned long long)object->expected_size, object->expected_hash);
    fflush(stdout);
    return 0;
}

static void usage(FILE *stream) {
    fprintf(stream,
        "usage: fwserialrx --firmware PATH --manifest PATH --metadata PATH [options]\n"
        "\n"
        "Receive a PMOSUART/1 framed Base64 firmware transfer on stdin/stdout.\n"
        "The manifest object is optional, but --manifest reserves its destination.\n"
        "\n"
        "Options:\n"
        "  --idle-timeout SECONDS   per-line timeout (default 120)\n"
        "  --max-firmware BYTES     default 16777216\n"
        "  --max-manifest BYTES     default 1048576\n");
}

static int parse_options(int argc, char **argv, struct options *options) {
    memset(options, 0, sizeof(*options));
    options->idle_timeout = DEFAULT_IDLE_TIMEOUT;
    options->max_firmware = DEFAULT_MAX_FIRMWARE;
    options->max_manifest = DEFAULT_MAX_MANIFEST;
    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) {
            usage(stdout); exit(0);
        }
        if (i + 1 >= argc) return -1;
        const char *value = argv[++i];
        uint64_t parsed = 0;
        if (!strcmp(argument, "--firmware")) options->firmware_path = value;
        else if (!strcmp(argument, "--manifest")) options->manifest_path = value;
        else if (!strcmp(argument, "--metadata")) options->metadata_path = value;
        else if (!strcmp(argument, "--idle-timeout")) {
            if (!parse_u64(value, &parsed) || parsed > 3600) return -1;
            options->idle_timeout = (unsigned int)parsed;
        } else if (!strcmp(argument, "--max-firmware")) {
            if (!parse_u64(value, &parsed)) return -1;
            options->max_firmware = parsed;
        } else if (!strcmp(argument, "--max-manifest")) {
            if (!parse_u64(value, &parsed)) return -1;
            options->max_manifest = parsed;
        } else return -1;
    }
    return options->firmware_path && options->manifest_path && options->metadata_path ? 0 : -1;
}

int main(int argc, char **argv) {
    struct options options;
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr); return 2;
    }
    umask(077);
    firmware_object = (struct object_state){
        .kind = "firmware", .destination = options.firmware_path, .fd = -1
    };
    manifest_object = (struct object_state){
        .kind = "manifest", .destination = options.manifest_path, .fd = -1
    };
    unlink(options.firmware_path);
    unlink(options.manifest_path);
    unlink(options.metadata_path);

    printf(PROTOCOL " READY max-firmware=%llu max-manifest=%llu\n",
           (unsigned long long)options.max_firmware,
           (unsigned long long)options.max_manifest);
    fflush(stdout);

    char line[LINE_MAX_BYTES];
    int result = 1;
    while (true) {
        struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
        int ready;
        do ready = poll(&descriptor, 1, (int)options.idle_timeout * 1000);
        while (ready < 0 && errno == EINTR);
        if (ready == 0) { emit_error("timeout", "idle timeout expired"); break; }
        if (ready < 0 || !(descriptor.revents & (POLLIN | POLLHUP))) {
            emit_error("io", "serial input failed"); break;
        }
        if (!fgets(line, sizeof(line), stdin)) {
            emit_error("eof", "serial input ended"); break;
        }
        size_t length = strlen(line);
        if (!length || (line[length - 1] != '\n' && !feof(stdin))) {
            int c; while ((c = fgetc(stdin)) != '\n' && c != EOF) {}
            emit_error("line", "frame exceeds line limit"); break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *save = NULL;
        char *protocol = strtok_r(line, " ", &save);
        char *command = strtok_r(NULL, " ", &save);
        if (!protocol || strcmp(protocol, PROTOCOL) || !command) {
            emit_error("protocol", "invalid protocol prefix"); break;
        }
        if (!strcmp(command, "ABORT")) {
            emit_error("aborted", "sender aborted transfer"); break;
        }
        if (!strcmp(command, "BEGIN")) {
            char *kind = strtok_r(NULL, " ", &save);
            char *size = strtok_r(NULL, " ", &save);
            char *hash = strtok_r(NULL, " ", &save);
            char *name = strtok_r(NULL, " ", &save);
            struct object_state *object = kind ? object_for_kind(kind) : NULL;
            uint64_t maximum = object == &firmware_object ? options.max_firmware : options.max_manifest;
            if (!object || !size || !hash || !name || strtok_r(NULL, " ", &save) ||
                object_begin(object, size, hash, name, maximum) != 0) {
                emit_error("begin", "invalid object declaration"); break;
            }
            printf(PROTOCOL " BEGIN-ACK %s\n", object->kind); fflush(stdout);
            continue;
        }
        if (!strcmp(command, "DATA")) {
            char *kind = strtok_r(NULL, " ", &save);
            char *sequence = strtok_r(NULL, " ", &save);
            char *crc = strtok_r(NULL, " ", &save);
            char *payload = strtok_r(NULL, " ", &save);
            struct object_state *object = kind ? object_for_kind(kind) : NULL;
            int status = (!object || !sequence || !crc || !payload ||
                          strtok_r(NULL, " ", &save)) ? -1 :
                         object_data(object, sequence, crc, payload);
            if (status < 0) { emit_error("data", "invalid data frame"); break; }
            continue;
        }
        if (!strcmp(command, "END")) {
            char *kind = strtok_r(NULL, " ", &save);
            char *frames = strtok_r(NULL, " ", &save);
            struct object_state *object = kind ? object_for_kind(kind) : NULL;
            if (!object || !frames || strtok_r(NULL, " ", &save) ||
                object_end(object, frames) != 0) {
                emit_error("end", "object length, checksum, or sequence mismatch"); break;
            }
            continue;
        }
        if (!strcmp(command, "DONE")) {
            if (strtok_r(NULL, " ", &save) || active_object || !firmware_object.complete ||
                metadata_write(&options) != 0) {
                emit_error("done", "firmware is incomplete or metadata could not be written");
                break;
            }
            printf(PROTOCOL " COMPLETE firmware=%s manifest=%s\n",
                   firmware_object.name,
                   manifest_object.complete ? manifest_object.name : "none");
            fflush(stdout);
            result = 0;
            break;
        }
        emit_error("command", "unknown transfer command");
        break;
    }
    if (result != 0) {
        unlink(options.firmware_path);
        unlink(options.manifest_path);
        unlink(options.metadata_path);
    }
    cleanup_all();
    return result;
}
