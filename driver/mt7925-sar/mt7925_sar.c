// SPDX-License-Identifier: GPL-2.0-only
#include <ntifs.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>
#include <devguid.h>
#include "mt7925_sar_ioctl.h"

typedef struct _SAR_CHILD {
  WDFQUEUE parent_queue;
} SAR_CHILD;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SAR_CHILD, SarChild)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SarAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SarChildControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SarParentControl;

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
  WDF_DRIVER_CONFIG cfg;
  WDF_DRIVER_CONFIG_INIT(&cfg, SarAdd);
  return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

static void EvaluateSar(WDFIOTARGET target, const char name[4], NEKODROP_SAR_METHOD *method) {
  ACPI_EVAL_INPUT_BUFFER input = {0};
  input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
  RtlCopyMemory(input.MethodName, name, 4);

  WDF_MEMORY_DESCRIPTOR in, out;
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, method->data, sizeof(method->data));
  WDF_REQUEST_SEND_OPTIONS options;
  WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(2));

  ULONG_PTR returned = 0;
  NTSTATUS status = WdfIoTargetSendIoctlSynchronously(target, WDF_NO_HANDLE,
      IOCTL_ACPI_EVAL_METHOD, &in, &out, &options, &returned);
  if (returned > sizeof(method->data)) status = STATUS_DEVICE_DATA_ERROR;
  if (!NT_SUCCESS(status)) {
    RtlZeroMemory(method->data, sizeof(method->data));
    returned = 0;
  }

  method->status = (unsigned int)status;
  method->size = (unsigned int)returned;
}

VOID SarParentControl(WDFQUEUE queue, WDFREQUEST request, size_t out_len, size_t in_len, ULONG code) {
  if (code != IOCTL_NEKODROP_MT7925_READ_SAR || in_len) {
    WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
    return;
  }

  NEKODROP_MT7925_SAR *reply;
  NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(*reply), (PVOID *)&reply, NULL);
  if (!NT_SUCCESS(status)) {
    WdfRequestComplete(request, status);
    return;
  }

  UNREFERENCED_PARAMETER(out_len);
  RtlZeroMemory(reply, sizeof(*reply));
  reply->version = NEKODROP_MT7925_SAR_VERSION;
  WDFIOTARGET target = WdfDeviceGetIoTarget(WdfIoQueueGetDevice(queue));
  EvaluateSar(target, "MTDS", &reply->mtds);
  EvaluateSar(target, "MTGS", &reply->mtgs);
  WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*reply));
}

VOID SarChildControl(WDFQUEUE queue, WDFREQUEST request, size_t out_len, size_t in_len, ULONG code) {
  UNREFERENCED_PARAMETER(out_len);
  if (code != IOCTL_NEKODROP_MT7925_READ_SAR || in_len) {
    WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
    return;
  }
  if (WdfRequestGetRequestorMode(request) == UserMode) {
    PEPROCESS process = IoGetRequestorProcess(WdfRequestWdmGetIrp(request));
    PACCESS_TOKEN token = process ? PsReferencePrimaryToken(process) : NULL;
    const BOOLEAN admin = token && SeTokenIsAdmin(token);
    if (token) PsDereferencePrimaryToken(token);
    if (!admin) {
      WdfRequestComplete(request, STATUS_ACCESS_DENIED);
      return;
    }
  }
  WDF_REQUEST_FORWARD_OPTIONS options;
  WDF_REQUEST_FORWARD_OPTIONS_INIT(&options);
  NTSTATUS status = WdfRequestForwardToParentDeviceIoQueue(
      request, SarChild(WdfIoQueueGetDevice(queue))->parent_queue, &options);
  if (!NT_SUCCESS(status)) WdfRequestComplete(request, status);
}

static NTSTATUS CreateSarChild(WDFDEVICE parent, WDFQUEUE parent_queue) {
  DECLARE_CONST_UNICODE_STRING(device_id, L"NekoDrop\\MT7925Sar");
  DECLARE_CONST_UNICODE_STRING(instance_id, L"0");
  DECLARE_CONST_UNICODE_STRING(security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

  // A raw PDO publishes a sideband interface independently of the UMDF
  // function driver's file objects. KMDF owns the parent-child lifetime.
  PWDFDEVICE_INIT init = WdfPdoInitAllocate(parent);
  if (!init) return STATUS_INSUFFICIENT_RESOURCES;
  NTSTATUS status = WdfPdoInitAssignRawDevice(init, &GUID_DEVCLASS_SYSTEM);
  if (NT_SUCCESS(status)) status = WdfPdoInitAssignDeviceID(init, &device_id);
  if (NT_SUCCESS(status)) status = WdfPdoInitAssignInstanceID(init, &instance_id);
  if (NT_SUCCESS(status)) status = WdfDeviceInitAssignSDDLString(init, &security);
  if (!NT_SUCCESS(status)) {
    WdfDeviceInitFree(init);
    return status;
  }
  WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
  WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
  WdfPdoInitAllowForwardingRequestToParent(init);

  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, SAR_CHILD);
  WDFDEVICE child;
  status = WdfDeviceCreate(&init, &attrs, &child);
  if (!NT_SUCCESS(status)) {
    if (init) WdfDeviceInitFree(init);
    return status;
  }
  SarChild(child)->parent_queue = parent_queue;
  status = WdfDeviceCreateDeviceInterface(child, &GUID_DEVINTERFACE_NEKODROP_MT7925_SAR, NULL);
  if (NT_SUCCESS(status)) {
    WDF_IO_QUEUE_CONFIG cfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&cfg, WdfIoQueueDispatchSequential);
    cfg.EvtIoDeviceControl = SarChildControl;
    status = WdfIoQueueCreate(child, &cfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
  }
  if (NT_SUCCESS(status)) status = WdfFdoAddStaticChild(parent, child);
  if (!NT_SUCCESS(status)) WdfObjectDelete(child);
  return status;
}

NTSTATUS SarAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
  UNREFERENCED_PARAMETER(driver);
  DECLARE_CONST_UNICODE_STRING(e138, L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00");
  WCHAR hardware_ids[512] = {0};
  ULONG required = 0;
  NTSTATUS status = WdfFdoInitQueryProperty(init, DevicePropertyHardwareID,
      sizeof(hardware_ids) - sizeof(WCHAR), hardware_ids, &required);
  if (!NT_SUCCESS(status)) return status;
  UNICODE_STRING actual;
  RtlInitUnicodeString(&actual, hardware_ids);
  if (!RtlEqualUnicodeString(&actual, &e138, TRUE)) return STATUS_NOT_SUPPORTED;

  WdfFdoInitSetFilter(init);
  WDFDEVICE device;
  status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &device);
  if (!NT_SUCCESS(status)) return status;

  WDF_IO_QUEUE_CONFIG cfg;
  WDF_IO_QUEUE_CONFIG_INIT(&cfg, WdfIoQueueDispatchSequential);
  cfg.PowerManaged = WdfTrue;
  cfg.EvtIoDeviceControl = SarParentControl;
  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
  attrs.ExecutionLevel = WdfExecutionLevelPassive;
  WDFQUEUE queue;
  status = WdfIoQueueCreate(device, &cfg, &attrs, &queue);
  return NT_SUCCESS(status) ? CreateSarChild(device, queue) : status;
}
