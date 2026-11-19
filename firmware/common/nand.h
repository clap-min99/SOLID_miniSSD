
#ifndef SOLID_NAND_H
#define SOLID_NAND_H

#include <stdint.h>
#include <stdbool.h>

/* W25N01GV physical geometry */
#define NAND_MAX_CHIPS          8U
#define NAND_PAGE_SIZE         2048U
#define NAND_SPARE_SIZE        64U
#define NAND_PAGES_PER_BLOCK   64U
#define NAND_BLOCKS_PER_CHIP   1024U

/* Physical NAND address */
typedef struct {
    uint8_t chip;
    uint16_t block;
    uint8_t page;
} nand_addr_t;

/* NAND operation result */
typedef enum {
    NAND_OK = 0,
    NAND_ERR_INVALID_ARG,
    NAND_ERR_NOT_INITIALIZED,
    NAND_ERR_NO_DEVICE,
    NAND_ERR_TIMEOUT,
    NAND_ERR_IO,
    NAND_ERR_WRITE_PROTECTED,
    NAND_ERR_PROGRAM_FAIL,
    NAND_ERR_ERASE_FAIL,
    NAND_ERR_ECC_UNCORRECTABLE
} nand_result_t;

/* NAND physical geometry */
typedef struct {
    uint16_t page_size;
    uint16_t spare_size;
    uint16_t pages_per_block;
    uint16_t blocks_per_chip;
} nand_geometry_t;

/* ECC result of a page read */
typedef enum {
    NAND_ECC_NO_ERROR = 0,
    NAND_ECC_CORRECTED,
    NAND_ECC_UNCORRECTABLE,
    NAND_ECC_UNKNOWN
} nand_ecc_t;

/* Diagnostic status */
typedef struct {
    bool busy;
    bool write_enabled;
    bool program_failed;
    bool erase_failed;
    nand_ecc_t ecc;
} nand_status_t;

/* Initialization and discovery */
nand_result_t nand_init(void);

nand_result_t nand_probe(
    uint8_t chip
);

nand_result_t nand_get_geometry(
    uint8_t chip,
    nand_geometry_t *geometry
);

/* Data operations: 2048-byte main area */
nand_result_t nand_read_page(
    nand_addr_t addr,
    uint8_t *buffer,
    nand_ecc_t *ecc
);

nand_result_t nand_program_page(
    nand_addr_t addr,
    const uint8_t *buffer
);

/* Block-level erase */
nand_result_t nand_erase_block(
    uint8_t chip,
    uint16_t block
);

/* Factory bad-block marker inspection */
nand_result_t nand_is_factory_bad_block(
    uint8_t chip,
    uint16_t block,
    bool *is_bad
);

/* Diagnostics */
nand_result_t nand_get_status(
    uint8_t chip,
    nand_status_t *status
);

#endif /* SOLID_NAND_H */
