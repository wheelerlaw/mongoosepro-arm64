/*++

public.h

Public interface shared between dtmonpro.sys (ARM64 KMDF) and user-mode
callers (monpj432.dll). The device interface GUID and the IOCTL codes are
identical to the original x64 vendor driver so the existing 32-bit J2534
DLL works unchanged.

--*/

#ifndef DTMONPRO_PUBLIC_H
#define DTMONPRO_PUBLIC_H

//
// Device interface exposed via WdfDeviceCreateDeviceInterface.
// Same GUID the original driver registers and that monpj432.dll opens.
// {F99F6FCE-03F4-4817-8A03-8CFDE4743B92}
//
DEFINE_GUID(GUID_DEVINTERFACE_DTMONPRO,
    0xf99f6fce, 0x03f4, 0x4817, 0x8a, 0x03, 0x8c, 0xfd, 0xe4, 0x74, 0x3b, 0x92);

//
// All IOCTLs use device type 0x5500, METHOD_BUFFERED. The numeric values
// below must match the original driver exactly; the C_ASSERTs turn any
// CTL_CODE combination mistake into a compile error.
//
#define DTMONPRO_DEVICE_TYPE    0x5500

// Returns the full USB configuration descriptor.                    0x55006000
#define IOCTL_DTMONPRO_GET_CONFIG_DESCRIPTOR \
    CTL_CODE(DTMONPRO_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

// Returns 8 constant bytes { 1, 0, 0, 0, 1, 0, 0, 0 }.              0x55006018
#define IOCTL_DTMONPRO_GET_CONSTANTS \
    CTL_CODE(DTMONPRO_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS)

// Returns USB string descriptor index 3 (serial), LANGID 0x0409,
// UTF-16LE, not NUL-terminated.                                     0x5500601C
#define IOCTL_DTMONPRO_GET_SERIAL_NUMBER \
    CTL_CODE(DTMONPRO_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS)

// Resets the USB port (interrupt/bulk targets are stopped around it).0x5500A004
#define IOCTL_DTMONPRO_RESET_PORT \
    CTL_CODE(DTMONPRO_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// Vendor control transfer 0xDA (device re-enumeration).             0x5500A00C
#define IOCTL_DTMONPRO_REENUMERATE \
    CTL_CODE(DTMONPRO_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)

C_ASSERT(IOCTL_DTMONPRO_GET_CONFIG_DESCRIPTOR == 0x55006000);
C_ASSERT(IOCTL_DTMONPRO_GET_CONSTANTS          == 0x55006018);
C_ASSERT(IOCTL_DTMONPRO_GET_SERIAL_NUMBER      == 0x5500601C);
C_ASSERT(IOCTL_DTMONPRO_RESET_PORT             == 0x5500A004);
C_ASSERT(IOCTL_DTMONPRO_REENUMERATE            == 0x5500A00C);

#endif // DTMONPRO_PUBLIC_H
