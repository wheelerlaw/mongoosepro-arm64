# mongoosepro-arm64

Unofficial native **ARM64** KMDF replacement for `dtmonpro.sys` — the kernel
driver for the Drew Technologies **MongoosePro JLR** J2534 interface
(`USB\VID_18E1&PID_0104`). Written for running Volvo **VIDA 2014D** (a 32-bit
x86 app) on Windows 11 ARM64 (e.g. Apple Silicon VMs), where the vendor's
x64-only kernel driver cannot load.

> **Unofficial**: not affiliated with, or endorsed by, Drew Technologies /
> Opus IVS. The kernel driver here is a clean-room reimplementation; you
> still need the vendor's user-mode pieces.

## What this replaces — and what it doesn't

This repo ships **only** the kernel-mode driver (`dtmonpro.sys` + INF).
VIDA talks to the device through the vendor's 32-bit J2534 DLL, which is
still required:

- `monpj432.dll` and `MPConfigApp.exe` — install them from the vendor
  package (`J2534_Driver_x64.msi`) **with its x64 driver-install step
  skipped/removed** (the MSI's DIFx action fails on ARM64; the file copy
  and registry registration work fine).
- The J2534 registry registration under
  `HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04\Drew Technologies Inc. -
  MongoosePro JLR` (the vendor MSI creates this; it must point at the
  x86 `monpj432.dll`).

## Requirements

- Windows 11 ARM64 (10.0.22000 / build 26100 tested; the INF declares
  `NTARM64.10.0...16299`)
- A MongoosePro JLR cable on `USB\VID_18E1&PID_0104`
- **Test-signing enabled** (the package is signed with a test cert, not an
  attestation/production signature):

  ```bat
  bcdedit /set testsigning on
  ```
  then reboot. Secure Boot must be **off** — Windows refuses test-signed
  drivers with Secure Boot on (this is automatic on VMs that boot the
  non-secure EDK2 firmware).

## Install (from a release zip)

1. Download `dtmonpro-arm64-<version>.zip` from
   [Releases](../../releases) and extract it (4 files: `.sys`, `.inf`,
   `.cat`, `mongoosepro-arm64-test.cer`).
2. Trust the test certificate (elevated):
   ```bat
   certutil -addstore -f Root mongoosepro-arm64-test.cer
   certutil -addstore -f TrustedPublisher mongoosepro-arm64-test.cer
   ```
3. Stage the driver:
   ```bat
   pnputil /add-driver dtmonpro.inf /install
   ```
   It publishes as `oemNN.inf` (class `VehiclePassThru`) even though the
   cable isn't plugged in — that's normal.
4. Plug the cable in. Windows binds `dtmonpro.sys` automatically; the
   device appears under **Device Manager → Vehicle PassThru Interfaces**,
   and the vendor's `MPConfigApp.exe` shows the serial number once the
   vendor package is installed.

## Uninstall

```bat
pnputil /delete-driver oemNN.inf /uninstall /force
```

## Local build

Visual Studio 2022 (Community or better) + the "Windows Driver Kit"
component + standalone Windows SDK & WDK 10.0.26100, with the MSVC ARM64
build tools and ARM64 Spectre-mitigated libraries installed:

```bat
msbuild dtmonpro.sln /p:Configuration=Release /p:Platform=ARM64
```

Output package: `ARM64\Release\dtmonpro\` (sys + inf + cat, test-signed
with a local `WDKTestCert`). To build without any signing, pass
`/p:SignMode=Off`.

## CI / releases

`.github/workflows/build.yml` builds `Release|ARM64` on the GitHub-hosted
`windows-11-arm` runner (VS 2022 Enterprise + WDK 10.0.26100 + ARM64
Spectre libs are in the image; MSBuild is located via `vswhere`).
`/warnaserror` is on — any warning fails the build.

- Every run stamps the INF `DriverVer`, produces a catalog via `inf2cat`,
  and uploads `dtmonpro-arm64-<version>.zip` + `SHA256SUMS` as an artifact.
- If the `SIGNING_PFX_BASE64` / `SIGNING_PFX_PASSWORD` secrets are present,
  the `.sys` and `.cat` are Authenticode-signed with that cert; the
  matching public cert is `certs/mongoosepro-arm64-test.cer` and is
  included in the zip.
- Tagging `vX.Y.Z` stamps version `X.Y.Z.0`, attaches the zip + sums to a
  GitHub release, and **requires** signing (the release step fails if the
  secrets are absent).

## Debugging

Errors are reported via `KdPrintEx(DPFLTR_IHVDRIVER_ID, ...)`; there is no
WPP tracing. To see them:

- **DebugView** (Sysinternals, run elevated): Capture → Capture Kernel.
- Or raise the kernel print filter (elevated, then reboot):

  ```bat
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xffffffff
  ```

## Known deviations / notes

- **Missing pipes now fail.** The original logged an error if the bulk
  IN, bulk OUT or interrupt pipe was missing after SelectConfig and
  carried on with NULL handles. This build fails `PrepareHardware` with
  `STATUS_INSUFFICIENT_RESOURCES` (deliberate; documented in code).
- The original's unused manual queue, WPP/WMI tracing, and the
  friendly-name/location property lookups are omitted.
- Interrupt-pipe continuous reader: 1-byte transfers stored in the
  device context, never consumed — faithful to the original.
- On every file create, a vendor control transfer (`bRequest 0xDB`,
  `wValue 1`) is issued and the create always succeeds; on cleanup the
  same request goes out with `wValue 0` — faithful to the original.
- `[ClassInstall32]` is kept from the vendor INF so the
  `VehiclePassThru` class name registers. If you ever need strict
  InfVerif `/u` (universal) compliance you may have to drop it and
  accept the class showing a raw GUID in Device Manager.
- KMDF 1.15 is inbox on Windows 10/11 — no coinstaller is needed.
