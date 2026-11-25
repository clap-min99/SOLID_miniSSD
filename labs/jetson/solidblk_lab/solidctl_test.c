#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { REQUEST_SIZE = 16, REPLY_HEADER_SIZE = 8, BLOCK_SIZE = 2048 };

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    const char *device = argc == 2 ? argv[1] : "/dev/solidctl_lab";
    uint8_t request[REQUEST_SIZE];
    uint8_t reply[REPLY_HEADER_SIZE + BLOCK_SIZE] = {0};
    int result = 1;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [control-device]\n", argv[0]);
        return 2;
    }

    int fd = open(device, O_RDWR);

    if (fd < 0) {
        perror(device);
        return 1;
    }

    ssize_t received = read(fd, request, sizeof(request));
    if (received < 0) {
        perror("read request");
        goto done;
    }
    if (received != REQUEST_SIZE) {
        fputs("FAIL: expected one 16-byte request\n", stderr);
        goto done;
    }

    uint32_t id = read_le32(request);
    uint32_t op = read_le32(request + 4);
    uint32_t lba = read_le32(request + 8);
    uint32_t length = read_le32(request + 12);
    printf("Request: ID=%u op=%u LBA=%u bytes=%u\n",
           (unsigned)id, (unsigned)op, (unsigned)lba, (unsigned)length);
    if ((argc == 1 && id != 42) || op != 2 || lba != 2 || length != BLOCK_SIZE) {
        fputs("FAIL: unexpected request\n", stderr);
        goto done;
    }

    memcpy(reply, request, 4); /* Same request ID; status stays zero. */
    for (size_t i = 0; i < BLOCK_SIZE; ++i)
        reply[REPLY_HEADER_SIZE + i] = (uint8_t)(i ^ (i >> 8));

    ssize_t sent = write(fd, reply, sizeof(reply));
    if (sent < 0) {
        perror("write reply");
        goto done;
    }
    if (sent != (ssize_t)sizeof(reply)) {
        fputs("FAIL: incomplete reply\n", stderr);
        goto done;
    }

    puts("PASS: kernel accepted the 2048-byte reply");
    result = 0;

done:
    close(fd);
    return result;
}
