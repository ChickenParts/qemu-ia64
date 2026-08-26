# Default configuration for ia64-softmmu

# Basic UART for early console/debug (hw/char/serial-mm.c).
CONFIG_SERIAL=y
CONFIG_SERIAL_MM=y

# PCI + VGA for guest firmware (Flash.fd) UI and device enumeration.
CONFIG_PCI=y
CONFIG_PCI_DEVICES=y
CONFIG_PIIX=y
CONFIG_SMBUS_EEPROM=y
CONFIG_PCKBD=y

# Standard ISA floppy controller; PIIX supplies its IRQ and i8257 DMA paths.
CONFIG_FDC_ISA=y
