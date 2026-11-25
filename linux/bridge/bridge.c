#define _DEFAULT_SOURCE

#include "protocol.h"
#include "solid_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

_Static_assert(SSD_CMD_HEADER_SIZE == 13 && SSD_CPL_HEADER_SIZE == 4,
               "expected the current UART header layout");
_Static_assert(SSD_LBA_SIZE == SOLID_IPC_BLOCK_BYTES, "block size mismatch");
_Static_assert(SSD_OP_READ == SOLID_IPC_OP_READ &&
               SSD_OP_WRITE == SOLID_IPC_OP_WRITE, "opcode mismatch");

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)get_le16(p) | ((uint32_t)get_le16(p + 2) << 16);
}

static int configure_uart(int fd)
{
    struct termios settings;

    if (tcgetattr(fd, &settings) < 0)
        return -1;
    cfmakeraw(&settings);
    settings.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, B115200) < 0 || cfsetospeed(&settings, B115200) < 0)
        return -1;
    return tcsetattr(fd, TCSAFLUSH, &settings);
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int uart_transfer(int fd, uint8_t *data, size_t size, int sending,
                         int64_t deadline)
{
    struct pollfd port = { .fd = fd, .events = sending ? POLLOUT : POLLIN };

    while (size) {
        int64_t now = monotonic_ms();
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        int ready = poll(&port, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (!ready || (port.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            errno = ready ? EIO : ETIMEDOUT;
            return -1;
        }
        if (!(port.revents & port.events))
            continue;

        ssize_t n = sending ? write(fd, data, size) : read(fd, data, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

/* A negative result means that the UART stream can no longer be trusted. */
static int transfer_block(int fd, uint8_t op, uint16_t cid, uint64_t lba,
                          uint8_t *data, int64_t deadline)
{
    uint8_t command[SSD_CMD_HEADER_SIZE] = {op, (uint8_t)cid, (uint8_t)(cid >> 8)};
    uint8_t completion[SSD_CPL_HEADER_SIZE];

    for (unsigned int i = 0; i < 8; ++i)
        command[3 + i] = (uint8_t)(lba >> (i * 8U));
    /* nlb stays zero: the serial firmware processes one logical block. */
    if (uart_transfer(fd, command, sizeof(command), 1, deadline) < 0 ||
        (op == SSD_OP_WRITE && uart_transfer(fd, data, SSD_LBA_SIZE, 1, deadline) < 0) ||
        uart_transfer(fd, completion, sizeof(completion), 0, deadline) < 0)
        return -1;

    if (get_le16(completion) != cid) {
        errno = EPROTO;
        return -1;
    }
    uint16_t status = get_le16(completion + 2);
    if (status != SOLID_OK)
        return status;
    if (op == SSD_OP_READ && uart_transfer(fd, data, SSD_LBA_SIZE, 0, deadline) < 0)
        return -1;
    return 0;
}

static int serve_requests(int uart, int control)
{
    static uint8_t request[SOLID_IPC_REQUEST_HEADER + SOLID_IPC_MAX_BYTES];
    static uint8_t reply[SOLID_IPC_REPLY_HEADER + SOLID_IPC_MAX_BYTES];
    uint16_t cid = 0;

    for (;;) {
        ssize_t received = read(control, request, sizeof(request));
        if (received < 0 && errno == EINTR)
            continue;
        if (received < (ssize_t)SOLID_IPC_REQUEST_HEADER) {
            if (received >= 0)
                errno = EPROTO;
            perror("read /dev/solidctl");
            return 1;
        }

        const uint32_t op = get_le32(request + 4);
        const uint32_t lba = get_le32(request + 8);
        const uint32_t length = get_le32(request + 12);
        const int reading = op == SOLID_IPC_OP_READ;
        int status = 1;

        if ((reading || op == SOLID_IPC_OP_WRITE) && length &&
            length <= SOLID_IPC_MAX_BYTES && length % SSD_LBA_SIZE == 0 &&
            (size_t)received == SOLID_IPC_REQUEST_HEADER + (reading ? 0U : length)) {
            uint8_t *data = reading ? reply + SOLID_IPC_REPLY_HEADER :
                                      request + SOLID_IPC_REQUEST_HEADER;
            int64_t now = monotonic_ms();

            status = now < 0 ? -1 : 0;
            const int64_t deadline = now + SOLID_IPC_TIMEOUT_MS - 2000;
            for (uint32_t offset = 0; status == 0 && offset < length; offset += SSD_LBA_SIZE)
                status = transfer_block(uart, (uint8_t)op, ++cid,
                                        (uint64_t)lba + offset / SSD_LBA_SIZE,
                                        data + offset, deadline);

            if (status < 0)
                perror("UART transaction");
            else if (status)
                fprintf(stderr, "STM32 returned status 0x%04x at CID %u\n",
                        (unsigned int)status, (unsigned int)cid);
        } else {
            fputs("Invalid kernel request\n", stderr);
        }

        memcpy(reply, request, 4);
        memset(reply + 4, 0, 4);
        reply[4] = status != 0;
        size_t reply_size = SOLID_IPC_REPLY_HEADER + (reading && status == 0 ? length : 0);
        ssize_t sent;
        do {
            sent = write(control, reply, reply_size);
        } while (sent < 0 && errno == EINTR);
        if (sent != (ssize_t)reply_size) {
            if (sent >= 0)
                errno = EIO;
            perror("write /dev/solidctl");
            return 1;
        }
        if (status < 0) {
            fputs("UART stream lost. Reinitialize HIL before restarting the bridge.\n", stderr);
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ST-LINK serial device>\n", argv[0]);
        return 2;
    }

    int uart = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (uart < 0) {
        perror(argv[1]);
        return 1;
    }
    if (configure_uart(uart) < 0) {
        perror("UART configuration");
        close(uart);
        return 1;
    }

    int control = open("/dev/solidctl", O_RDWR | O_CLOEXEC);
    if (control < 0) {
        perror("open /dev/solidctl");
        close(uart);
        return 1;
    }

    puts("SOLID bridge ready");
    fflush(stdout);
    int result = serve_requests(uart, control);
    close(control);
    close(uart);
    return result;
}
