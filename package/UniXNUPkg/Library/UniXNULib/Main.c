/** @file
  UniXNURun() and logging.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#include <Protocol/SerialIo.h>

STATIC EFI_SERIAL_IO_PROTOCOL  *mSerial;

VOID
UniXNULogInit (
  VOID
  )
{
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiSerialIoProtocolGuid, NULL, (VOID **)&mSerial))) {
    mSerial = NULL;
  }
}

VOID
EFIAPI
UniXNULog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[256];
  UINTN    Prefix;
  UINTN    Size;

  Prefix = AsciiSPrint (Line, sizeof (Line), "UNIXNU: ");
  VA_START (Args, Format);
  AsciiVSPrint (Line + Prefix, sizeof (Line) - Prefix - 2, Format, Args);
  VA_END (Args);
  AsciiStrCatS (Line, sizeof (Line), "\r\n");

  DEBUG ((PcdGet32 (PcdUniXNUDebugLevel), "%a", Line));
  if (mSerial != NULL) {
    Size = AsciiStrLen (Line);
    mSerial->Write (mSerial, &Size, Line);
  }
}

//
// The kernel paints one 48x32 bar per arm_init stage along the bottom of the
// framebuffer (sweet_early.c). Those pixels survive a crash and the reset, so
// reading them tells us how far the last boot actually got -- which is the one
// thing a screen full of UniXNU menu cannot show.
//
// Bar geometry must match sweet_early.c: x in [stage*48, stage*48+48),
// y in [height-32, height), colour 0xff00ff00 | ((stage & 7) * 32 << 16).
//
STATIC UINT32
ReadKernelStageMask (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_STATUS                    Status;
  volatile UINT32               *Fb;
  UINT32                        Width;
  UINT32                        Height;
  UINT32                        Stride;
  UINT32                        Stage;
  UINT32                        Mask;

  //
  // Preferred source: the bitmask the kernel itself stored in DRAM (0119).
  // It survives fbcon clearing the screen and the reset, unlike the bars.
  //
  {
    volatile UINT64  *Note = (volatile UINT64 *)(UINTN)0x8d3003000ULL;

    //
    // Note[1]: arm_init stages recorded by sweet_early_mark (0119).
    // Note[2]: start.s checkpoint recorded by the NUWA_BREADCRUMB macro (0120),
    //          0 = never entered start_first_cpu, 1 = entry, 3 = boot args read,
    //          5 = bootstrap page tables built, 6 = MMU enabled.
    //
    if (Note[0] == 0x4e4f5445ULL) {
      UINT32  StartCkpt = (UINT32)Note[2];
      UINT32  Stages    = (UINT32)Note[1];

      if ((StartCkpt != 0) || (Stages != 0)) {
        //
        // Fold the start.s checkpoint into the high half so one number carries
        // both facts: bits 32+ = checkpoint, low bits = arm_init stage mask.
        //
        return (UINT32)((Stages != 0) ? Stages : (0x40000000u | StartCkpt));
      }
    }
  }

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return 0;
  }

  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  Stride = Gop->Mode->Info->PixelsPerScanLine;
  if ((Height < 3200) || (Stride < Width) || (Gop->Mode->FrameBufferBase == 0)) {
    return 0;
  }

  Fb   = (volatile UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
  Mask = 0;
  for (Stage = 1; Stage <= 20; Stage++) {
    UINT64  X = (UINT64)Stage * 48 + 24;
    UINT32  Expect = 0xff00ff00u | ((Stage & 7u) * 32u << 16);
    UINT32  Pixel;

    if (X >= Width) {
      continue;
    }
    Pixel = Fb[(UINT64)(Height - 16) * Stride + X];
    if ((Pixel & 0xffffff00u) == (Expect & 0xffffff00u)) {
      Mask |= (1u << Stage);
    }
  }

  return Mask;
}

EFI_STATUS
EFIAPI
UniXNURun (
  IN EFI_HANDLE      ImageHandle,
  IN UNIXNU_HOST  Host
  )
{
  EFI_STATUS       Status;
  UNIXNU_CONFIG        Config;
  UNIXNU_UI            Ui;
  UNIXNU_INJECT_STATS  Stats;
  UINTN            Index;
  BOOLEAN          Countdown;
  UNIXNU_ENTRY         *Entry;
  UNIXNU_ENTRY         BootCopy;
  UINT32               KernelStages;

  //
  // A boot manager waits for people; the 5 minute BDS watchdog does not.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  UniXNULogInit ();
  UniXNULog (
    "UniXNU %a starting as %a, firmware %s rev 0x%x",
    UNIXNU_VERSION_STRING,
    (Host == UniXNUHostFirmware) ? "firmware boot option" : "application",
    (gST->FirmwareVendor != NULL) ? gST->FirmwareVendor : L"?",
    gST->FirmwareRevision
    );

  //
  // Started straight from BDS, storage may not be connected yet.
  //
  if (Host == UniXNUHostFirmware) {
    UniXNUConnectAll ();
  }

  UniXNUConfigLoad (ImageHandle, &Config);
  UniXNULog (
    "config: %u entries, %u drivers, %u dt patches, %u acpi tables, %u slices, %u kits loaded, %u skipped",
    (UINT32)Config.EntryCount,
    (UINT32)Config.DriverCount,
    (UINT32)Config.PatchCount,
    (UINT32)Config.AcpiCount,
    (UINT32)Config.SliceCount,
    (UINT32)Config.KitsLoaded,
    (UINT32)Config.KitsSkipped
    );

  //
  // Bring-up diagnostics: ConOut, so they show on the panel even with no
  // serial port wired (nuwa has none).
  //
  Print (L"UNIXNU: cfg entries=%u drivers=%u patches=%u kits=%u/%u\n",
         (UINT32)Config.EntryCount, (UINT32)Config.DriverCount, (UINT32)Config.PatchCount,
         (UINT32)Config.KitsLoaded, (UINT32)Config.KitsSkipped);

  UniXNUPublishSlices (&Config);

  //
  // Drivers before the UI: an injected driver may provide the display or
  // the input device the menu needs.
  //
  ZeroMem (&Stats, sizeof (Stats));
  UniXNUInjectDrivers (ImageHandle, &Config, &Stats);
  UniXNUInjectAcpi (&Config, &Stats);

  //
  // Grab the kernel's stage bars before UniXNUUiInit switches the mode and
  // paints over them.
  //
  KernelStages = ReadKernelStageMask ();

  UniXNUUiInit (&Config, &Ui);
  if (KernelStages != 0) {
    UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice),
                   L"last kernel boot reached stage mask 0x%x", KernelStages);
  } else {
    UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice),
                   L"no kernel stage bars found (kernel died before arm_init)");
  }
  Print (L"UNIXNU: kernel stage mask 0x%x\n", KernelStages);
  UnicodeSPrint (
    Ui.Info,
    sizeof (Ui.Info),
    L"drivers %u loaded / %u failed    kits %u    acpi %u",
    (UINT32)Stats.DriversLoaded,
    (UINT32)Stats.DriversFailed,
    (UINT32)Config.KitsLoaded,
    (UINT32)Stats.AcpiInstalled
    );

  Countdown = TRUE;
  for ( ; ;) {
    Index  = UniXNUMenuRun (&Ui, &Config, Countdown);
    Print (L"UNIXNU: menu picked %u of %u\n", (UINT32)Index, (UINT32)Config.EntryCount);
    if ((Config.EntryCount == 0) || (Index >= Config.EntryCount)) {
      Print (L"UNIXNU: entry index out of range - stopping\n");
      return EFI_NOT_FOUND;
    }
    Entry  = &Config.Entries[Index];
    BootCopy = *Entry;
    if (Entry->Confirm != NULL) {
      UINTN  Choice;

      if (!UniXNUMenuConfirm (&Ui, Entry, &Choice)) {
        UniXNULog ("menu: entry %u not confirmed", (UINT32)Index);
        UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' cancelled", Entry->Title);
        Countdown = FALSE;
        continue;
      }

      if (Entry->ConfirmCount > 0) {
        //
        // The chosen option's arguments follow the entry's own, on a copy: the entry itself stays as configured.
        //
        UINTN  Length = (Entry->Args != NULL ? StrLen (Entry->Args) : 0) + 1 + StrLen (Entry->ConfirmArgs[Choice]) + 1;

        BootCopy.Args = AllocateZeroPool (Length * sizeof (CHAR16));
        if (BootCopy.Args != NULL) {
          UnicodeSPrint (BootCopy.Args, Length * sizeof (CHAR16), L"%s %s", Entry->Args != NULL ? Entry->Args : L"", Entry->ConfirmArgs[Choice]);
        } else {
          BootCopy.Args = Entry->Args;
        }
      }
    }

    Status = UniXNUBootEntry (ImageHandle, &Config, &BootCopy);
    Print (L"UNIXNU: boot entry -> %r\n", Status);
    if ((Entry->Type == UniXNUEntryAction) && (Entry->Action == UniXNUActionExit)) {
      return EFI_SUCCESS;
    }

    //
    // The payload returned or failed: back to the menu, without the
    // countdown so a broken default cannot loop on a machine with input.
    //
    UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' returned: %r", Entry->Title, Status);
    UniXNULog ("menu: entry %u returned %r", (UINT32)Index, Status);
    gBS->SetWatchdogTimer (0, 0, 0, NULL);
    Countdown = FALSE;
  }
}
