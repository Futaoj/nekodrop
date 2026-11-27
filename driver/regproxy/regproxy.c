/* SPDX-License-Identifier: MIT */
/* UMDF 2 function driver that owns the Wi-Fi card's PCI function and relays
 * register accesses to a user-mode radio stack. No DMA, no interrupts. */

/* windows.h and ntstatus.h both define the common status codes. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include "regproxy_ioctl.h"
#include <ntstatus.h>
#include <wdf.h>
#include <devpkey.h>
#include <acpiioct.h>

/* Select the largest memory BAR of at least 8 KiB: Intel's 22000/AX210 CSR
 * window is 8 KiB (BAR0), while Realtek RTL8852CE/BE use a 1 MiB BAR2 and
 * MediaTek MT7921/MT7922 use a 1 MiB BAR0. Smaller resources are not CSRs. */
#define MIN_BAR_LEN 0x2000u

typedef struct _DEVICE_CONTEXT {
  PVOID bar; /* pseudo base address, only valid for the WDF register macros */
  SIZE_T bar_len;
  PHYSICAL_ADDRESS bar_phys;
  REGPROXY_OP close_ops[REGPROXY_MAX_CLOSE_OPS];
  size_t close_op_count;
  BOOLEAN client_open;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetCtx)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD RpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE RpEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE RpEvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RpEvtIoDeviceControl;
EVT_WDF_DEVICE_FILE_CREATE RpEvtFileCreate;
EVT_WDF_FILE_CLEANUP RpEvtFileCleanup;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG cfg;
  WDF_DRIVER_CONFIG_INIT(&cfg, RpEvtDeviceAdd);
  return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                         &cfg, WDF_NO_HANDLE);
}

NTSTATUS RpEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit) {
  UNREFERENCED_PARAMETER(Driver);

  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDevicePrepareHardware = RpEvtPrepareHardware;
  pnp.EvtDeviceReleaseHardware = RpEvtReleaseHardware;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

  WDF_FILEOBJECT_CONFIG files;
  WDF_FILEOBJECT_CONFIG_INIT(&files, RpEvtFileCreate, WDF_NO_EVENT_CALLBACK,
                             RpEvtFileCleanup);
  WdfDeviceInitSetFileObjectConfig(DeviceInit, &files,
                                   WDF_NO_OBJECT_ATTRIBUTES);

  /* Device scope serializes the queue against file cleanup, so the close ops
   * never race a batch still in flight from a dying client. */
  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
  attr.SynchronizationScope = WdfSynchronizationScopeDevice;
  WDFDEVICE dev;
  NTSTATUS st = WdfDeviceCreate(&DeviceInit, &attr, &dev);
  if (!NT_SUCCESS(st)) return st;

  st = WdfDeviceCreateDeviceInterface(dev, &GUID_DEVINTERFACE_WINAWDL_REGPROXY,
                                      NULL);
  if (!NT_SUCCESS(st)) return st;

  WDF_IO_QUEUE_CONFIG queue;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
  queue.EvtIoDeviceControl = RpEvtIoDeviceControl;
  return WdfIoQueueCreate(dev, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS RpEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                              WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Raw);
  DEVICE_CONTEXT *ctx = GetCtx(Device);
  ULONG count = WdfCmResourceListGetCount(Translated);

  PCM_PARTIAL_RESOURCE_DESCRIPTOR best = NULL;
  for (ULONG i = 0; i < count; i++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR res =
        WdfCmResourceListGetDescriptor(Translated, i);
    if (!res || res->Type != CmResourceTypeMemory) continue;
    if (res->u.Memory.Length < MIN_BAR_LEN) continue;
    if (!best || res->u.Memory.Length > best->u.Memory.Length) best = res;
  }
  if (!best) return STATUS_DEVICE_CONFIGURATION_ERROR;

  NTSTATUS st = WdfDeviceMapIoSpace(Device, best->u.Memory.Start,
                                    best->u.Memory.Length, MmNonCached,
                                    &ctx->bar);
  if (!NT_SUCCESS(st)) return st;
  ctx->bar_len = best->u.Memory.Length;
  ctx->bar_phys = best->u.Memory.Start;
  return STATUS_SUCCESS;
}

NTSTATUS RpEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Translated);
  DEVICE_CONTEXT *ctx = GetCtx(Device);
  if (ctx->bar) {
    WdfDeviceUnmapIoSpace(Device, ctx->bar, ctx->bar_len);
    ctx->bar = NULL;
    ctx->bar_len = 0;
  }
  ctx->close_op_count = 0;
  return STATUS_SUCCESS;
}

static BOOLEAN op_is_valid(const REGPROXY_OP *op, SIZE_T bar_len) {
  if (op->width != 1 && op->width != 2 && op->width != 4) return FALSE;
  if (op->kind > REGPROXY_CLEAR_BITS) return FALSE;
  if (op->offset & (op->width - 1u)) return FALSE;
  return (SIZE_T)op->offset + op->width <= bar_len;
}

static ULONG read_reg(WDFDEVICE dev, PUCHAR reg, UCHAR width) {
  if (width == 4) return WDF_READ_REGISTER_ULONG(dev, (PULONG)reg);
  if (width == 2) return WDF_READ_REGISTER_USHORT(dev, (PUSHORT)reg);
  return WDF_READ_REGISTER_UCHAR(dev, reg);
}

static void write_reg(WDFDEVICE dev, PUCHAR reg, UCHAR width, ULONG value) {
  if (width == 4)
    WDF_WRITE_REGISTER_ULONG(dev, (PULONG)reg, value);
  else if (width == 2)
    WDF_WRITE_REGISTER_USHORT(dev, (PUSHORT)reg, (USHORT)value);
  else
    WDF_WRITE_REGISTER_UCHAR(dev, reg, (UCHAR)value);
}

static void run_op(WDFDEVICE dev, PUCHAR reg, REGPROXY_OP *op) {
  switch (op->kind) {
  case REGPROXY_READ:
    op->value = read_reg(dev, reg, op->width);
    break;
  case REGPROXY_WRITE:
    write_reg(dev, reg, op->width, op->value);
    break;
  case REGPROXY_SET_BITS:
    write_reg(dev, reg, op->width, read_reg(dev, reg, op->width) | op->value);
    break;
  default:
    write_reg(dev, reg, op->width, read_reg(dev, reg, op->width) & ~op->value);
    break;
  }
}

static NTSTATUS run_batch(WDFDEVICE dev, const DEVICE_CONTEXT *ctx,
                          REGPROXY_OP *ops, size_t count) {
  /* Reject the whole batch before any op touches hardware. */
  for (size_t i = 0; i < count; i++)
    if (!op_is_valid(&ops[i], ctx->bar_len)) return STATUS_INVALID_PARAMETER;

  for (size_t i = 0; i < count; i++)
    run_op(dev, (PUCHAR)ctx->bar + ops[i].offset, &ops[i]);
  return STATUS_SUCCESS;
}

static NTSTATUS handle_get_info(WDFDEVICE dev, const DEVICE_CONTEXT *ctx,
                                WDFREQUEST request, size_t *written) {
  REGPROXY_INFO *info = NULL;
  NTSTATUS st = WdfRequestRetrieveOutputBuffer(request, sizeof(*info),
                                               (PVOID *)&info, NULL);
  if (!NT_SUCCESS(st)) return st;

  info->version = REGPROXY_VERSION;
  info->bar_len = (unsigned int)ctx->bar_len;
  info->bar_phys = (unsigned long long)ctx->bar_phys.QuadPart;
  info->direct_mapped =
      ctx->bar && WdfDeviceGetHardwareRegisterMappedAddress(dev, ctx->bar);
  *written = sizeof(*info);
  return STATUS_SUCCESS;
}

static NTSTATUS handle_batch(WDFDEVICE dev, const DEVICE_CONTEXT *ctx,
                             WDFREQUEST request, size_t *written) {
  REGPROXY_OP *in = NULL, *ops = NULL;
  size_t in_len = 0, out_len = 0;
  NTSTATUS st = WdfRequestRetrieveInputBuffer(request, sizeof(*in),
                                              (PVOID *)&in, &in_len);
  if (!NT_SUCCESS(st)) return st;
  st = WdfRequestRetrieveOutputBuffer(request, sizeof(*ops), (PVOID *)&ops,
                                      &out_len);
  if (!NT_SUCCESS(st)) return st;

  size_t count = in_len / sizeof(*ops);
  if (!ctx->bar) return STATUS_DEVICE_NOT_READY;
  if (count > REGPROXY_MAX_OPS || in_len % sizeof(*ops) || out_len < in_len)
    return STATUS_INVALID_PARAMETER;

  /* UMDF gives METHOD_BUFFERED requests separate input and output buffers,
   * unlike KMDF, so results have to be written to the output copy. */
  RtlMoveMemory(ops, in, in_len);
  st = run_batch(dev, ctx, ops, count);
  if (NT_SUCCESS(st)) *written = in_len;
  return st;
}

static NTSTATUS handle_set_close_ops(DEVICE_CONTEXT *ctx, WDFREQUEST request,
                                     size_t in_len) {
  if (in_len == 0) {
    ctx->close_op_count = 0;
    return STATUS_SUCCESS;
  }
  REGPROXY_OP *ops = NULL;
  NTSTATUS st = WdfRequestRetrieveInputBuffer(request, sizeof(*ops),
                                              (PVOID *)&ops, &in_len);
  if (!NT_SUCCESS(st)) return st;

  size_t count = in_len / sizeof(*ops);
  if (count > REGPROXY_MAX_CLOSE_OPS || in_len % sizeof(*ops))
    return STATUS_INVALID_PARAMETER;
  for (size_t i = 0; i < count; i++)
    if (!op_is_valid(&ops[i], ctx->bar_len) || ops[i].kind == REGPROXY_READ)
      return STATUS_INVALID_PARAMETER;

  RtlCopyMemory(ctx->close_ops, ops, in_len);
  ctx->close_op_count = count;
  return STATUS_SUCCESS;
}

static NTSTATUS check_e138(WDFDEVICE dev) {
  static const WCHAR hardware_id[] = L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00";
  WCHAR ids[512];
  WDF_DEVICE_PROPERTY_DATA prop;
  WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &DEVPKEY_Device_HardwareIds);
  DEVPROPTYPE type;
  ULONG length;
  NTSTATUS st = WdfDeviceQueryPropertyEx(dev, &prop, sizeof(ids), ids, &length, &type);
  if (!NT_SUCCESS(st)) return st;
  /* PCI lists the most specific (including revision) hardware ID first. */
  if (type != DEVPROP_TYPE_STRING_LIST || length < sizeof(hardware_id) ||
      _wcsnicmp(ids, hardware_id, ARRAYSIZE(hardware_id)) != 0)
    return STATUS_NOT_SUPPORTED;
  return STATUS_SUCCESS;
}

static void read_acpi_method(HANDLE file, const char name[4], REGPROXY_SAR_METHOD *out) {
  ACPI_EVAL_INPUT_BUFFER_V1 input = {0};
  input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V1;
  CopyMemory(input.MethodName, name, 4);
  HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!event) {
    out->status = (unsigned int)STATUS_INSUFFICIENT_RESOURCES;
    return;
  }
  OVERLAPPED io = {0};
  /* WDF owns the target's I/O completion port. Signal only our event. */
  io.hEvent = (HANDLE)((ULONG_PTR)event | 1);
  DWORD bytes = 0;
  BOOL ok = DeviceIoControl(file, IOCTL_ACPI_EVAL_METHOD, &input, sizeof(input),
                            out->data, sizeof(out->data), &bytes, &io);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  NTSTATUS st = STATUS_SUCCESS;
  if (error == ERROR_IO_PENDING) {
    DWORD wait = WaitForSingleObject(event, 2000);
    if (wait != WAIT_OBJECT_0) {
      CancelIoEx(file, &io);
      /* Cancellation can race completion. Keep all buffers live until done. */
      GetOverlappedResult(file, &io, &bytes, TRUE);
      st = wait == WAIT_TIMEOUT ? STATUS_IO_TIMEOUT : STATUS_UNSUCCESSFUL;
    } else {
      ok = GetOverlappedResult(file, &io, &bytes, FALSE);
      error = ok ? ERROR_SUCCESS : GetLastError();
    }
  }
  if (st == STATUS_SUCCESS && error != ERROR_SUCCESS) {
    /* Win32 error mappings are many-to-one. Only the actual I/O completion
     * can establish STATUS_OBJECT_NAME_NOT_FOUND, never GetLastError alone. */
    st = (NTSTATUS)io.Internal;
    if (NT_SUCCESS(st) || (st == STATUS_OBJECT_NAME_NOT_FOUND && error != ERROR_FILE_NOT_FOUND))
      st = STATUS_UNSUCCESSFUL;
  }
  CloseHandle(event);
  if (bytes > sizeof(out->data) || (st == STATUS_OBJECT_NAME_NOT_FOUND && bytes != 0))
    st = STATUS_DEVICE_DATA_ERROR;
  if (st != STATUS_SUCCESS) {
    SecureZeroMemory(out->data, sizeof(out->data));
    bytes = 0;
  }
  out->status = (unsigned int)st;
  out->size = (unsigned int)bytes;
}

static NTSTATUS check_acpi_address(WDFDEVICE dev, HANDLE file) {
  WDF_DEVICE_PROPERTY_DATA prop;
  WDF_DEVICE_PROPERTY_DATA_INIT(&prop, &DEVPKEY_Device_Address);
  DEVPROPTYPE type;
  ULONG bytes, address;
  NTSTATUS st = WdfDeviceQueryPropertyEx(dev, &prop, sizeof(address), &address, &bytes, &type);
  if (!NT_SUCCESS(st)) return st;
  if (type != DEVPROP_TYPE_UINT32 || bytes != sizeof(address)) return STATUS_DEVICE_DATA_ERROR;

  REGPROXY_SAR_METHOD result = {0};
  read_acpi_method(file, "_ADR", &result);
  if (result.status != STATUS_SUCCESS) return (NTSTATUS)result.status;
  PACPI_EVAL_OUTPUT_BUFFER_V1 output = (PACPI_EVAL_OUTPUT_BUFFER_V1)result.data;
  if (result.size != sizeof(*output) || output->Length != result.size ||
      output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 || output->Count != 1 ||
      output->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
      output->Argument[0].DataLength != sizeof(ULONG) || output->Argument[0].Argument != address)
    return STATUS_DEVICE_DATA_ERROR;
  return STATUS_SUCCESS;
}

static NTSTATUS handle_e138_sar(WDFDEVICE dev, WDFREQUEST request, size_t in_len, size_t *written) {
  if (in_len) return STATUS_INVALID_PARAMETER;
  NTSTATUS st = check_e138(dev);
  if (!NT_SUCCESS(st)) return st;
  REGPROXY_MT7925_SAR *reply;
  st = WdfRequestRetrieveOutputBuffer(request, sizeof(*reply), (PVOID *)&reply, NULL);
  if (!NT_SUCCESS(st)) return st;
  ZeroMemory(reply, sizeof(*reply));
  reply->version = REGPROXY_MT7925_SAR_VERSION;
  /* Driver-created requests need a file-backed lower target on this UMDF
   * stack. An open failure is a transport error, never an absent AML method. */
  WDFIOTARGET target;
  st = WdfIoTargetCreate(dev, WDF_NO_OBJECT_ATTRIBUTES, &target);
  if (!NT_SUCCESS(st)) return st;
  WDF_IO_TARGET_OPEN_PARAMS params;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&params, NULL);
  st = WdfIoTargetOpen(target, &params);
  if (!NT_SUCCESS(st)) {
    WdfObjectDelete(target);
    return st;
  }
  HANDLE file = WdfIoTargetWdmGetTargetFileHandle(target);
  st = file ? check_acpi_address(dev, file) : STATUS_INVALID_HANDLE;
  if (NT_SUCCESS(st)) {
    read_acpi_method(file, "MTDS", &reply->mtds);
    read_acpi_method(file, "MTGS", &reply->mtgs);
  }
  WdfIoTargetClose(target);
  WdfObjectDelete(target);
  if (!NT_SUCCESS(st)) return st;
  *written = sizeof(*reply);
  return STATUS_SUCCESS;
}

VOID RpEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen,
                          size_t InLen, ULONG Code) {
  UNREFERENCED_PARAMETER(OutLen);
  WDFDEVICE dev = WdfIoQueueGetDevice(Queue);
  DEVICE_CONTEXT *ctx = GetCtx(dev);
  NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
  size_t written = 0;

  if (Code == IOCTL_REGPROXY_MT7925_READ_SAR)
    st = handle_e138_sar(dev, Request, InLen, &written);
  else if (Code == IOCTL_REGPROXY_GET_INFO)
    st = handle_get_info(dev, ctx, Request, &written);
  else if (Code == IOCTL_REGPROXY_BATCH)
    st = handle_batch(dev, ctx, Request, &written);
  else if (Code == IOCTL_REGPROXY_SET_CLOSE_OPS)
    st = handle_set_close_ops(ctx, Request, InLen);

  WdfRequestCompleteWithInformation(Request, st, written);
}

/* One client at a time, so the close ops belong to exactly one handle. UMDF
 * has no WdfDeviceInitSetExclusive, hence the manual count. */
VOID RpEvtFileCreate(WDFDEVICE Device, WDFREQUEST Request,
                     WDFFILEOBJECT FileObject) {
  UNREFERENCED_PARAMETER(FileObject);
  DEVICE_CONTEXT *ctx = GetCtx(Device);
  if (ctx->client_open) {
    WdfRequestComplete(Request, STATUS_SHARING_VIOLATION);
    return;
  }
  ctx->client_open = TRUE;
  WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID RpEvtFileCleanup(WDFFILEOBJECT FileObject) {
  WDFDEVICE dev = WdfFileObjectGetDevice(FileObject);
  DEVICE_CONTEXT *ctx = GetCtx(dev);
  if (ctx->bar)
    for (size_t i = 0; i < ctx->close_op_count; i++)
      run_op(dev, (PUCHAR)ctx->bar + ctx->close_ops[i].offset,
             &ctx->close_ops[i]);
  ctx->close_op_count = 0;
  ctx->client_open = FALSE;
}
