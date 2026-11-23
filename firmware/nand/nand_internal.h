#ifndef SOLID_NAND_INTERNAL_H
#define SOLID_NAND_INTERNAL_H

#include "../common/nand.h"

#include <stddef.h>

enum
{
    NAND_CMD_SET_FEATURE = 0x1F,
    NAND_REG_STATUS = 0xC0,
    NAND_REG_CONFIGURATION = 0xB0,
};

typedef struct
{
    uint8_t port;
    uint8_t pin;
} nand_cs_t;

/* Call after validating the chip index. */
nand_cs_t nand_chip_select(uint8_t chip);
nand_result_t nand_write_configuration(nand_cs_t cs, uint8_t configuration);
nand_result_t nand_transfer(nand_cs_t cs, const uint8_t *command,
                            size_t command_size, uint8_t *data, size_t data_size);
/* Send the command and payload while keeping the chip selected. */
nand_result_t nand_transfer_write(nand_cs_t cs, const uint8_t *command,
                                  size_t command_size, const uint8_t *data, size_t data_size);
nand_result_t nand_get_feature(nand_cs_t cs, uint8_t address, uint8_t *value);
nand_result_t nand_wait_ready(nand_cs_t cs);

#endif
