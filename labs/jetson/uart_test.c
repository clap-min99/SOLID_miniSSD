#define _DEFAULT_SOURCE

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* Diagnostic firmware attaches a fixed test block to successful READs. */
_Static_assert(SSD_CMD_HEADER_SIZE == 13, "expected a 13-byte command header");
_Static_assert(SSD_CPL_HEADER_SIZE == 4, "expected a 4-byte completion header");
_Static_assert(SSD_LBA_SIZE == 2048, "expected a 2048-byte test block");

static void encode_command(uint8_t wire[SSD_CMD_HEADER_SIZE],
                           const SSD_Command *cmd)
{
    wire[0] = cmd->op;
    wire[1] = (uint8_t)cmd->cid;
    wire[2] = (uint8_t)(cmd->cid >> 8);

    for (unsigned i = 0; i < 8; ++i)
        wire[3 + i] = (uint8_t)(cmd->slba >> (8U * i));

    wire[11] = (uint8_t)cmd->nlb;
    wire[12] = (uint8_t)(cmd->nlb >> 8);
}

static uint16_t read_le16(const uint8_t *wire)
{
    return (uint16_t)((uint16_t)wire[0] | ((uint16_t)wire[1] << 8));
}

static int configure_uart(int fd, cc_t timeout)
{
    struct termios config;

    if (tcgetattr(fd, &config) < 0)
        return -1;

    cfmakeraw(&config);
    config.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    config.c_cflag |= CS8 | CLOCAL | CREAD;
    config.c_iflag &= ~(IXON | IXOFF | IXANY);
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = timeout;

    if (cfsetispeed(&config, B115200) < 0 || cfsetospeed(&config, B115200) < 0)
        return -1;

    return tcsetattr(fd, TCSAFLUSH, &config);
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size != 0) {
        ssize_t sent = write(fd, data, size);

        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0) {
            if (sent == 0)
                errno = EIO;
            return -1;
        }

        data += sent;
        size -= (size_t)sent;
    }

    return 0;
}

static int read_all(int fd, uint8_t *data, size_t size)
{
    while (size != 0) {
        ssize_t received = read(fd, data, size);

        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0) {
            if (received == 0)
                errno = ETIMEDOUT;
            return -1;
        }

        data += received;
        size -= (size_t)received;
    }

    return 0;
}

static uint32_t read_le32(const uint8_t *wire)
{
    return (uint32_t)read_le16(wire) | ((uint32_t)read_le16(wire + 2) << 16);
}

static int bridge_io_once(const char *port, const char *control)
{
    enum { request_header_size = 16 };
    uint8_t request[request_header_size + SSD_LBA_SIZE];
    uint8_t reply[8 + SSD_LBA_SIZE] = {0};
    uint8_t command[SSD_CMD_HEADER_SIZE];
    uint8_t completion[SSD_CPL_HEADER_SIZE];
    int result = 1;
    int uart = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);
    int ctl = -1;

    if (uart < 0) {
        perror(port);
        return 1;
    }
    if (configure_uart(uart, 10) < 0) {
        perror("UART configuration");
        goto done;
    }
    ctl = open(control, O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        perror(control);
        goto done;
    }
    ssize_t received;
    do {
        received = read(ctl, request, sizeof(request));
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        perror("kernel request");
        goto done;
    }
    if (received < request_header_size) {
        fputs("Incomplete kernel request header\n", stderr);
        goto done;
    }

    const uint32_t op = read_le32(request + 4);
    const int is_write = op == SSD_OP_WRITE;
    const size_t expected_size = request_header_size + (is_write ? SSD_LBA_SIZE : 0);

    /* Kernel IPC uses a 32-bit ID. The single UART command uses a 16-bit CID. */
    SSD_Command cmd = {
        .op = (uint8_t)op,
        .cid = (uint16_t)read_le32(request),
        .slba = read_le32(request + 8),
        .nlb = 0,
    };
    memcpy(reply, request, 4);
    reply[4] = 1; /* Kernel lab treats every nonzero status as an I/O error. */

    if ((op != SSD_OP_READ && !is_write) ||
        read_le32(request + 12) != SSD_LBA_SIZE ||
        (size_t)received != expected_size) {
        fputs("Unsupported kernel request\n", stderr);
    } else {
        encode_command(command, &cmd);
        if (write_all(uart, command, sizeof(command)) < 0 ||
            (is_write && write_all(uart, request + request_header_size, SSD_LBA_SIZE) < 0)) {
            perror("UART request");
        } else if (read_all(uart, completion, sizeof(completion)) < 0) {
            perror("UART completion");
        } else if (read_le16(completion) != cmd.cid ||
                   read_le16(completion + 2) != SOLID_OK) {
            fputs("UART response CID or status mismatch\n", stderr);
        } else if (!is_write && read_all(uart, reply + 8, SSD_LBA_SIZE) < 0) {
            perror("UART READ payload");
        } else {
            reply[4] = 0;
        }
    }

    ssize_t sent = write(ctl, reply, sizeof(reply));
    if (sent < 0) {
        perror("kernel reply");
    } else if (sent != (ssize_t)sizeof(reply)) {
        fputs("Incomplete kernel reply\n", stderr);
    } else if (reply[4] == 0) {
        printf("PASS bridge-%s: LBA=%u bytes=%u\n", is_write ? "write" : "read",
               (unsigned)cmd.slba, (unsigned)SSD_LBA_SIZE);
        result = 0;
    }

done:
    if (ctl >= 0)
        close(ctl);
    close(uart);
    return result;
}

int main(int argc, char **argv)
{
    SSD_Command cmd = { .op = SSD_OP_READ, .cid = 0x1234, .slba = 2, .nlb = 0 };
    uint8_t command[SSD_CMD_HEADER_SIZE];
    uint8_t completion[SSD_CPL_HEADER_SIZE];
    uint16_t expected_status = SOLID_OK;
    struct stat device;
    int result = 1;

    if (argc == 4 && (strcmp(argv[2], "bridge-io") == 0 ||
                      strcmp(argv[2], "bridge-read") == 0))
        return bridge_io_once(argv[1], argv[3]);

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <serial-device> <read|write|bad-write|bad-op|bad-nlb>\n", argv[0]);
        fprintf(stderr, "       %s <serial-device> bridge-io <control-device>\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[2], "write") == 0) {
        cmd.op = SSD_OP_WRITE;
    } else if (strcmp(argv[2], "bad-write") == 0) {
        cmd.op = SSD_OP_WRITE;
        expected_status = 0x007f; /* Diagnostic payload mismatch. */
    } else if (strcmp(argv[2], "bad-op") == 0) {
        cmd.op = 0xff;
        expected_status = ERR_HIL_BAD_COMMAND;
    } else if (strcmp(argv[2], "bad-nlb") == 0) {
        cmd.nlb = 1;
        expected_status = ERR_HIL_BAD_FIELD;
    } else if (strcmp(argv[2], "read") != 0) {
        fprintf(stderr, "Unknown test: %s\n", argv[2]);
        return 2;
    }

    int fd = open(argv[1], O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }

    if (fstat(fd, &device) < 0) {
        perror("fstat");
        goto done;
    }
    if (!S_ISCHR(device.st_mode)) {
        fprintf(stderr, "%s is not a serial character device\n", argv[1]);
        goto done;
    }
    if (configure_uart(fd, 30) < 0) {
        perror("UART configuration");
        goto done;
    }

    encode_command(command, &cmd);
    if (write_all(fd, command, sizeof(command)) < 0) {
        perror("command write");
        goto done;
    }
    if (cmd.op == SSD_OP_WRITE) {
        uint8_t payload[SSD_LBA_SIZE];

        for (size_t i = 0; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(i ^ (i >> 8));

        if (expected_status != SOLID_OK)
            payload[sizeof(payload) - 1] ^= 1U;

        if (write_all(fd, payload, sizeof(payload)) < 0) {
            perror("WRITE payload");
            goto done;
        }
    }
    if (read_all(fd, completion, sizeof(completion)) < 0) {
        perror("completion read");
        goto done;
    }

    const uint16_t cid = read_le16(&completion[0]);
    const uint16_t status = read_le16(&completion[2]);
    if (cid != cmd.cid || status != expected_status) {
        fprintf(stderr, "FAIL %s: CID=0x%04x status=0x%04x, expected 0x%04x/0x%04x\n",
                argv[2], (unsigned)cid, (unsigned)status,
                (unsigned)cmd.cid, (unsigned)expected_status);
        goto done;
    }

    if (cmd.op == SSD_OP_READ && status == SOLID_OK) {
        uint8_t payload[SSD_LBA_SIZE];

        if (read_all(fd, payload, sizeof(payload)) < 0) {
            perror("READ payload");
            goto done;
        }

        for (size_t i = 0; i < sizeof(payload); ++i) {
            const uint8_t expected = (uint8_t)(i ^ (i >> 8));

            if (payload[i] != expected) {
                fprintf(stderr, "FAIL read: byte %zu = 0x%02x, expected 0x%02x\n",
                        i, (unsigned)payload[i], (unsigned)expected);
                goto done;
            }
        }

        printf("PASS payload: %zu bytes matched\n", sizeof(payload));
    }

    printf("PASS %s: CID=0x%04x status=0x%04x\n",
           argv[2], (unsigned)cid, (unsigned)status);
    result = 0;

done:
    close(fd);
    return result;
}
