/*
 * windivert.c
 * (C) 2013, all rights reserved,
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ntddk.h>
#include <fwpsk.h>
#include <fwpmk.h>
#include <wdf.h>
#include <stdarg.h>
#include <ntstrsafe.h>

#include "windivert_device.h"

/*
 * WDK function declaration cruft.
 */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD windivert_unload;
EVT_WDF_IO_IN_CALLER_CONTEXT windivert_caller_context;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL windivert_ioctl;
EVT_WDF_DEVICE_FILE_CREATE windivert_create;
EVT_WDF_TIMER windivert_timer;
EVT_WDF_FILE_CLEANUP windivert_cleanup;
EVT_WDF_FILE_CLOSE windivert_close;

/*
 * Debugging macros.
 */
// #define DEBUG_ON
#define DEBUG_BUFSIZE       256

#ifdef DEBUG_ON
static void DEBUG(PCCH format, ...)
{
    va_list args;
    char buf[DEBUG_BUFSIZE+1];
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return;
    }
    va_start(args, format);
    RtlStringCbVPrintfA(buf, DEBUG_BUFSIZE, format, args);
    DbgPrint("WINDIVERT: %s\n", buf);
    va_end(args);
}
static void DEBUG_ERROR(PCCH format, NTSTATUS status, ...)
{
    va_list args;
    char buf[DEBUG_BUFSIZE+1];
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return;
    }
    va_start(args, status);
    RtlStringCbVPrintfA(buf, DEBUG_BUFSIZE, format, args);
    DbgPrint("WINDIVERT: *** ERROR ***: (status = %x): %s\n", status, buf);
    va_end(args);
}
#else       // DEBUG_ON
#define DEBUG(format, ...)
#define DEBUG_ERROR(format, status, ...)
#endif      // DEBUG_ON

/*
 * WinDivert packet filter.
 */
struct filter_s
{
    UINT8  protocol:4;                          // field's protocol
    UINT8  test:4;                              // Filter test
    UINT8  field;                               // Field of interest
    UINT16 success;                             // Success continuation
    UINT16 failure;                             // Fail continuation
    UINT32 arg[4];                              // Comparison argument
};
typedef struct filter_s *filter_t;
#define WINDIVERT_FILTER_PROTOCOL_NONE          0
#define WINDIVERT_FILTER_PROTOCOL_IP            1
#define WINDIVERT_FILTER_PROTOCOL_IPV6          2
#define WINDIVERT_FILTER_PROTOCOL_ICMP          3
#define WINDIVERT_FILTER_PROTOCOL_ICMPV6        4
#define WINDIVERT_FILTER_PROTOCOL_TCP           5
#define WINDIVERT_FILTER_PROTOCOL_UDP           6
#define WINDIVERT_FILTER_TAG                    'Fvid'

/*
 * WinDivert context information.
 */
#define WINDIVERT_CONTEXT_MAGIC                 0xAA5D1C5BC439AA72ull
#define WINDIVERT_CONTEXT_SIZE                  (sizeof(struct context_s))
#define WINDIVERT_CONTEXT_MAXLAYERS             4
#define WINDIVERT_CONTEXT_OUTBOUND_IPV4_LAYER   0
#define WINDIVERT_CONTEXT_INBOUND_IPV4_LAYER    1
#define WINDIVERT_CONTEXT_OUTBOUND_IPV6_LAYER   2
#define WINDIVERT_CONTEXT_INBOUND_IPV6_LAYER    3
typedef enum
{
    WINDIVERT_CONTEXT_STATE_OPENING = 0xA0,     // Context is opening.
    WINDIVERT_CONTEXT_STATE_OPEN    = 0xB1,     // Context is open.
    WINDIVERT_CONTEXT_STATE_CLOSING = 0xC2,     // Context is closing.
    WINDIVERT_CONTEXT_STATE_CLOSED  = 0xD3,     // Context is closed.
    WINDIVERT_CONTEXT_STATE_INVALID = 0xE4      // Context is invalid.
} context_state_t;
struct context_s
{
    UINT64 magic;                               // WINDIVERT_CONTEXT_MAGIC
    context_state_t state;                      // Context's state.
    KSPIN_LOCK lock;                            // Context-wide lock.
    WDFDEVICE device;                           // Context's device.
    LIST_ENTRY packet_queue;                    // Packet queue.
    ULONG packet_queue_length;                  // Packet queue length.
    ULONG packet_queue_maxlength;               // Packet queue max length.
    WDFTIMER timer;                             // Packet timer.
    UINT timer_timeout;                         // Packet timeout (in ms).
    BOOL timer_ticktock;                        // Packet timer ticktock.
    WDFQUEUE read_queue;                        // Read queue.
    KEVENT read_event;                          // Read event.
    void *read_thread;                          // Read thread.
    UINT8 layer_0;                              // Context's layer (initial).
    UINT8 layer;                                // Context's layer.
    UINT64 flags_0;                             // Context's flags (initial).
    UINT64 flags;                               // Context's flags.
    UINT32 priority_0;                          // Context's priority (initial).
    UINT32 priority;                            // Context's priority.
    GUID sublayer_guid[WINDIVERT_CONTEXT_MAXLAYERS];
                                                // Sublayer GUIDs.
    GUID callout_guid[WINDIVERT_CONTEXT_MAXLAYERS];
                                                // Callout GUIDs.
    GUID filter_guid[WINDIVERT_CONTEXT_MAXLAYERS];
                                                // Filter GUIDs.
    BOOL registered[WINDIVERT_CONTEXT_MAXLAYERS];
                                                // What is registered?
    HANDLE engine_handle;                       // WFP engine handle.
    LONG filter_on;                             // Is filter on?
    filter_t filter;                            // Packet filter.
};
typedef struct context_s context_s;
typedef struct context_s *context_t;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(context_s, windivert_context_get);

/*
 * WinDivert Layer information.
 */
typedef void (*windivert_callout_t)(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
struct layer_s
{
    wchar_t *sublayer_name;                 // Sub-layer name.
    wchar_t *sublayer_desc;                 // Sub-layer description.
    wchar_t *callout_name;                  // Call-out name.
    wchar_t *callout_desc;                  // Call-out description.
    wchar_t *filter_name;                   // Filter name.
    wchar_t *filter_desc;                   // Filter description.
    GUID guid;                              // WFP layer GUID.
    windivert_callout_t callout;            // Call-out.
};
typedef struct layer_s *layer_t;

/*
 * WinDivert request context.
 */
struct req_context_s
{
    struct windivert_addr_s *addr;          // Pointer to address structure.
};
typedef struct req_context_s req_context_s;
typedef struct req_context_s *req_context_t;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(req_context_s, windivert_req_context_get);

/*
 * WinDivert packet structure.
 */
#define WINDIVERT_PACKET_TAG                'Pvid'
#define WINDIVERT_PACKET_SIZE               (sizeof(struct packet_s))
struct packet_s
{
    LIST_ENTRY entry;                       // Entry for queue
    PNET_BUFFER buffer;                     // The packet
    PNET_BUFFER_LIST buffers;               // The NBL contain the packet
    PNET_BUFFER_LIST clone;                 // Clone of buffer
    UINT8 direction;                        // Packet direction
    UINT32 if_idx;                          // Interface index
    UINT32 sub_if_idx;                      // Sub-interface index
    BOOL ip_checksum;                       // IP checksum is valid
    BOOL tcp_checksum;                      // TCP checksum is valid
    BOOL udp_checksum;                      // UDP checksum is valid
    BOOL timer_ticktock;                    // Time-out ticktock
};
typedef struct packet_s *packet_t;
#define WINDIVERT_NET_BUFFER_LIST_TAG       'Lvid'

/*
 * WinDivert address definition.
 */
struct windivert_addr_s
{
    UINT32 IfIdx;
    UINT32 SubIfIdx;
    UINT8  Direction;
};
typedef struct windivert_addr_s *windivert_addr_t;

/*
 * Header definitions.
 */
struct iphdr
{
    UINT8  HdrLength:4;
    UINT8  Version:4;
    UINT8  TOS;
    UINT16 Length;
    UINT16 Id;
    UINT16 FragOff0;
    UINT8  TTL;
    UINT8  Protocol;
    UINT16 Checksum;
    UINT32 SrcAddr;
    UINT32 DstAddr;
};
struct ipv6hdr
{
    UINT8  TrafficClass0:4;
    UINT8  Version:4;
    UINT8  FlowLabel0:4;
    UINT8  TrafficClass1:4;
    UINT16 FlowLabel1;
    UINT16 Length;
    UINT8  NextHdr;
    UINT8  HopLimit;
    UINT32 SrcAddr[4];
    UINT32 DstAddr[4];
};
struct icmphdr
{
    UINT8  Type;
    UINT8  Code;
    UINT16 Checksum;
    UINT32 Body;
};
struct icmpv6hdr
{
    UINT8  Type;
    UINT8  Code;
    UINT16 Checksum;
    UINT32 Body;
};
struct tcphdr
{
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT32 SeqNum;
    UINT32 AckNum;
    UINT16 Reserved1:4;
    UINT16 HdrLength:4;
    UINT16 Fin:1;
    UINT16 Syn:1;
    UINT16 Rst:1;
    UINT16 Psh:1;
    UINT16 Ack:1;
    UINT16 Urg:1;
    UINT16 Reserved2:2;
    UINT16 Window;
    UINT16 Checksum;
    UINT16 UrgPtr;
};
struct udphdr
{
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT16 Length;
    UINT16 Checksum;
};

#define IPHDR_GET_FRAGOFF(hdr)          (((hdr)->FragOff0) & 0xFF1F)
#define IPHDR_GET_MF(hdr)               (((hdr)->FragOff0) & 0x0020)
#define IPHDR_GET_DF(hdr)               (((hdr)->FragOff0) & 0x0040)
#define IPV6HDR_GET_TRAFFICCLASS(hdr)   \
    ((((hdr)->TrafficClass0) << 4) | ((hdr)->TrafficClass1))
#define IPV6HDR_GET_FLOWLABEL(hdr)      \
    ((((UINT32)(hdr)->FlowLabel0) << 16) | ((UINT32)(hdr)->FlowLabel1))

/*
 * Misc.
 */
#define UINT8_MAX       0xFF
#define UINT16_MAX      0xFFFF

/*
 * Global handles.
 */
HANDLE inject_handle;
HANDLE injectv6_handle;
NDIS_HANDLE pool_handle;

/*
 * Prototypes.
 */
extern VOID windivert_ioctl(IN WDFQUEUE queue, IN WDFREQUEST request,
    IN size_t in_length, IN size_t out_len, IN ULONG code);
extern NTSTATUS windivert_read(context_t context, WDFREQUEST request);
static void windivert_read_service_worker(PVOID context_0);
static void windivert_read_service(context_t context);
static BOOLEAN windivert_context_verify(context_t context,
    context_state_t state);
extern VOID windivert_create(IN WDFDEVICE device, IN WDFREQUEST request,
    IN WDFFILEOBJECT object);
static NTSTATUS windivert_register_callouts(context_t context, BOOL is_inbound,
    BOOL is_outbound, BOOL is_ipv4, BOOL is_ipv6);
static NTSTATUS windivert_register_callout(context_t context, UINT idx,
    layer_t layer);
extern VOID windivert_timer(IN WDFTIMER timer);
extern VOID windivert_cleanup(IN WDFFILEOBJECT object);
extern VOID windivert_close(IN WDFFILEOBJECT object);
extern NTSTATUS windivert_write(context_t context, WDFREQUEST request,
    windivert_addr_t addr);
extern void NTAPI windivert_inject_complete(VOID *context,
    NET_BUFFER_LIST *packets, BOOLEAN dispatch_level);
static NTSTATUS windivert_notify_callout(IN FWPS_CALLOUT_NOTIFY_TYPE type,
    IN const GUID *filter_key, IN const FWPS_FILTER0 *filter);
static void windivert_classify_outbound_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_inbound_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_outbound_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_inbound_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_forward_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_forward_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static void windivert_classify_callout(IN UINT8 direction, IN UINT32 if_idx,
    IN UINT32 sub_if_idx, IN BOOL isipv4,
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result);
static BOOL windivert_reinject_packet(context_t context, UINT8 direction,
    BOOL isipv4, UINT32 if_idx, UINT32 sub_if_idx, UINT32 priority,
    PNET_BUFFER_LIST buffers, PNET_BUFFER buffer);
static void NTAPI windivert_reinject_complete(VOID *context,
    NET_BUFFER_LIST *buffers_cpy, BOOLEAN dispatch_level);
static BOOL windivert_queue_packet(context_t context, PNET_BUFFER_LIST buffers,
    PNET_BUFFER buffer, UINT8 direction, UINT32 if_idx, UINT32 sub_if_idx);
static void windivert_free_packet(packet_t packet);
static UINT16 windivert_checksum(const void *pseudo_header,
    size_t pseudo_header_len, const void *data, size_t size);
static void windivert_update_checksums(void *header, size_t len,
    BOOL update_ip, BOOL update_tcp, BOOL update_udp);
static BOOL windivert_filter(PNET_BUFFER buffer, UINT32 if_idx,
    UINT32 sub_if_idx, BOOL outbound, filter_t filter);
static filter_t windivert_filter_compile(windivert_ioctl_filter_t ioctl_filter,
    size_t ioctl_filter_len);
static void windivert_filter_analyze(filter_t filter, BOOL *is_inbound,
    BOOL *is_outbound, BOOL *ip_ipv4, BOOL *is_ipv6);
static BOOL windivert_filter_test(filter_t filter, UINT16 ip, UINT8 protocol,
    UINT8 field, UINT32 arg);

/*
 * Defined layers.
 */
static struct layer_s layer_inbound_network_ipv4_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerInboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (inbound IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutInboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" callout network (inbound IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterInboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" filter network (inbound IPv4)",
    {0},
    windivert_classify_inbound_network_v4_callout,
};
static layer_t layer_inbound_network_ipv4 = &layer_inbound_network_ipv4_0;

static struct layer_s layer_outbound_network_ipv4_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerOutboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (outbound IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutOutboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" callout network (outbound IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterOutboundNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" filter network (outbound IPv4)",
    {0},
    windivert_classify_outbound_network_v4_callout,
};
static layer_t layer_outbound_network_ipv4 = &layer_outbound_network_ipv4_0;

static struct layer_s layer_inbound_network_ipv6_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerInboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (inbound IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutInboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" callout network (inbound IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterInboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" filter network (inbound IPv6)",
    {0},
    windivert_classify_inbound_network_v6_callout,
};
static layer_t layer_inbound_network_ipv6 = &layer_inbound_network_ipv6_0;

static struct layer_s layer_outbound_network_ipv6_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerOutboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (outbound IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutOutboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" callout network (outbound IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterOutboundNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" filter network (outbound IPv6)",
    {0},
    windivert_classify_outbound_network_v6_callout,
};
static layer_t layer_outbound_network_ipv6 = &layer_outbound_network_ipv6_0;

static struct layer_s layer_forward_network_ipv4_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerForwardNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (forward IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutForwardNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" callout network (forward IPv4)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterForwardNetworkIPv4",
    L"" WINDIVERT_DEVICE_NAME L" filter network (forward IPv4)",
    {0},
    windivert_classify_forward_network_v4_callout,
};
static layer_t layer_forward_network_ipv4 = &layer_forward_network_ipv4_0;

static struct layer_s layer_forward_network_ipv6_0 =
{
    L"" WINDIVERT_DEVICE_NAME L"_SubLayerForwardNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" sublayer network (forward IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_CalloutForwardNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" callout network (forward IPv6)",
    L"" WINDIVERT_DEVICE_NAME L"_FilterForwardNetworkIPv6",
    L"" WINDIVERT_DEVICE_NAME L" filter network (forward IPv6)",
    {0},
    windivert_classify_forward_network_v6_callout,
};
static layer_t layer_forward_network_ipv6 = &layer_forward_network_ipv6_0;

/*
 * WinDivert driver entry routine.
 */
extern NTSTATUS DriverEntry(IN PDRIVER_OBJECT driver_obj,
    IN PUNICODE_STRING reg_path)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    PWDFDEVICE_INIT device_init;
    WDFDEVICE device;
    WDF_FILEOBJECT_CONFIG file_config;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDFQUEUE queue;
    WDF_OBJECT_ATTRIBUTES obj_attrs;
    NET_BUFFER_LIST_POOL_PARAMETERS pool_params;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(device_name,
        L"\\Device\\" WINDIVERT_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(dos_device_name,
        L"\\??\\" WINDIVERT_DEVICE_NAME);

    DEBUG("LOAD: loading WinDivert driver");

    // Initialize the layers.
    layer_inbound_network_ipv4->guid    = FWPM_LAYER_INBOUND_IPPACKET_V4;
    layer_outbound_network_ipv4->guid   = FWPM_LAYER_OUTBOUND_IPPACKET_V4;
    layer_inbound_network_ipv6->guid    = FWPM_LAYER_INBOUND_IPPACKET_V6;
    layer_outbound_network_ipv6->guid   = FWPM_LAYER_OUTBOUND_IPPACKET_V6;
    layer_forward_network_ipv4->guid    = FWPM_LAYER_IPFORWARD_V4;
    layer_forward_network_ipv6->guid    = FWPM_LAYER_IPFORWARD_V6;

    // Configure ourself as a non-PnP driver:
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = windivert_unload;
    status = WdfDriverCreate(driver_obj, reg_path, WDF_NO_OBJECT_ATTRIBUTES,
        &config, &driver);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WDF driver", status);
        return status;
    }
    device_init = WdfControlDeviceInitAllocate(driver,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (device_init == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        DEBUG_ERROR("failed to allocate WDF control device init structure",
            status);
        return status;
    }
    WdfDeviceInitSetDeviceType(device_init, FILE_DEVICE_NETWORK);
    WdfDeviceInitSetIoType(device_init, WdfDeviceIoDirect);
    status = WdfDeviceInitAssignName(device_init, &device_name);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WDF device name", status);
        WdfDeviceInitFree(device_init);
        return status;
    }
    WDF_FILEOBJECT_CONFIG_INIT(&file_config, windivert_create, windivert_close,
        windivert_cleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&obj_attrs, context_s);
    WdfDeviceInitSetFileObjectConfig(device_init, &file_config, &obj_attrs);
    WdfDeviceInitSetIoInCallerContextCallback(device_init,
        windivert_caller_context);
    WDF_OBJECT_ATTRIBUTES_INIT(&obj_attrs);
    status = WdfDeviceCreate(&device_init, &obj_attrs, &device);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WDF control device", status);
        WdfDeviceInitFree(device_init);
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config,
        WdfIoQueueDispatchParallel);
    queue_config.EvtIoRead          = NULL;
    queue_config.EvtIoWrite         = NULL;
    queue_config.EvtIoDeviceControl = windivert_ioctl;
    WDF_OBJECT_ATTRIBUTES_INIT(&obj_attrs);
    status = WdfIoQueueCreate(device, &queue_config, &obj_attrs, &queue);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create default WDF queue", status);
        return status;
    }
    status = WdfDeviceCreateSymbolicLink(device, &dos_device_name);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create device symbolic link", status);
        return status;
    }
    WdfControlFinishInitializing(device);

    // Create the packet injection handles.
    status = FwpsInjectionHandleCreate0(AF_INET, 
        FWPS_INJECTION_TYPE_NETWORK | FWPS_INJECTION_TYPE_FORWARD,
        &inject_handle);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WFP packet injection handle", status);
        return status;
    }
    status = FwpsInjectionHandleCreate0(AF_INET6, 
        FWPS_INJECTION_TYPE_NETWORK | FWPS_INJECTION_TYPE_FORWARD,
        &injectv6_handle);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WFP ipv6 packet injection handle",
            status);
        return status;
    }

    // Create the packet pool handle.
    RtlZeroMemory(&pool_params, sizeof(pool_params));
    pool_params.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pool_params.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool_params.Header.Size = sizeof(pool_params);
    pool_params.fAllocateNetBuffer = TRUE;
    pool_params.PoolTag = WINDIVERT_NET_BUFFER_LIST_TAG;
    pool_params.DataSize = 0;
    pool_handle = NdisAllocateNetBufferListPool(NULL, &pool_params);
    if (pool_handle == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        DEBUG_ERROR("failed to allocate net buffer list pool", status);
        return status;
    }

    return STATUS_SUCCESS;
}

/*
 * WinDivert driver unload routine.
 */
extern VOID windivert_unload(IN WDFDRIVER Driver)
{
    DEBUG("UNLOAD: unloading the WinDivert driver");
    FwpsInjectionHandleDestroy0(inject_handle);
    FwpsInjectionHandleDestroy0(injectv6_handle);
    NdisFreeNetBufferPool(pool_handle);
}

/*
 * WinDivert context verify.
 */
static BOOLEAN windivert_context_verify(context_t context,
    context_state_t state)
{
    if (context == NULL)
    {
        DEBUG_ERROR("failed to verify context; context is NULL",
            STATUS_INVALID_HANDLE);
        return FALSE;
    }
    if (context->magic != WINDIVERT_CONTEXT_MAGIC)
    {
        DEBUG_ERROR("failed to verify context; invalid magic number",
            STATUS_INVALID_HANDLE);
        return FALSE;
    }
    if (context->state != state)
    {
        DEBUG_ERROR("failed to verify context; expected context state %x, "
            "found context state %x", STATUS_INVALID_HANDLE, state,
            context->state);
        return FALSE;
    }
    return TRUE;
}

/*
 * WinDivert create routine.
 */
extern VOID windivert_create(IN WDFDEVICE device, IN WDFREQUEST request,
    IN WDFFILEOBJECT object)
{
    NET_BUFFER_LIST_POOL_PARAMETERS pool_params;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDF_TIMER_CONFIG timer_config;
    WDF_OBJECT_ATTRIBUTES timer_attributes;
    FWPM_SESSION0 session;
    HANDLE thread;
    NTSTATUS status = STATUS_SUCCESS;
    UINT8 i;
    context_t context = windivert_context_get(object);

    DEBUG("CREATE: creating a new WinDivert context (context=%p)", context);

    // Initialise the new context:
    context->magic  = WINDIVERT_CONTEXT_MAGIC;
    context->state  = WINDIVERT_CONTEXT_STATE_OPENING;
    context->device = device;
    context->packet_queue_length = 0;
    context->packet_queue_maxlength = WINDIVERT_PARAM_QUEUE_LEN_DEFAULT;
    context->timer_timeout = WINDIVERT_PARAM_QUEUE_TIME_DEFAULT;
    context->layer_0     = WINDIVERT_LAYER_DEFAULT;
    context->layer       = WINDIVERT_LAYER_DEFAULT;
    context->flags_0     = 0;
    context->flags       = 0;
    context->priority_0  = WINDIVERT_PRIORITY_DEFAULT;
    context->priority    = WINDIVERT_PRIORITY_DEFAULT;
    context->read_thread = NULL;
    context->filter      = NULL;
    for (i = 0; i < WINDIVERT_CONTEXT_MAXLAYERS; i++)
    {
        context->registered[i] = FALSE;
    }
    context->filter_on = FALSE;
    KeInitializeSpinLock(&context->lock);
    InitializeListHead(&context->packet_queue);
    for (i = 0; i < WINDIVERT_CONTEXT_MAXLAYERS; i++)
    {
        status = ExUuidCreate(&context->sublayer_guid[i]);
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed to create sub-layer GUID", status);
            goto windivert_create_exit;
        }
        status = ExUuidCreate(&context->callout_guid[i]);
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed to create callout GUID", status);
            goto windivert_create_exit;
        }
        status = ExUuidCreate(&context->filter_guid[i]);
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed to create filter GUID", status);
            goto windivert_create_exit;
        }
    }
    RtlZeroMemory(&pool_params, sizeof(pool_params));
    pool_params.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pool_params.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool_params.Header.Size = sizeof(pool_params);
    pool_params.fAllocateNetBuffer = TRUE;
    pool_params.PoolTag = WINDIVERT_NET_BUFFER_LIST_TAG;
    pool_params.DataSize = 0;
    WDF_IO_QUEUE_CONFIG_INIT(&queue_config, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queue_config, WDF_NO_OBJECT_ATTRIBUTES,
        &context->read_queue);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create I/O read queue", status);
        goto windivert_create_exit;
    }
    KeInitializeEvent(&context->read_event, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL,
        NULL, windivert_read_service_worker, (PVOID)context);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create read service thread", status);
        goto windivert_create_exit;
    }
    status = ObReferenceObjectByHandle(thread, 0, NULL, KernelMode,
        &context->read_thread, NULL);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create read service thread object", status);
        goto windivert_create_exit;
    }
    WDF_TIMER_CONFIG_INIT(&timer_config, windivert_timer);
    timer_config.AutomaticSerialization = TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&timer_attributes);
    timer_attributes.ParentObject = (WDFOBJECT)object;
    status = WdfTimerCreate(&timer_config, &timer_attributes, &context->timer);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create packet time-out timer", status);
        goto windivert_create_exit;
    }
    RtlZeroMemory(&session, sizeof(session));
    session.flags |= FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_DEFAULT, NULL, &session,
        &context->engine_handle);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create WFP engine handle", status);
        goto windivert_create_exit;
    }
    context->state = WINDIVERT_CONTEXT_STATE_OPEN;

windivert_create_exit:

    // Clean-up on error:
    if (!NT_SUCCESS(status))
    {
        context->state = WINDIVERT_CONTEXT_STATE_INVALID;
        if (context->read_queue != NULL)
        {
            WdfObjectDelete(context->read_queue);
        }
        if (context->timer != NULL)
        {
            WdfObjectDelete(context->timer);
        }
        if (context->engine_handle != NULL)
        {
            FwpmEngineClose0(context->engine_handle);
        }
        if (context->read_thread != NULL)
        {
            KeSetEvent(&context->read_event, IO_NO_INCREMENT, FALSE);
            KeWaitForSingleObject(context->read_thread, Executive,
                KernelMode, FALSE, NULL);
            ObDereferenceObject(context->read_thread);
        }
    }

    WdfRequestComplete(request, status);
}

/*
 * Register all WFP callouts.
 */
static NTSTATUS windivert_register_callouts(context_t context, BOOL is_inbound,
    BOOL is_outbound, BOOL is_ipv4, BOOL is_ipv6)
{
    UINT8 i, j;
    layer_t layers[WINDIVERT_CONTEXT_MAXLAYERS];
    NTSTATUS status;

    i = 0;
    switch (context->layer)
    {
        case WINDIVERT_LAYER_NETWORK:
            if (is_inbound && is_ipv4)
            {
                layers[i++] = layer_inbound_network_ipv4;
            }
            if (is_outbound && is_ipv4)
            {
                layers[i++] = layer_outbound_network_ipv4;
            }
            if (is_inbound && is_ipv6)
            {
                layers[i++] = layer_inbound_network_ipv6;
            }
            if (is_outbound && is_ipv6)
            {
                layers[i++] = layer_outbound_network_ipv6;
            }
            break;

        case WINDIVERT_LAYER_NETWORK_FORWARD:
            if (is_ipv4)
            {
                layers[i++] = layer_forward_network_ipv4;
            }
            if (is_ipv6)
            {
                layers[i++] = layer_forward_network_ipv6;
            }
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    status = FwpmTransactionBegin0(context->engine_handle, 0);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to begin WFP transaction", status);
        goto windivert_register_callouts_exit;
    }
    for (j = 0; j < i; j++)
    {
        status = windivert_register_callout(context, j, layers[j]);
        if (!NT_SUCCESS(status))
        {
            FwpmTransactionAbort0(context->engine_handle);
            goto windivert_register_callouts_exit;
        }
    }
    status = FwpmTransactionCommit0(context->engine_handle);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to commit WFP transaction", status);
        goto windivert_register_callouts_exit;
    }

windivert_register_callouts_exit:

    if (!NT_SUCCESS(status))
    {
        for (j = 0; j < i; j++)
        {
            if (context->registered[j])
            {
                FwpsCalloutUnregisterByKey0(&context->callout_guid[j]);
                context->registered[j] = FALSE;
            }
        }
    }

    return status;
}

/*
 * Register a WFP callout.
 */
static NTSTATUS windivert_register_callout(context_t context, UINT idx,
    layer_t layer)
{
    FWPM_SUBLAYER0 sublayer;
    FWPS_CALLOUT0 scallout;
    FWPM_CALLOUT0 mcallout;
    FWPM_FILTER0 filter;
    BOOL registered = FALSE;
    NTSTATUS status;

    RtlZeroMemory(&sublayer, sizeof(sublayer));
    sublayer.subLayerKey             = context->sublayer_guid[idx];
    sublayer.displayData.name        = layer->sublayer_name;
    sublayer.displayData.description = layer->sublayer_desc;
    sublayer.weight                  =
       (UINT16)(WINDIVERT_PRIORITY_MAX - context->priority);
    RtlZeroMemory(&scallout, sizeof(scallout));
    scallout.calloutKey              = context->callout_guid[idx];
    scallout.classifyFn              = layer->callout;
    scallout.notifyFn                = windivert_notify_callout;
    scallout.flowDeleteFn            = NULL;
    RtlZeroMemory(&mcallout, sizeof(mcallout));
    mcallout.calloutKey              = context->callout_guid[idx];
    mcallout.displayData.name        = layer->callout_name;
    mcallout.displayData.description = layer->callout_desc;
    mcallout.applicableLayer         = layer->guid;
    RtlZeroMemory(&filter, sizeof(filter));
    filter.filterKey                 = context->filter_guid[idx];
    filter.layerKey                  = layer->guid;
    filter.displayData.name          = layer->filter_name;
    filter.displayData.description   = layer->filter_desc;
    filter.action.type               = FWP_ACTION_CALLOUT_TERMINATING;
    filter.action.calloutKey         = context->callout_guid[idx];
    filter.subLayerKey               = context->sublayer_guid[idx];
    filter.weight.type               = FWP_EMPTY;
    filter.rawContext                = (UINT64)context;
    status = FwpmSubLayerAdd0(context->engine_handle, &sublayer, NULL);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to add WFP sub-layer", status);
        goto windivert_register_callout_error;
    }
    status = FwpsCalloutRegister0(WdfDeviceWdmGetDeviceObject(context->device),
        &scallout, NULL);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to register WFP callout", status);
        goto windivert_register_callout_error;
    }
    registered = TRUE;
    status = FwpmCalloutAdd0(context->engine_handle, &mcallout, NULL, NULL);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to add WFP callout", status);
        goto windivert_register_callout_error;
    }
    status = FwpmFilterAdd0(context->engine_handle, &filter, NULL, NULL);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to add WFP filter", status);
        goto windivert_register_callout_error;
    }
    context->registered[idx] = TRUE;

    return STATUS_SUCCESS;

windivert_register_callout_error:
    if (registered)
    {
        FwpsCalloutUnregisterByKey0(&context->callout_guid[idx]);
    }
    return status;
}

/*
 * WinDivert old-packet cleanup routine.
 */
extern VOID windivert_timer(IN WDFTIMER timer)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    PLIST_ENTRY entry;
    WDFFILEOBJECT object = (WDFFILEOBJECT)WdfTimerGetParentObject(timer);
    context_t context = windivert_context_get(object);
    packet_t packet;

    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_OPEN))
    {
        return;
    }

    // DEBUG("TIMER (context=%p, ticktock=%u)", context,
    //     context->timer_ticktock);

    // Sweep away old packets.
    KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    while (!IsListEmpty(&context->packet_queue))
    {
        entry = RemoveHeadList(&context->packet_queue);
        packet = CONTAINING_RECORD(entry, struct packet_s, entry);
        if (packet->timer_ticktock == context->timer_ticktock)
        {
            InsertHeadList(&context->packet_queue, entry);
            break;
        }
        context->packet_queue_length--;
        KeReleaseInStackQueuedSpinLock(&lock_handle);

        // Packet is old, dispose of it.
        DEBUG("TIMEOUT (context=%p, packet=%p)", context, packet);
        windivert_free_packet(packet);
        KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    }

    KeReleaseInStackQueuedSpinLock(&lock_handle);
    context->timer_ticktock = !context->timer_ticktock;

    // Restart the timer.
    WdfTimerStart(context->timer,
        WDF_REL_TIMEOUT_IN_MS(context->timer_timeout));
}

/*
 * Divert cleanup routine.
 */
extern VOID windivert_cleanup(IN WDFFILEOBJECT object)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    PLIST_ENTRY entry;
    UINT i;
    context_t context = windivert_context_get(object);
    packet_t packet;
    NTSTATUS status;
    
    DEBUG("CLEANUP: cleaning up WinDivert context (context=%p)", context);
    
    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_OPEN))
    {
        return;
    }
    WdfTimerStop(context->timer, TRUE);
    KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    context->state = WINDIVERT_CONTEXT_STATE_CLOSING;
    KeSetEvent(&context->read_event, IO_NO_INCREMENT, FALSE);
    while (!IsListEmpty(&context->packet_queue))
    {
        entry = RemoveHeadList(&context->packet_queue);
        KeReleaseInStackQueuedSpinLock(&lock_handle);
        packet = CONTAINING_RECORD(entry, struct packet_s, entry);
        windivert_free_packet(packet);
        KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    }
    KeReleaseInStackQueuedSpinLock(&lock_handle);
    WdfIoQueuePurge(context->read_queue, NULL, NULL);
    WdfObjectDelete(context->read_queue);
    WdfObjectDelete(context->timer);

    status = FwpmTransactionBegin0(context->engine_handle, 0);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to begin WFP transaction", status);
        goto windivert_cleanup_exit;
    }
    for (i = 0; i < WINDIVERT_CONTEXT_MAXLAYERS; i++)
    {
	    if (!context->registered[i])
	    {
	        continue;
	    }
        status = FwpmFilterDeleteByKey0(context->engine_handle,
            context->filter_guid+i);
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed delete WFP filter", status);
            FwpmTransactionAbort0(context->engine_handle);
            goto windivert_cleanup_exit;
        }
        status = FwpmSubLayerDeleteByKey0(context->engine_handle,
            context->sublayer_guid+i);
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed delete WFP sub-layer", status);
            FwpmTransactionAbort0(context->engine_handle);
            goto windivert_cleanup_exit;
        }
    }
    status = FwpmTransactionCommit0(context->engine_handle);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to commit WFP transaction", status);
        goto windivert_cleanup_exit;
    }

windivert_cleanup_exit:
    FwpmEngineClose0(context->engine_handle);
    for (i = 0; i < WINDIVERT_CONTEXT_MAXLAYERS; i++)
    {
        if (context->registered[i])
        {
            FwpsCalloutUnregisterByKey0(&context->callout_guid[i]);
        }
    }
    if (context->filter != NULL)
    {   
        ExFreePoolWithTag(context->filter, WINDIVERT_FILTER_TAG);
        context->filter = NULL;
    }
    KeWaitForSingleObject(context->read_thread, Executive, KernelMode, FALSE,
        NULL);
    ObDereferenceObject(context->read_thread);
}

/*
 * WinDivert close routine.
 */
extern VOID windivert_close(IN WDFFILEOBJECT object)
{
    context_t context = windivert_context_get(object);
    
    DEBUG("CLOSE: closing WinDivert context (context=%p)", context);
    
    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_CLOSING))
    {
        return;
    }
    context->state = WINDIVERT_CONTEXT_STATE_CLOSED;
}

/*
 * WinDivert read routine.
 */
static NTSTATUS windivert_read(context_t context, WDFREQUEST request)
{
    NTSTATUS status = STATUS_SUCCESS;

    DEBUG("READ: reading diverted packet (context=%p, request=%p)", context,
        request);

    // Forward the request to the pending read queue:
    status = WdfRequestForwardToIoQueue(request, context->read_queue);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to forward I/O request to read queue", status);
        return status;
    }

    // Service the read request:
    windivert_read_service(context);

    return STATUS_SUCCESS;
}

/*
 * Handle windivert_read_service worker.
 */
static void windivert_read_service_worker(PVOID context_0)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    context_t context = (context_t)context_0;

    /*
     * NOTE: We cannot verify the context because we do not know what state
     *       it is in.
     */

    while (TRUE)
    {
        KeWaitForSingleObject(&context->read_event, Executive, KernelMode,
            FALSE, NULL);

        KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
        KeClearEvent(&context->read_event);
        if (context->state != WINDIVERT_CONTEXT_STATE_OPEN)
        {
            break;
        }
        KeReleaseInStackQueuedSpinLock(&lock_handle);

        // Service reads:
        windivert_read_service(context);
    }

    KeReleaseInStackQueuedSpinLock(&lock_handle);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * WinDivert read request service.
 */
static void windivert_read_service(context_t context)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    WDFREQUEST request;
    PLIST_ENTRY entry;
    PMDL dst_mdl;
    PVOID dst, src;
    ULONG dst_len, src_len;
    NTSTATUS status;
    packet_t packet;
    req_context_t req_context;
    windivert_addr_t addr;

    DEBUG("windivert_read_service");

    KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    while (context->state == WINDIVERT_CONTEXT_STATE_OPEN &&
           !IsListEmpty(&context->packet_queue))
    {
        status = WdfIoQueueRetrieveNextRequest(context->read_queue, &request);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        entry = RemoveHeadList(&context->packet_queue);
        context->packet_queue_length--;
        KeReleaseInStackQueuedSpinLock(&lock_handle);
        packet = CONTAINING_RECORD(entry, struct packet_s, entry);
        
        DEBUG("SERVICE: servicing read request (context=%p, request=%p, "
            "packet=%p)", context, request, packet);
        
        // We have now have a read request and a packet; service the read.
        status = WdfRequestRetrieveOutputWdmMdl(request, &dst_mdl);
        dst_len = 0;
        if (!NT_SUCCESS(status))
        {
            DEBUG_ERROR("failed to retrieve output MDL", status);
            goto windivert_read_service_complete;
        }
        dst = MmGetSystemAddressForMdlSafe(dst_mdl, NormalPagePriority);
        if (dst == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            DEBUG_ERROR("failed to get address of output MDL", status);
            goto windivert_read_service_complete;
        }
        dst_len = MmGetMdlByteCount(dst_mdl);
        src_len = NET_BUFFER_DATA_LENGTH(packet->buffer);
        dst_len = (src_len < dst_len? src_len: dst_len);
        src = NdisGetDataBuffer(packet->buffer, dst_len, NULL, 1, 0);
        if (src == NULL)
        {
            NdisGetDataBuffer(packet->buffer, dst_len, dst, 1, 0);
        }
        else
        {
            RtlCopyMemory(dst, src, dst_len);
        }
        
        // Write the address information.
        req_context = windivert_req_context_get(request);
        addr = req_context->addr;
        if (addr != NULL)
        {
            addr->IfIdx = packet->if_idx;
            addr->SubIfIdx = packet->sub_if_idx;
            addr->Direction = packet->direction;
        }

        // Compute the IP/TCP/UDP checksums here (if required).
        if ((context->flags & WINDIVERT_FLAG_NO_CHECKSUM) == 0)
        {
            windivert_update_checksums(dst, dst_len, packet->ip_checksum,
                packet->tcp_checksum, packet->udp_checksum);
        }

        status = STATUS_SUCCESS;

windivert_read_service_complete:
        windivert_free_packet(packet);
        if (NT_SUCCESS(status))
        {
            WdfRequestCompleteWithInformation(request, status, dst_len);
        }
        else
        {
            WdfRequestComplete(request, status);
        }
        KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    }
    KeReleaseInStackQueuedSpinLock(&lock_handle);
}

/*
 * WinDivert write routine.
 */
static NTSTATUS windivert_write(context_t context, WDFREQUEST request,
    windivert_addr_t addr)
{
    PMDL mdl = NULL;
    PVOID data;
    UINT data_len;
    struct iphdr *ip_header;
    BOOL isipv4;
    HANDLE handle;
    PNET_BUFFER_LIST buffers = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    DEBUG("WRITE: writing/injecting a packet (context=%p, request=%p)",
        context, request);

    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_OPEN))
    {
        status = STATUS_INVALID_DEVICE_STATE;
        goto windivert_write_exit;
    }

    if (addr->Direction != WINDIVERT_DIRECTION_INBOUND &&
        addr->Direction != WINDIVERT_DIRECTION_OUTBOUND)
    {
        status = STATUS_INVALID_PARAMETER;
        DEBUG_ERROR("failed to inject packet; invalid direction", status);
        goto windivert_write_exit;
    }

    status = WdfRequestRetrieveOutputWdmMdl(request, &mdl);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to retrieve input MDL", status);
        goto windivert_write_exit;
    }

    data = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (data == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        DEBUG_ERROR("failed to get MDL address", status);
        goto windivert_write_exit;
    }
    
    data_len = MmGetMdlByteCount(mdl);
    if (data_len < sizeof(struct iphdr))
    {
        status = STATUS_BUFFER_TOO_SMALL;
        DEBUG_ERROR("write buffer too small, cannot read ip header", status);
        goto windivert_write_exit;
    }

    ip_header = (struct iphdr *)data;
    switch (ip_header->Version)
    {
        case 4:
            isipv4 = TRUE;
            break;
        case 6:
            isipv4 = FALSE;
            break;
        default:
            status = STATUS_INVALID_PARAMETER;
            DEBUG_ERROR("failed to inject packet; not IPv4 nor IPv6", status);
            goto windivert_write_exit;
    }

    status = FwpsAllocateNetBufferAndNetBufferList0(pool_handle, 0, 0, mdl,
        0, data_len, &buffers);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to create NET_BUFFER_LIST for injected packet",
            status);
        goto windivert_write_exit;
    }

    handle = (isipv4? inject_handle: injectv6_handle);
    if (context->layer == WINDIVERT_LAYER_NETWORK_FORWARD)
    {
        status = FwpsInjectForwardAsync0(handle, (HANDLE)context->priority,
            0, (isipv4? AF_INET: AF_INET6), UNSPECIFIED_COMPARTMENT_ID,
            addr->IfIdx, buffers, windivert_inject_complete, (HANDLE)request);
    }
    else if (addr->Direction == WINDIVERT_DIRECTION_OUTBOUND)
    {
        status = FwpsInjectNetworkSendAsync0(handle,
            (HANDLE)context->priority, 0, UNSPECIFIED_COMPARTMENT_ID, buffers,
            windivert_inject_complete, (HANDLE)request);
    }
    else
    {
        status = FwpsInjectNetworkReceiveAsync0(handle, 
            (HANDLE)context->priority, 0, UNSPECIFIED_COMPARTMENT_ID,
            addr->IfIdx, addr->SubIfIdx, buffers, windivert_inject_complete,
            (HANDLE)request);
    }

windivert_write_exit:

    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to (re)inject packet", status);
        if (buffers != NULL)
        {
            FwpsFreeNetBufferList0(buffers);
        }
    }

    return status;
}

/*
 * WinDivert inject complete routine.
 */
static void NTAPI windivert_inject_complete(VOID *context,
    NET_BUFFER_LIST *buffers, BOOLEAN dispatch_level)
{
    WDFREQUEST request = (WDFREQUEST)context;
    PNET_BUFFER buffer;
    size_t length = 0;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(dispatch_level);

    DEBUG("COMPLETE: write/inject packet complete (request=%p)", request);

    buffer = NET_BUFFER_LIST_FIRST_NB(buffers);
    status = NET_BUFFER_LIST_STATUS(buffers);
    if (NT_SUCCESS(status))
    {
        length = NET_BUFFER_DATA_LENGTH(buffer);
    }
    else
    {
        DEBUG_ERROR("failed to inject packet", status);
    }
    FwpsFreeNetBufferList0(buffers);
    WdfRequestCompleteWithInformation(request, status, length);
}

/*
 * WinDivert caller context preprocessing.
 */
VOID windivert_caller_context(IN WDFDEVICE device, IN WDFREQUEST request)
{
    PCHAR inbuf;
    size_t inbuflen;
    WDF_REQUEST_PARAMETERS params;
    WDFMEMORY memobj;
    windivert_addr_t addr = NULL;
    windivert_ioctl_t ioctl;
    WDF_OBJECT_ATTRIBUTES attributes;
    req_context_t req_context = NULL;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(request, &params);

    if (params.Type != WdfRequestTypeDeviceControl)
    {
        goto windivert_caller_context_exit;
    }

    // Get and verify the input buffer.
    status = WdfRequestRetrieveInputBuffer(request, 0, &inbuf, &inbuflen);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to retrieve input buffer", status);
        goto windivert_caller_context_error;
    }

    if (inbuflen != sizeof(struct windivert_ioctl_s))
    {
        status = STATUS_INVALID_DEVICE_REQUEST;
        DEBUG_ERROR("input buffer not an ioctl message header", status);
        goto windivert_caller_context_error;
    }

    ioctl = (windivert_ioctl_t)inbuf;
    if (ioctl->version != WINDIVERT_IOCTL_VERSION ||
        ioctl->magic != WINDIVERT_IOCTL_MAGIC)
    {
        status = STATUS_INVALID_DEVICE_REQUEST;
        DEBUG_ERROR("input buffer contained a bad ioctl message header",
            status);
        goto windivert_caller_context_error;
    }

    // Probe and lock user buffers here (if required).
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, req_context_s);
    status = WdfObjectAllocateContext(request, &attributes, &req_context);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to allocate request context for ioctl", status);
        goto windivert_caller_context_error;
    }
    req_context->addr = NULL;
    if (ioctl->arg == (UINT64)NULL)
    {
        goto windivert_caller_context_exit;
    }
    switch (params.Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_WINDIVERT_RECV:
            status = WdfRequestProbeAndLockUserBufferForWrite(request,
                (PVOID)ioctl->arg, sizeof(struct windivert_addr_s), &memobj);
            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("invalid arg pointer for RECV ioctl", status);
                goto windivert_caller_context_error;
            }
            addr = (windivert_addr_t)WdfMemoryGetBuffer(memobj, NULL);
            break;

        case IOCTL_WINDIVERT_SEND:
            status = WdfRequestProbeAndLockUserBufferForRead(request,
                (PVOID)ioctl->arg, sizeof(struct windivert_addr_s), &memobj);
            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("invalid arg pointer for SEND ioctl", status);
                goto windivert_caller_context_error;
            }
            addr = (windivert_addr_t)WdfMemoryGetBuffer(memobj, NULL);
            break;

        case IOCTL_WINDIVERT_START_FILTER:
            status = STATUS_INVALID_DEVICE_REQUEST;
            DEBUG_ERROR("arg pointer is non-NULL for SET_FILTER ioctl",
                status);
            goto windivert_caller_context_error;

        case IOCTL_WINDIVERT_SET_LAYER:
        case IOCTL_WINDIVERT_SET_PRIORITY:
        case IOCTL_WINDIVERT_SET_FLAGS:
        case IOCTL_WINDIVERT_SET_PARAM:
        case IOCTL_WINDIVERT_GET_PARAM:
            break;
        
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            DEBUG_ERROR("failed to complete I/O control; invalid request",
                status);
            goto windivert_caller_context_error;
    }
    
    req_context->addr = addr;

windivert_caller_context_exit:

    status = WdfDeviceEnqueueRequest(device, request);
    
windivert_caller_context_error:    
    
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to enqueue request", status);
        WdfRequestComplete(request, status);
    }
}

/*
 * WinDivert I/O control.
 */
extern VOID windivert_ioctl(IN WDFQUEUE queue, IN WDFREQUEST request,
    IN size_t out_length, IN size_t in_length, IN ULONG code)
{
    PCHAR inbuf, outbuf;
    size_t inbuflen, outbuflen, filter_len;
    windivert_ioctl_t ioctl;
    windivert_ioctl_filter_t filter;
    windivert_addr_t addr;
    req_context_t req_context;
    NTSTATUS status = STATUS_SUCCESS;
    context_t context =
        windivert_context_get(WdfRequestGetFileObject(request));
    UINT64 value, *valptr;
    UNREFERENCED_PARAMETER(queue);

    DEBUG("IOCTL: I/O control request (context=%p)", context);

    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_OPEN))
    {
        status = STATUS_INVALID_DEVICE_STATE;
        goto windivert_ioctl_exit;
    }

    // Get the buffers and do sanity checks.
    status = WdfRequestRetrieveInputBuffer(request, 0, &inbuf, &inbuflen);
    if (!NT_SUCCESS(status))
    {
        DEBUG_ERROR("failed to retrieve input buffer", status);
        goto windivert_ioctl_exit;
    }
    switch (code)
    {
        case IOCTL_WINDIVERT_START_FILTER: case IOCTL_WINDIVERT_GET_PARAM:
            status = WdfRequestRetrieveOutputBuffer(request, 0, &outbuf,
                &outbuflen);
            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("failed to retrieve output buffer", status);
                goto windivert_ioctl_exit;
            }
            break;
        default:
            outbuf = NULL;
            outbuflen = 0;
            break;
    }

    // Handle the ioctl:
    switch (code)
    {
        case IOCTL_WINDIVERT_RECV:
            status = windivert_read(context, request);
            if (NT_SUCCESS(status))
            {
                return;
            }
            break;
        
        case IOCTL_WINDIVERT_SEND:
            
            req_context = windivert_req_context_get(request);
            addr = req_context->addr;
            status = windivert_write(context, request, addr);
            if (NT_SUCCESS(status))
            {
                return;
            }
            break;
        
        case IOCTL_WINDIVERT_START_FILTER:
        {
            BOOL is_inbound, is_outbound, is_ipv4, is_ipv6;

            if (InterlockedExchange(&context->filter_on, TRUE) == TRUE)
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("duplicate SET_FILTER ioctl", status);
                goto windivert_ioctl_exit;
            }

            context->layer = context->layer_0;
            context->flags = context->flags_0;
            context->priority = context->priority_0;

            filter = (windivert_ioctl_filter_t)outbuf;
            filter_len = outbuflen;
            context->filter = windivert_filter_compile(filter, filter_len);
            if (context->filter == NULL)
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("failed to compile filter", status);
                goto windivert_ioctl_exit;
            }

            if ((context->flags & WINDIVERT_FLAG_PASSTHRU) != 0)
            {
                // Passthru mode.
                is_inbound = is_outbound = is_ipv4 = is_ipv6 = FALSE;
            }
            else
            {
                windivert_filter_analyze(context->filter, &is_inbound,
                    &is_outbound, &is_ipv4, &is_ipv6);
            }
            status = windivert_register_callouts(context, is_inbound,
                is_outbound, is_ipv4, is_ipv6);

            // Start the timer.
            WdfTimerStart(context->timer,
                WDF_REL_TIMEOUT_IN_MS(context->timer_timeout));

            break;
        }

        case IOCTL_WINDIVERT_SET_LAYER:
            ioctl = (windivert_ioctl_t)inbuf;
            if (ioctl->arg > WINDIVERT_LAYER_MAX)
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("failed to set layer; value too big", status);
                goto windivert_ioctl_exit;
            }
            context->layer_0 = (UINT8)ioctl->arg;
            break;

        case IOCTL_WINDIVERT_SET_PRIORITY:
            ioctl = (windivert_ioctl_t)inbuf;
            if (ioctl->arg > WINDIVERT_PRIORITY_MAX)
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("failed to set priority; value too big", status);
                goto windivert_ioctl_exit;
            }
            context->priority_0 = (UINT16)ioctl->arg;
            break;

        case IOCTL_WINDIVERT_SET_FLAGS:
            ioctl = (windivert_ioctl_t)inbuf;
            if (!WINDIVERT_FLAGS_VALID(ioctl->arg))
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("failed to set flags; invalid flags value",
                    status);
                goto windivert_ioctl_exit;
            }
            context->flags_0 = ioctl->arg;
            break;

        case IOCTL_WINDIVERT_SET_PARAM:
            ioctl = (windivert_ioctl_t)inbuf;
            value = ioctl->arg;
            switch ((WINDIVERT_PARAM)ioctl->arg8)
            {
                case WINDIVERT_PARAM_QUEUE_LEN:
                    if (value < WINDIVERT_PARAM_QUEUE_LEN_MIN ||
                        value > WINDIVERT_PARAM_QUEUE_LEN_MAX)
                    {
                        status = STATUS_INVALID_DEVICE_REQUEST;
                        DEBUG_ERROR("failed to set queue length; invalid "
                            "value", status);
                        goto windivert_ioctl_exit;
                    }
                    context->packet_queue_maxlength = (ULONG)value;
                    break;

                case WINDIVERT_PARAM_QUEUE_TIME:
                    if (value < WINDIVERT_PARAM_QUEUE_TIME_MIN ||
                        value > WINDIVERT_PARAM_QUEUE_TIME_MAX)
                    {
                        status = STATUS_INVALID_DEVICE_REQUEST;
                        DEBUG_ERROR("failed to set queue time; invalid "
                            "value", status);
                        goto windivert_ioctl_exit;
                    }
                    context->timer_timeout = (UINT)value;
                    break;

                default:
                    status = STATUS_INVALID_DEVICE_REQUEST;
                    DEBUG_ERROR("failed to set parameter; invalid parameter",
                        status);
                    goto windivert_ioctl_exit;
            }
            break;

        case IOCTL_WINDIVERT_GET_PARAM:
            ioctl = (windivert_ioctl_t)inbuf;
            if (outbuflen != sizeof(UINT64))
            {
                status = STATUS_INVALID_DEVICE_REQUEST;
                DEBUG_ERROR("failed to get parameter; invalid output "
                    "buffer size", status);
                goto windivert_ioctl_exit;
            }
            valptr = (UINT64 *)outbuf;
            switch ((WINDIVERT_PARAM)ioctl->arg8)
            {
                case WINDIVERT_PARAM_QUEUE_LEN:
                    *valptr = context->packet_queue_maxlength;
                    break;
                case WINDIVERT_PARAM_QUEUE_TIME:
                    *valptr = context->timer_timeout;
                    break;
                default:
                    status = STATUS_INVALID_DEVICE_REQUEST;
                    DEBUG_ERROR("failed to get parameter; invalid parameter",
                        status);
                    goto windivert_ioctl_exit;
            }
            break;

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            DEBUG_ERROR("failed to complete I/O control; invalid request",
                status);
            break;
    }

windivert_ioctl_exit:
    WdfRequestComplete(request, status);
}

/*
 * WinDivert notify callout.
 */
static NTSTATUS windivert_notify_callout(IN FWPS_CALLOUT_NOTIFY_TYPE type,
    IN const GUID *filter_key, IN const FWPS_FILTER0 *filter)
{
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(filter_key);
    UNREFERENCED_PARAMETER(filter);
    return STATUS_SUCCESS;
}

/*
 * WinDivert classify outbound IPv4 callout.
 */
static void windivert_classify_outbound_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    windivert_classify_callout(WINDIVERT_DIRECTION_OUTBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_OUTBOUND_IPPACKET_V4_INTERFACE_INDEX].value.uint32,
        fixed_vals->incomingValue[
            FWPS_FIELD_OUTBOUND_IPPACKET_V4_SUB_INTERFACE_INDEX].value.uint32,
        TRUE, fixed_vals, meta_vals, data, filter, flow_context, result);
}

/*
 * WinDivert classify outbound IPv6 callout.
 */
static void windivert_classify_outbound_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    windivert_classify_callout(WINDIVERT_DIRECTION_OUTBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_OUTBOUND_IPPACKET_V6_INTERFACE_INDEX].value.uint32,
        fixed_vals->incomingValue[
            FWPS_FIELD_OUTBOUND_IPPACKET_V6_SUB_INTERFACE_INDEX].value.uint32,
        FALSE, fixed_vals, meta_vals, data, filter, flow_context, result);
}

/*
 * WinDivert classify inbound IPv4 callout.
 */
static void windivert_classify_inbound_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    PNET_BUFFER_LIST buffers = (PNET_BUFFER_LIST)data;
    PNET_BUFFER buffer;
    NTSTATUS status;

    if (!(result->rights & FWPS_RIGHT_ACTION_WRITE) || data == NULL)
    {
        return;
    }

    buffer = NET_BUFFER_LIST_FIRST_NB(buffers);
    status = NdisRetreatNetBufferDataStart(buffer, meta_vals->ipHeaderSize,
        0, NULL);
    if (!NT_SUCCESS(status))
    {
        result->actionType = FWP_ACTION_PERMIT;
        return;
    }
    windivert_classify_callout(WINDIVERT_DIRECTION_INBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_INBOUND_IPPACKET_V4_INTERFACE_INDEX].value.uint32,
        fixed_vals->incomingValue[
            FWPS_FIELD_INBOUND_IPPACKET_V4_SUB_INTERFACE_INDEX].value.uint32,
        TRUE, fixed_vals, meta_vals, data, filter, flow_context, result);
    if (result->actionType != FWP_ACTION_BLOCK)
    {
        NdisAdvanceNetBufferDataStart(buffer, meta_vals->ipHeaderSize,
            FALSE, NULL);
    }
}

/*
 * WinDivert classify inbound IPv6 callout.
 */
static void windivert_classify_inbound_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    PNET_BUFFER_LIST buffers = (PNET_BUFFER_LIST)data;
    PNET_BUFFER buffer;
    NTSTATUS status;
   
    if (!(result->rights & FWPS_RIGHT_ACTION_WRITE) || data == NULL)
    {
        return;
    }

    buffer = NET_BUFFER_LIST_FIRST_NB(buffers);
    status = NdisRetreatNetBufferDataStart(buffer, sizeof(struct ipv6hdr),
        0, NULL);
    if (!NT_SUCCESS(status))
    {
        result->actionType = FWP_ACTION_PERMIT;
        return;
    }
    windivert_classify_callout(WINDIVERT_DIRECTION_INBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_INBOUND_IPPACKET_V6_INTERFACE_INDEX].value.uint32,
        fixed_vals->incomingValue[
            FWPS_FIELD_INBOUND_IPPACKET_V6_SUB_INTERFACE_INDEX].value.uint32,
        FALSE, fixed_vals, meta_vals, data, filter, flow_context, result);
    if (result->actionType != FWP_ACTION_BLOCK)
    {
        NdisAdvanceNetBufferDataStart(buffer, sizeof(struct ipv6hdr), FALSE,
            NULL);
    }
}

/*
 * WinDivert classify forward IPv4 callout.
 */
static void windivert_classify_forward_network_v4_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    windivert_classify_callout(WINDIVERT_DIRECTION_OUTBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_IPFORWARD_V4_DESTINATION_INTERFACE_INDEX].value.uint32,
        0, TRUE, fixed_vals, meta_vals, data, filter, flow_context, result);
}

/*
 * WinDivert classify forward IPv6 callout.
 */
static void windivert_classify_forward_network_v6_callout(
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    windivert_classify_callout(WINDIVERT_DIRECTION_OUTBOUND,
        fixed_vals->incomingValue[
            FWPS_FIELD_IPFORWARD_V6_DESTINATION_INTERFACE_INDEX].value.uint32,
        0, FALSE, fixed_vals, meta_vals, data, filter, flow_context, result);
}


/*
 * WinDivert classify callout.
 */
static void windivert_classify_callout(IN UINT8 direction, IN UINT32 if_idx,
    IN UINT32 sub_if_idx, IN BOOL isipv4,
    IN const FWPS_INCOMING_VALUES0 *fixed_vals,
    IN const FWPS_INCOMING_METADATA_VALUES0 *meta_vals, IN OUT void *data,
    const FWPS_FILTER0 *filter, IN UINT64 flow_context,
    OUT FWPS_CLASSIFY_OUT0 *result)
{
    FWPS_PACKET_INJECTION_STATE packet_state;
    HANDLE packet_context;
    UINT32 priority;
    PNET_BUFFER_LIST buffers, buffers_fst, buffers_itr;
    PNET_BUFFER buffer;
    BOOL outbound;
    context_t context;
    packet_t packet;

    // Basic checks:
    if (!(result->rights & FWPS_RIGHT_ACTION_WRITE) || data == NULL)
    {
        return;
    }

    context = (context_t)filter->context;
    buffers = (PNET_BUFFER_LIST)data;
    if (isipv4)
    {
        packet_state = FwpsQueryPacketInjectionState0(inject_handle, buffers,
            &packet_context);
    }
    else
    {
        packet_state = FwpsQueryPacketInjectionState0(injectv6_handle,
            buffers, &packet_context);
    }
    if (!windivert_context_verify(context, WINDIVERT_CONTEXT_STATE_OPEN) ||
        (context->flags & WINDIVERT_FLAG_PASSTHRU) != 0)
    {
        result->actionType = FWP_ACTION_PERMIT;
        return;
    }
    if (packet_state == FWPS_PACKET_INJECTED_BY_SELF ||
        packet_state == FWPS_PACKET_PREVIOUSLY_INJECTED_BY_SELF)
    {
        priority = (UINT32)packet_context;
        if (priority <= context->priority)
        {
            result->actionType = FWP_ACTION_PERMIT;
            return;
        }
    }
    else
    {
        priority = 0;
    }

    /*
     * This code is complicated by the fact the a single NET_BUFFER_LIST
     * may contain several NET_BUFFER structures.  Each NET_BUFFER needs to
     * be filtered independently.  To achieve this we do the following:
     * 1) First check if any NET_BUFFER passes the filter.
     * 2) If no, then CONTINUE the entire NET_BUFFER_LIST.
     * 3) Else, split the NET_BUFFER_LIST into individual NET_BUFFERs; and
     *    either queue or re-inject based on the filter.
     */

    // Find the first NET_BUFFER we need to queue:
    buffers_fst = buffers;
    outbound = (direction == WINDIVERT_DIRECTION_OUTBOUND);
    do
    {
        buffer = NET_BUFFER_LIST_FIRST_NB(buffers_fst);
        if (windivert_filter(buffer, if_idx, sub_if_idx, outbound,
            context->filter))
        {
            break;
        }
        buffers_fst= NET_BUFFER_LIST_NEXT_NBL(buffers_fst);
    }
    while (buffers_fst != NULL);

    // No NET_BUFFER needs to be queued, permit the entire NET_BUFFER_LIST:
    if (buffers_fst == NULL)
    {
        result->actionType = FWP_ACTION_PERMIT;
        return;
    }

    if ((context->flags & WINDIVERT_FLAG_SNIFF) == 0)
    {
        // Re-inject all packets up to 'buffers_fst'
        buffers_itr = buffers;
        while (buffers_itr != buffers_fst)
        {
            buffer = NET_BUFFER_LIST_FIRST_NB(buffers_itr);
            if (!windivert_reinject_packet(context, direction, isipv4, if_idx,
                    sub_if_idx, priority, buffers, buffer))
            {
                goto windivert_classify_callout_exit;
            }
            buffers_itr = NET_BUFFER_LIST_NEXT_NBL(buffers_itr);
        }
    }
    else
    {
        buffers_itr = buffers_fst;
    }

    // Queue buffers_itr = buffers_fst, which matched our filter.
    buffer = NET_BUFFER_LIST_FIRST_NB(buffers_itr);
    if (!windivert_queue_packet(context, buffers, buffer, direction, if_idx,
            sub_if_idx))
    {
        goto windivert_classify_callout_exit;
    }
    buffers_itr = NET_BUFFER_LIST_NEXT_NBL(buffers_itr);

    // Queue or re-inject remaining packets.
    while (buffers_itr != NULL)
    {
        buffer = NET_BUFFER_LIST_FIRST_NB(buffers_itr);
        if (windivert_filter(buffer, if_idx, sub_if_idx, outbound,
            context->filter))
        {
            if (!windivert_queue_packet(context, buffers, buffer, direction,
                    if_idx, sub_if_idx))
            {
                goto windivert_classify_callout_exit;
            }
        }
        else if ((context->flags & WINDIVERT_FLAG_SNIFF) == 0)
        {
            if (!windivert_reinject_packet(context, direction, isipv4, if_idx,
                    sub_if_idx, priority, buffers, buffer))
            {
                goto windivert_classify_callout_exit;
            }
        }
    }

    // Since new packets have been queued, service any read.
    if ((context->flags & WINDIVERT_FLAG_DROP) == 0)
    {
        KeSetEvent(&context->read_event, IO_NO_INCREMENT, FALSE);
    }

windivert_classify_callout_exit:

    if ((context->flags & WINDIVERT_FLAG_SNIFF) != 0)
    {
        result->actionType = FWP_ACTION_PERMIT;
    }
    else
    {
        result->actionType = FWP_ACTION_BLOCK;
        result->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB;
        result->rights &= ~FWPS_RIGHT_ACTION_WRITE;
    }
}

/*
 * Queue a NET_BUFFER.
 */
static BOOL windivert_queue_packet(context_t context, PNET_BUFFER_LIST buffers,
    PNET_BUFFER buffer, UINT8 direction, UINT32 if_idx, UINT32 sub_if_idx)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO checksum_info;
    PLIST_ENTRY entry;
    packet_t packet;
    NTSTATUS status;

    if ((context->flags & WINDIVERT_FLAG_DROP) != 0)
    {
        return TRUE;
    }

    packet = (packet_t)ExAllocatePoolWithTag(NonPagedPool,
        WINDIVERT_PACKET_SIZE, WINDIVERT_PACKET_TAG);
    if (packet == NULL)
    {
        return FALSE;
    }

    if ((context->flags & WINDIVERT_FLAG_SNIFF) != 0)
    {
        // Clone the buffer
        status = FwpsAllocateNetBufferAndNetBufferList0(
            pool_handle, 0, 0, NET_BUFFER_FIRST_MDL(buffer),
            NET_BUFFER_DATA_OFFSET(buffer), NET_BUFFER_DATA_LENGTH(buffer),
            &packet->clone);
        if (!NT_SUCCESS(status))
        {
            ExFreePoolWithTag(packet, WINDIVERT_PACKET_TAG);
            return FALSE;
        }
        buffer = NET_BUFFER_LIST_FIRST_NB(packet->clone);
    }
    else
    {
        packet->clone = NULL;
    }
    
    checksum_info.Value = NET_BUFFER_LIST_INFO(buffers,
        TcpIpChecksumNetBufferListInfo);
    packet->buffer = buffer;
    packet->buffers = buffers;
    packet->direction = direction;
    packet->if_idx = if_idx;
    packet->sub_if_idx = sub_if_idx;
    if (direction == WINDIVERT_DIRECTION_OUTBOUND)
    {
        // IPv4 Checksum is not calculated yet
        packet->ip_checksum = TRUE;
        packet->tcp_checksum = (BOOL)checksum_info.Transmit.TcpChecksum;
        packet->udp_checksum = (BOOL)checksum_info.Transmit.UdpChecksum;
    }
    else
    {
        packet->ip_checksum = FALSE;
        packet->tcp_checksum = FALSE;
        packet->udp_checksum = FALSE;
    }
    packet->timer_ticktock = context->timer_ticktock;
    entry = &packet->entry;
    FwpsReferenceNetBufferList0(buffers, FALSE);
    KeAcquireInStackQueuedSpinLock(&context->lock, &lock_handle);
    if (context->state != WINDIVERT_CONTEXT_STATE_OPEN)
    {
        // We are no longer open
        KeReleaseInStackQueuedSpinLock(&lock_handle);
        windivert_free_packet(packet);
        return FALSE;
    }
    InsertTailList(&context->packet_queue, entry);
    entry = NULL;
    context->packet_queue_length++;
    if (context->packet_queue_length > context->packet_queue_maxlength)
    {
        entry = RemoveHeadList(&context->packet_queue);
        context->packet_queue_length--;
    }
    KeReleaseInStackQueuedSpinLock(&lock_handle);
    if (entry != NULL)
    {
        // Queue is full; 'entry' contains a dropped packet.
        DEBUG("DROP: packet queue is full, dropping packet");
        packet = CONTAINING_RECORD(entry, struct packet_s, entry);
        windivert_free_packet(packet);
    }
    DEBUG("PACKET: diverting packet (packet=%p)", packet);

    return TRUE;
}

/*
 * Free a packet.
 */
static void windivert_free_packet(packet_t packet)
{
    FwpsDereferenceNetBufferList0(packet->buffers, FALSE);
    if (packet->clone != NULL)
    {
        FwpsFreeNetBufferList0(packet->clone);
    }
    ExFreePoolWithTag(packet, WINDIVERT_PACKET_TAG);
}

/*
 * Re-inject a NET_BUFFER.
 */
static BOOL windivert_reinject_packet(context_t context, UINT8 direction,
    BOOL isipv4, UINT32 if_idx, UINT32 sub_if_idx, UINT32 priority,
    PNET_BUFFER_LIST buffers, PNET_BUFFER buffer)
{
    PNET_BUFFER_LIST buffers_cpy;
    HANDLE handle;
    NTSTATUS status;

    status = FwpsAllocateNetBufferAndNetBufferList0(
        pool_handle, 0, 0, NET_BUFFER_FIRST_MDL(buffer),
        NET_BUFFER_DATA_OFFSET(buffer), NET_BUFFER_DATA_LENGTH(buffer),
        &buffers_cpy);
    if (!NT_SUCCESS(status))
    {
        return FALSE;
    }
    FwpsReferenceNetBufferList0(buffers, FALSE);
    handle = (isipv4? inject_handle: injectv6_handle);
    if (context->layer == WINDIVERT_LAYER_NETWORK_FORWARD)
    {
        status = FwpsInjectForwardAsync0(handle, (HANDLE)priority, 0,
            (isipv4? AF_INET: AF_INET6), UNSPECIFIED_COMPARTMENT_ID,
            if_idx, buffers_cpy, windivert_reinject_complete, (HANDLE)buffers);
    }   
    else if (direction == WINDIVERT_DIRECTION_OUTBOUND)
    {
        status = FwpsInjectNetworkSendAsync0(handle, (HANDLE)priority, 0,
            UNSPECIFIED_COMPARTMENT_ID, buffers_cpy,
            windivert_reinject_complete, (HANDLE)buffers);
    }
    else
    {
        // NOTE: this case should never occur since inbound net buffers only
        //       ever contain one packet.  We keep for completeness.
        status = FwpsInjectNetworkReceiveAsync0(handle, (HANDLE)priority, 0,
            UNSPECIFIED_COMPARTMENT_ID, if_idx, sub_if_idx, buffers_cpy,
            windivert_reinject_complete, (HANDLE)buffers);
    }
    if (!NT_SUCCESS(status))
    {
        FwpsDereferenceNetBufferList0(buffers, FALSE);
        FwpsFreeNetBufferList0(buffers_cpy);
        return FALSE;
    }
    return TRUE;
}

/*
 * WinDivert (re)inject complete.
 */
static void NTAPI windivert_reinject_complete(VOID *context,
    NET_BUFFER_LIST *buffers_cpy, BOOLEAN dispatch_level)
{
    PNET_BUFFER_LIST buffers;
    UNREFERENCED_PARAMETER(dispatch_level);

    buffers = (PNET_BUFFER_LIST)context;
    FwpsDereferenceNetBufferList0(buffers, FALSE);
    FwpsFreeNetBufferList0(buffers_cpy);
}

/*
 * Generic checksum calculation.
 */
static UINT16 windivert_checksum(const void *pseudo_header,
    size_t pseudo_header_len, const void *data, size_t len)
{
    register const UINT16 *data16 = (const UINT16 *)pseudo_header;
    register size_t len16 = pseudo_header_len >> 1;
    register UINT32 sum = 0;
    size_t i;

    for (i = 0; i < len16; i++)
    {
        sum += (UINT32)data16[i];
    }

    data16 = (const UINT16 *)data;
    len16 = len >> 1;
    for (i = 0; i < len16; i++)
    {
        sum += (UINT32)data16[i];
    }
    
    if (len & 0x1)
    {
        const UINT8 *data8 = (const UINT8 *)data;
        sum += (UINT32)data8[len-1];
    }

    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += (sum >> 16);
    sum = ~sum;
    return (UINT16)sum;
}

/*
 * Given a well-formed packet, update the IP and/or TCP/UDP checksums if
 * required.
 */
static void windivert_update_checksums(void *header, size_t len,
    BOOL update_ip, BOOL update_tcp, BOOL update_udp)
{
    struct
    {
        UINT32 SrcAddr;
        UINT32 DstAddr;
        UINT8  Zero;
        UINT8  Protocol;
        UINT16 TransLength;
    } pseudo_header;
    struct iphdr *ip_header = (struct iphdr *)header;
    size_t ip_header_len, trans_len;
    void *trans_header;
    struct tcphdr *tcp_header;
    struct udphdr *udp_header;
    UINT16 *trans_check_ptr;
    UINT sum;

    if (!update_ip && !update_tcp && !update_udp)
    {
        return;
    }

    if (len < sizeof(struct iphdr))
    {
        return;
    }

    if (ip_header->Version != 4)
    {
        return;
    }

    ip_header_len = ip_header->HdrLength*sizeof(UINT32);
    if (len < ip_header_len)
    {
        return;
    }

    if (update_ip)
    {
        ip_header->Checksum = 0;
        ip_header->Checksum = windivert_checksum(NULL, 0, ip_header,
            ip_header_len);
    }

    trans_len = RtlUshortByteSwap(ip_header->Length) - ip_header_len;
    trans_header = (UINT8 *)ip_header + ip_header_len;
    switch (ip_header->Protocol)
    {
        case IPPROTO_TCP:
            if (!update_tcp)
            {
                return;
            }
            tcp_header = (struct tcphdr *)trans_header;
            if (trans_len < sizeof(struct tcphdr))
            {
                return;
            }
            trans_check_ptr = &tcp_header->Checksum;
            break;
        case IPPROTO_UDP:
            if (!update_udp)
            {
                return;
            }
            udp_header = (struct udphdr *)trans_header;
            if (trans_len < sizeof(struct udphdr))
            {
                return;
            }
            trans_check_ptr = &udp_header->Checksum;
            break;
        default:
            return;
    }

    pseudo_header.SrcAddr     = ip_header->SrcAddr;
    pseudo_header.DstAddr     = ip_header->DstAddr;
    pseudo_header.Zero        = 0x0;
    pseudo_header.Protocol    = ip_header->Protocol;
    pseudo_header.TransLength = RtlUshortByteSwap((UINT16)trans_len);
    *trans_check_ptr = 0x0;
    sum = windivert_checksum(&pseudo_header, sizeof(pseudo_header),
        trans_header, trans_len);
    if (sum == 0 && ip_header->Protocol == IPPROTO_UDP)
    {
        *trans_check_ptr = 0xFFFF;
    }
    else
    {
        *trans_check_ptr = (UINT16)sum;
    }
}

/*
 * Checks if the given packet is of interest.
 */
static BOOL windivert_filter(PNET_BUFFER buffer, UINT32 if_idx,
    UINT32 sub_if_idx, BOOL outbound, filter_t filter)
{
    // Buffer contains enough space for a full size iphdr and tcphdr/udphdr
    // (without options)
    UINT8 storage[0xF*sizeof(UINT32) + sizeof(struct tcphdr)];
    UINT8 *headers;
    size_t tot_len, cpy_len, ip_header_len;
    struct iphdr *ip_header = NULL;
    struct ipv6hdr *ipv6_header = NULL;
    struct icmphdr *icmp_header = NULL;
    struct icmpv6hdr *icmpv6_header = NULL;
    struct tcphdr *tcp_header = NULL;
    struct udphdr *udp_header = NULL;
    UINT16 ip, ttl;
    UINT8 protocol;

    // Parse the headers:
    tot_len = NET_BUFFER_DATA_LENGTH(buffer);
    if (tot_len < sizeof(struct iphdr))
    {
        DEBUG("FILTER: REJECT (packet length too small)");
        return FALSE;
    }
    cpy_len = (tot_len < sizeof(storage)? tot_len: sizeof(storage));
    headers = (UINT8 *)NdisGetDataBuffer(buffer, (ULONG)cpy_len, storage, 1,
        0);
    if (headers == NULL)
    {
        headers = storage;
    }

    ip_header = (struct iphdr *)headers;
    switch (ip_header->Version)
    {
        case 4:
            ip_header_len = ip_header->HdrLength*sizeof(UINT32);
            if (RtlUshortByteSwap(ip_header->Length) != tot_len ||
                ip_header->HdrLength < 5 ||
                ip_header_len > tot_len)
            {
                DEBUG("FILTER: REJECT (bad IPv4 packet)");
                return FALSE;
            }
            protocol = ip_header->Protocol;
            break;
        case 6:
            ip_header = NULL;
            ipv6_header = (struct ipv6hdr *)headers;
            ip_header_len = sizeof(struct ipv6hdr);
            if (ip_header_len > tot_len ||
                RtlUshortByteSwap(ipv6_header->Length) +
                    sizeof(struct ipv6hdr) != tot_len)
            {
                DEBUG("FILTER: REJECT (bad IPv6 packet)");
                return FALSE;
            }
            protocol = ipv6_header->NextHdr;
            break;
        default:
            DEBUG("FILTER: REJECT (packet is neither IPv4 nor IPv6)");
            return FALSE;
    }

    switch (protocol)
    {
        case IPPROTO_ICMP:
            icmp_header = (struct icmphdr *)(headers + ip_header_len);
            if (ip_header == NULL ||
                sizeof(struct icmphdr) + ip_header_len > tot_len)
            {
                DEBUG("FILTER: REJECT (bad ICMP packet)");
                return FALSE;
            }
            break;
        case IPPROTO_ICMPV6:
            icmpv6_header = (struct icmpv6hdr *)(headers + ip_header_len);
            if (ipv6_header == NULL ||
                sizeof(struct icmpv6hdr) + ip_header_len > tot_len)
            {
                DEBUG("FILTER: REJECT (bad ICMPV6 packet)");
                return FALSE;
            }
            break;
        case IPPROTO_TCP:
            tcp_header = (struct tcphdr *)(headers + ip_header_len);
            if (tcp_header->HdrLength < 5 ||
                tcp_header->HdrLength*sizeof(UINT32) + ip_header_len > tot_len)
            {
                DEBUG("FILTER: REJECT (bad TCP packet)");
                return FALSE;
            }
            break;
        case IPPROTO_UDP:
            udp_header = (struct udphdr *)(headers + ip_header_len);
            if (sizeof(struct udphdr) + ip_header_len > tot_len)
            {
                DEBUG("FILTER: REJECT (bad UDP packet)");
                return FALSE;
            }
            break;
        default:
            break;
    }

    // Execute the filter:
    ip = 0;
    ttl = WINDIVERT_FILTER_MAXLEN+1;       // Additional safety
    while (ttl-- != 0)
    {
        BOOL result;
        UINT32 field[4];
        field[1] = 0;
        field[2] = 0;
        field[3] = 0;
        switch (filter[ip].protocol)
        {
            case WINDIVERT_FILTER_PROTOCOL_NONE:
                result = TRUE;
                break;
            case WINDIVERT_FILTER_PROTOCOL_IP:
                result = (ip_header != NULL);
                break;
            case WINDIVERT_FILTER_PROTOCOL_IPV6:
                result = (ipv6_header != NULL);
                break;
            case WINDIVERT_FILTER_PROTOCOL_ICMP:
                result = (icmp_header != NULL);
                break;
            case WINDIVERT_FILTER_PROTOCOL_ICMPV6:
                result = (icmpv6_header != NULL);
                break;
            case WINDIVERT_FILTER_PROTOCOL_TCP:
                result = (tcp_header != NULL);
                break;
            case WINDIVERT_FILTER_PROTOCOL_UDP:
                result = (udp_header != NULL);
                break;
            default:
                result = FALSE;
                break;
        }
        if (result)
        {
            switch (filter[ip].field)
            {
                case WINDIVERT_FILTER_FIELD_ZERO:
                    field[0] = 0;
                    break;
                case WINDIVERT_FILTER_FIELD_INBOUND:
                    field[0] = (UINT32)(!outbound);
                    break;
                case WINDIVERT_FILTER_FIELD_OUTBOUND:
                    field[0] = (UINT32)outbound;
                    break;
                case WINDIVERT_FILTER_FIELD_IFIDX:
                    field[0] = (UINT32)if_idx;
                    break;
                case WINDIVERT_FILTER_FIELD_SUBIFIDX:
                    field[0] = (UINT32)sub_if_idx;
                    break;
                case WINDIVERT_FILTER_FIELD_IP:
                    field[0] = (UINT32)(ip_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6:
                    field[0] = (UINT32)(ipv6_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_ICMP:
                    field[0] = (UINT32)(icmp_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_ICMPV6:
                    field[0] = (UINT32)(icmpv6_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP:
                    field[0] = (UINT32)(tcp_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_UDP:
                    field[0] = (UINT32)(udp_header != NULL);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_HDRLENGTH:
                    field[0] = (UINT32)ip_header->HdrLength;
                    break;
                case WINDIVERT_FILTER_FIELD_IP_TOS:
                    field[0] = (UINT32)RtlUshortByteSwap(ip_header->TOS);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_LENGTH:
                    field[0] = (UINT32)RtlUshortByteSwap(ip_header->Length);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_ID:
                    field[0] = (UINT32)RtlUshortByteSwap(ip_header->Id);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_DF:
                    field[0] = (UINT32)IPHDR_GET_DF(ip_header);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_MF:
                    field[0] = (UINT32)IPHDR_GET_MF(ip_header);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_FRAGOFF:
                    field[0] = (UINT32)RtlUshortByteSwap(
                        IPHDR_GET_FRAGOFF(ip_header));
                    break;
                case WINDIVERT_FILTER_FIELD_IP_TTL:
                    field[0] = (UINT32)ip_header->TTL;
                    break;
                case WINDIVERT_FILTER_FIELD_IP_PROTOCOL:
                    field[0] = (UINT32)ip_header->Protocol;
                    break;
                case WINDIVERT_FILTER_FIELD_IP_CHECKSUM:
                    field[0] = (UINT32)RtlUshortByteSwap(ip_header->Checksum);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_SRCADDR:
                    field[0] = (UINT32)RtlUlongByteSwap(ip_header->SrcAddr);
                    break;
                case WINDIVERT_FILTER_FIELD_IP_DSTADDR:
                    field[0] = (UINT32)RtlUlongByteSwap(ip_header->DstAddr);
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_TRAFFICCLASS:
                    field[0] = (UINT32)IPV6HDR_GET_TRAFFICCLASS(ipv6_header);
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_FLOWLABEL:
                    field[0] = (UINT32)RtlUlongByteSwap(
                        IPV6HDR_GET_FLOWLABEL(ipv6_header));
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_LENGTH:
                    field[0] = (UINT32)RtlUshortByteSwap(ipv6_header->Length);
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_NEXTHDR:
                    field[0] = (UINT32)ipv6_header->NextHdr;
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_HOPLIMIT:
                    field[0] = (UINT32)ipv6_header->HopLimit;
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_SRCADDR:
                    field[0] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->SrcAddr[3]);
                    field[1] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->SrcAddr[2]);
                    field[2] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->SrcAddr[1]);
                    field[3] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->SrcAddr[0]);
                    break;
                case WINDIVERT_FILTER_FIELD_IPV6_DSTADDR:
                    field[0] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->DstAddr[3]);
                    field[1] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->DstAddr[2]);
                    field[2] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->DstAddr[1]);
                    field[3] =
                        (UINT32)RtlUlongByteSwap(ipv6_header->DstAddr[0]);
                    break;
                case WINDIVERT_FILTER_FIELD_ICMP_TYPE:
                    field[0] = (UINT32)icmp_header->Type;
                    break;
                case WINDIVERT_FILTER_FIELD_ICMP_CODE:
                    field[0] = (UINT32)icmp_header->Code;
                    break;
                case WINDIVERT_FILTER_FIELD_ICMP_CHECKSUM:
                    field[0] =
                        (UINT32)RtlUshortByteSwap(icmp_header->Checksum);
                    break;
                case WINDIVERT_FILTER_FIELD_ICMP_BODY:
                    field[0] = (UINT32)RtlUlongByteSwap(icmp_header->Body);
                    break;
                case WINDIVERT_FILTER_FIELD_ICMPV6_TYPE:
                    field[0] = (UINT32)icmpv6_header->Type;
                    break;
                case WINDIVERT_FILTER_FIELD_ICMPV6_CODE:
                    field[0] = (UINT32)icmpv6_header->Code;
                    break;
                case WINDIVERT_FILTER_FIELD_ICMPV6_CHECKSUM:
                    field[0] = (UINT32)icmpv6_header->Checksum;
                    break;
                case WINDIVERT_FILTER_FIELD_ICMPV6_BODY:
                    field[0] = (UINT32)icmpv6_header->Body;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_SRCPORT:
                    field[0] = (UINT32)RtlUshortByteSwap(tcp_header->SrcPort);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_DSTPORT:
                    field[0] = (UINT32)RtlUshortByteSwap(tcp_header->DstPort);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_SEQNUM:
                    field[0] = (UINT32)RtlUlongByteSwap(tcp_header->SeqNum);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_ACKNUM:
                    field[0] = (UINT32)RtlUlongByteSwap(tcp_header->AckNum);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_HDRLENGTH:
                    field[0] = (UINT32)tcp_header->HdrLength;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_URG:
                    field[0] = (UINT32)tcp_header->Urg;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_ACK:
                    field[0] = (UINT32)tcp_header->Ack;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_PSH:
                    field[0] = (UINT32)tcp_header->Psh;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_RST:
                    field[0] = (UINT32)tcp_header->Rst;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_SYN:
                    field[0] = (UINT32)tcp_header->Syn;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_FIN:
                    field[0] = (UINT32)tcp_header->Fin;
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_WINDOW:
                    field[0] = (UINT32)RtlUshortByteSwap(tcp_header->Window);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_CHECKSUM:
                    field[0] = (UINT32)RtlUshortByteSwap(tcp_header->Checksum);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_URGPTR:
                    field[0] = (UINT32)RtlUshortByteSwap(tcp_header->UrgPtr);
                    break;
                case WINDIVERT_FILTER_FIELD_TCP_PAYLOADLENGTH:
                    field[0] = (UINT32)(tot_len - ip_header_len -
                        tcp_header->HdrLength*sizeof(UINT32));
                    break;
                case WINDIVERT_FILTER_FIELD_UDP_SRCPORT:
                    field[0] = (UINT32)RtlUshortByteSwap(udp_header->SrcPort);
                    break;
                case WINDIVERT_FILTER_FIELD_UDP_DSTPORT:
                    field[0] = (UINT32)RtlUshortByteSwap(udp_header->DstPort);
                    break;
                case WINDIVERT_FILTER_FIELD_UDP_LENGTH:
                    field[0] = (UINT32)RtlUshortByteSwap(udp_header->Length);
                    break;
                case WINDIVERT_FILTER_FIELD_UDP_CHECKSUM:
                    field[0] = (UINT32)RtlUshortByteSwap(udp_header->Checksum);
                    break;
                case WINDIVERT_FILTER_FIELD_UDP_PAYLOADLENGTH:
                    field[0] = (UINT32)(tot_len - ip_header_len -
                        sizeof(struct udphdr));
                    break;
                default:
                    field[0] = 0;
                    break;
            }
            switch (filter[ip].test)
            {
                case WINDIVERT_FILTER_TEST_EQ:
                    result = (field[0] == filter[ip].arg[0] &&
                              field[1] == filter[ip].arg[1] &&
                              field[2] == filter[ip].arg[2] &&
                              field[3] == filter[ip].arg[3]);
                    break;
                case WINDIVERT_FILTER_TEST_NEQ:
                    result = (field[0] != filter[ip].arg[0] ||
                              field[1] != filter[ip].arg[1] ||
                              field[2] != filter[ip].arg[2] ||
                              field[3] != filter[ip].arg[3]);
                    break;
                case WINDIVERT_FILTER_TEST_LT:
                    result = (field[3] < filter[ip].arg[3] ||
                             (field[3] == filter[ip].arg[3] &&
                              field[2] < filter[ip].arg[2] ||
                             (field[2] == filter[ip].arg[2] && 
                              field[1] < filter[ip].arg[1] ||
                             (field[1] == filter[ip].arg[1] &&
                              field[0] < filter[ip].arg[0]))));
                    break;
                case WINDIVERT_FILTER_TEST_LEQ:
                    result = (field[3] < filter[ip].arg[3] ||
                             (field[3] == filter[ip].arg[3] &&
                              field[2] < filter[ip].arg[2] ||
                             (field[2] == filter[ip].arg[2] && 
                              field[1] < filter[ip].arg[1] ||
                             (field[1] == filter[ip].arg[1] &&
                              field[0] <= filter[ip].arg[0]))));
                    break;
                case WINDIVERT_FILTER_TEST_GT:
                    result = (field[3] > filter[ip].arg[3] ||
                             (field[3] == filter[ip].arg[3] &&
                              field[2] > filter[ip].arg[2] ||
                             (field[2] == filter[ip].arg[2] && 
                              field[1] > filter[ip].arg[1] ||
                             (field[1] == filter[ip].arg[1] &&
                              field[0] > filter[ip].arg[0]))));
                    break;
                case WINDIVERT_FILTER_TEST_GEQ:
                    result = (field[3] > filter[ip].arg[3] ||
                             (field[3] == filter[ip].arg[3] &&
                              field[2] > filter[ip].arg[2] ||
                             (field[2] == filter[ip].arg[2] && 
                              field[1] > filter[ip].arg[1] ||
                             (field[1] == filter[ip].arg[1] &&
                              field[0] >= filter[ip].arg[0]))));
                    break;
                default:
                    result = FALSE;
                    break;
            }
        }
        ip = (result? filter[ip].success: filter[ip].failure);
        if (ip == WINDIVERT_FILTER_RESULT_ACCEPT)
        {
            return TRUE;
        }
        if (ip == WINDIVERT_FILTER_RESULT_REJECT)
        {
            return FALSE;
        }
    }
    DEBUG("FILTER: REJECT (filter TTL exceeded)");
    return FALSE;
}

/*
 * Analyze the given filter.
 */
static void windivert_filter_analyze(filter_t filter, BOOL *is_inbound,
    BOOL *is_outbound, BOOL *is_ipv4, BOOL *is_ipv6)
{
    BOOL result;

    // False filter?
    result = windivert_filter_test(filter, 0, WINDIVERT_FILTER_PROTOCOL_NONE,
        WINDIVERT_FILTER_FIELD_ZERO, 0);
    if (!result)
    {
        *is_inbound  = FALSE;
        *is_outbound = FALSE;
        *is_ipv4     = FALSE;
        *is_ipv6     = FALSE;
        return;
    }

    // Inbound?
    result = windivert_filter_test(filter, 0, WINDIVERT_FILTER_PROTOCOL_NONE,
        WINDIVERT_FILTER_FIELD_INBOUND, 1);
    if (result)
    {
        result = windivert_filter_test(filter, 0,
            WINDIVERT_FILTER_PROTOCOL_NONE, WINDIVERT_FILTER_FIELD_OUTBOUND,
            0);
    }
    *is_inbound = result;

    // Outbound?
    result = windivert_filter_test(filter, 0, WINDIVERT_FILTER_PROTOCOL_NONE,
        WINDIVERT_FILTER_FIELD_OUTBOUND, 1);
    if (result)
    {
        result = windivert_filter_test(filter, 0,
            WINDIVERT_FILTER_PROTOCOL_NONE, WINDIVERT_FILTER_FIELD_INBOUND, 0);
    }
    *is_outbound = result;

    // IPv4?
    result = windivert_filter_test(filter, 0, WINDIVERT_FILTER_PROTOCOL_NONE,
        WINDIVERT_FILTER_FIELD_IP, 1);
    if (result)
    {
        result = windivert_filter_test(filter, 0,
            WINDIVERT_FILTER_PROTOCOL_NONE, WINDIVERT_FILTER_FIELD_IPV6, 0);
    }
    *is_ipv4 = result;

    // Ipv6?
    result = windivert_filter_test(filter, 0, WINDIVERT_FILTER_PROTOCOL_NONE,
        WINDIVERT_FILTER_FIELD_IPV6, 1);
    if (result)
    {
        result = windivert_filter_test(filter, 0,
            WINDIVERT_FILTER_PROTOCOL_NONE, WINDIVERT_FILTER_FIELD_IP, 0);
    }
    *is_ipv6 = result;
}

/*
 * Test a filter for any packet where field = arg.
 */
static BOOL windivert_filter_test(filter_t filter, UINT16 ip, UINT8 protocol,
    UINT8 field, UINT32 arg)
{
    BOOL known = FALSE;
    BOOL result = FALSE;

    if (ip == WINDIVERT_FILTER_RESULT_ACCEPT)
    {
        return TRUE;
    }
    if (ip == WINDIVERT_FILTER_RESULT_REJECT)
    {
        return FALSE;
    }
    if (ip > WINDIVERT_FILTER_MAXLEN)
    {
        return FALSE;
    }

    if (filter[ip].protocol == protocol &&
        filter[ip].field == field)
    {
        known = TRUE;
        switch (filter[ip].test)
        {
            case WINDIVERT_FILTER_TEST_EQ:
                result = (arg == filter[ip].arg[0]);
                break;
            case WINDIVERT_FILTER_TEST_NEQ:
                result = (arg != filter[ip].arg[0]);
                break;
            case WINDIVERT_FILTER_TEST_LT:
                result = (arg < filter[ip].arg[0]);
                break;
            case WINDIVERT_FILTER_TEST_LEQ:
                result = (arg <= filter[ip].arg[0]);
                break;
            case WINDIVERT_FILTER_TEST_GT:
                result = (arg > filter[ip].arg[0]);
                break;
            case WINDIVERT_FILTER_TEST_GEQ:
                result = (arg >= filter[ip].arg[0]);
                break;
            default:
                result = FALSE;
                break;
        }
    }

    if (!known)
    {
        result = windivert_filter_test(filter, filter[ip].success, protocol,
            field, arg);
        if (result)
        {
            return TRUE;
        }
        return windivert_filter_test(filter, filter[ip].failure, protocol,
            field, arg);
    }
    else
    {
        ip = (result? filter[ip].success: filter[ip].failure);
        return windivert_filter_test(filter, ip, protocol, field, arg);
    }
}

/*
 * Compile a WinDivert filter from an IOCTL.
 */
static filter_t windivert_filter_compile(windivert_ioctl_filter_t ioctl_filter,
    size_t ioctl_filter_len)
{
    filter_t filter0 = NULL, result = NULL;
    UINT16 i;
    size_t length;

    if (ioctl_filter_len % sizeof(struct windivert_ioctl_filter_s) != 0)
    {
        goto windivert_filter_compile_exit;
    }
    length = ioctl_filter_len / sizeof(struct windivert_ioctl_filter_s);
    if (length >= WINDIVERT_FILTER_MAXLEN)
    {
        goto windivert_filter_compile_exit;
    }

    // Do NOT use the stack (size = 12Kb on x86) for filter0.
    filter0 = (filter_t)ExAllocatePoolWithTag(NonPagedPool,
        WINDIVERT_FILTER_MAXLEN*sizeof(struct filter_s), WINDIVERT_FILTER_TAG);
    if (filter0 == NULL)
    {
        goto windivert_filter_compile_exit;
    }
 
    for (i = 0; i < length; i++)
    {
        if (ioctl_filter[i].field > WINDIVERT_FILTER_FIELD_MAX ||
            ioctl_filter[i].test > WINDIVERT_FILTER_TEST_MAX)
        {
            goto windivert_filter_compile_exit;
        }
        switch (ioctl_filter[i].success)
        {
            case WINDIVERT_FILTER_RESULT_ACCEPT:
            case WINDIVERT_FILTER_RESULT_REJECT:
                break;
            default:
                if (ioctl_filter[i].success <= i ||
                    ioctl_filter[i].success >= length)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
        }
        switch (ioctl_filter[i].failure)
        {
            case WINDIVERT_FILTER_RESULT_ACCEPT:
            case WINDIVERT_FILTER_RESULT_REJECT:
                break;
            default:
                if (ioctl_filter[i].failure <= i ||
                    ioctl_filter[i].failure >= length)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
        }

        // Enforce size limits:
        if (ioctl_filter[i].field != WINDIVERT_FILTER_FIELD_IPV6_SRCADDR &&
            ioctl_filter[i].field != WINDIVERT_FILTER_FIELD_IPV6_DSTADDR)
        {
            if (ioctl_filter[i].arg[1] != 0 ||
                ioctl_filter[i].arg[2] != 0 ||
                ioctl_filter[i].arg[3] != 0)
            {
                goto windivert_filter_compile_exit;
            }
        }
        switch (ioctl_filter[i].field)
        {
            case WINDIVERT_FILTER_FIELD_ZERO:
            case WINDIVERT_FILTER_FIELD_INBOUND:
            case WINDIVERT_FILTER_FIELD_OUTBOUND:
            case WINDIVERT_FILTER_FIELD_IP:
            case WINDIVERT_FILTER_FIELD_IPV6:
            case WINDIVERT_FILTER_FIELD_ICMP:
            case WINDIVERT_FILTER_FIELD_ICMPV6:
            case WINDIVERT_FILTER_FIELD_TCP:
            case WINDIVERT_FILTER_FIELD_UDP:
            case WINDIVERT_FILTER_FIELD_IP_DF:
            case WINDIVERT_FILTER_FIELD_IP_MF:
            case WINDIVERT_FILTER_FIELD_TCP_URG:
            case WINDIVERT_FILTER_FIELD_TCP_ACK:
            case WINDIVERT_FILTER_FIELD_TCP_PSH:
            case WINDIVERT_FILTER_FIELD_TCP_RST:
            case WINDIVERT_FILTER_FIELD_TCP_SYN:
            case WINDIVERT_FILTER_FIELD_TCP_FIN:
                if (ioctl_filter[i].arg[0] > 1)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            case WINDIVERT_FILTER_FIELD_IP_HDRLENGTH:
            case WINDIVERT_FILTER_FIELD_TCP_HDRLENGTH:
                if (ioctl_filter[i].arg[0] > 0x0F)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            case WINDIVERT_FILTER_FIELD_IP_TTL:
            case WINDIVERT_FILTER_FIELD_IP_PROTOCOL:
            case WINDIVERT_FILTER_FIELD_IPV6_TRAFFICCLASS:
            case WINDIVERT_FILTER_FIELD_IPV6_NEXTHDR:
            case WINDIVERT_FILTER_FIELD_IPV6_HOPLIMIT:
            case WINDIVERT_FILTER_FIELD_ICMP_TYPE:
            case WINDIVERT_FILTER_FIELD_ICMP_CODE:
            case WINDIVERT_FILTER_FIELD_ICMPV6_TYPE:
            case WINDIVERT_FILTER_FIELD_ICMPV6_CODE:
                if (ioctl_filter[i].arg[0] > UINT8_MAX)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            case WINDIVERT_FILTER_FIELD_IP_FRAGOFF:
                if (ioctl_filter[i].arg[0] > 0x1FFF)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            case WINDIVERT_FILTER_FIELD_IP_TOS:
            case WINDIVERT_FILTER_FIELD_IP_LENGTH:
            case WINDIVERT_FILTER_FIELD_IP_ID:
            case WINDIVERT_FILTER_FIELD_IP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_IPV6_LENGTH:
            case WINDIVERT_FILTER_FIELD_ICMP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_ICMPV6_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_TCP_SRCPORT:
            case WINDIVERT_FILTER_FIELD_TCP_DSTPORT:
            case WINDIVERT_FILTER_FIELD_TCP_WINDOW:
            case WINDIVERT_FILTER_FIELD_TCP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_TCP_URGPTR:
            case WINDIVERT_FILTER_FIELD_TCP_PAYLOADLENGTH:
            case WINDIVERT_FILTER_FIELD_UDP_SRCPORT:
            case WINDIVERT_FILTER_FIELD_UDP_DSTPORT:
            case WINDIVERT_FILTER_FIELD_UDP_LENGTH:
            case WINDIVERT_FILTER_FIELD_UDP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_UDP_PAYLOADLENGTH:
                if (ioctl_filter[i].arg[0] > UINT16_MAX)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            case WINDIVERT_FILTER_FIELD_IPV6_FLOWLABEL:
                if (ioctl_filter[i].arg[0] > 0x000FFFFF)
                {
                    goto windivert_filter_compile_exit;
                }
                break;
            default:
                break;
        }
        filter0[i].field   = ioctl_filter[i].field;
        filter0[i].test    = ioctl_filter[i].test;
        filter0[i].success = ioctl_filter[i].success;
        filter0[i].failure = ioctl_filter[i].failure;
        filter0[i].arg[0]  = ioctl_filter[i].arg[0];
        filter0[i].arg[1]  = ioctl_filter[i].arg[1];
        filter0[i].arg[2]  = ioctl_filter[i].arg[2];
        filter0[i].arg[3]  = ioctl_filter[i].arg[3];

        // Protocol selection:
        switch (ioctl_filter[i].field)
        {
            case WINDIVERT_FILTER_FIELD_ZERO:
            case WINDIVERT_FILTER_FIELD_INBOUND:
            case WINDIVERT_FILTER_FIELD_OUTBOUND:
            case WINDIVERT_FILTER_FIELD_IFIDX:
            case WINDIVERT_FILTER_FIELD_SUBIFIDX:
            case WINDIVERT_FILTER_FIELD_IP:
            case WINDIVERT_FILTER_FIELD_IPV6:
            case WINDIVERT_FILTER_FIELD_ICMP:
            case WINDIVERT_FILTER_FIELD_ICMPV6:
            case WINDIVERT_FILTER_FIELD_TCP:
            case WINDIVERT_FILTER_FIELD_UDP:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_NONE;
                break;
            case WINDIVERT_FILTER_FIELD_IP_HDRLENGTH:
            case WINDIVERT_FILTER_FIELD_IP_TOS:
            case WINDIVERT_FILTER_FIELD_IP_LENGTH:
            case WINDIVERT_FILTER_FIELD_IP_ID:
            case WINDIVERT_FILTER_FIELD_IP_DF:
            case WINDIVERT_FILTER_FIELD_IP_MF:
            case WINDIVERT_FILTER_FIELD_IP_FRAGOFF:
            case WINDIVERT_FILTER_FIELD_IP_TTL:
            case WINDIVERT_FILTER_FIELD_IP_PROTOCOL:
            case WINDIVERT_FILTER_FIELD_IP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_IP_SRCADDR:
            case WINDIVERT_FILTER_FIELD_IP_DSTADDR:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_IP;
                break;
            case WINDIVERT_FILTER_FIELD_IPV6_TRAFFICCLASS:
            case WINDIVERT_FILTER_FIELD_IPV6_FLOWLABEL:
            case WINDIVERT_FILTER_FIELD_IPV6_LENGTH:
            case WINDIVERT_FILTER_FIELD_IPV6_NEXTHDR:
            case WINDIVERT_FILTER_FIELD_IPV6_HOPLIMIT:
            case WINDIVERT_FILTER_FIELD_IPV6_SRCADDR:
            case WINDIVERT_FILTER_FIELD_IPV6_DSTADDR:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_IPV6;
                break;
            case WINDIVERT_FILTER_FIELD_ICMP_TYPE:
            case WINDIVERT_FILTER_FIELD_ICMP_CODE:
            case WINDIVERT_FILTER_FIELD_ICMP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_ICMP_BODY:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_ICMP;
                break;
            case WINDIVERT_FILTER_FIELD_ICMPV6_TYPE:
            case WINDIVERT_FILTER_FIELD_ICMPV6_CODE:
            case WINDIVERT_FILTER_FIELD_ICMPV6_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_ICMPV6_BODY:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_ICMPV6;
                break;
            case WINDIVERT_FILTER_FIELD_TCP_SRCPORT:
            case WINDIVERT_FILTER_FIELD_TCP_DSTPORT:
            case WINDIVERT_FILTER_FIELD_TCP_SEQNUM:
            case WINDIVERT_FILTER_FIELD_TCP_ACKNUM:
            case WINDIVERT_FILTER_FIELD_TCP_HDRLENGTH:
            case WINDIVERT_FILTER_FIELD_TCP_URG:
            case WINDIVERT_FILTER_FIELD_TCP_ACK:
            case WINDIVERT_FILTER_FIELD_TCP_PSH:
            case WINDIVERT_FILTER_FIELD_TCP_RST:
            case WINDIVERT_FILTER_FIELD_TCP_SYN:
            case WINDIVERT_FILTER_FIELD_TCP_FIN:
            case WINDIVERT_FILTER_FIELD_TCP_WINDOW:
            case WINDIVERT_FILTER_FIELD_TCP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_TCP_URGPTR:
            case WINDIVERT_FILTER_FIELD_TCP_PAYLOADLENGTH:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_TCP;
                break;
            case WINDIVERT_FILTER_FIELD_UDP_SRCPORT:
            case WINDIVERT_FILTER_FIELD_UDP_DSTPORT:
            case WINDIVERT_FILTER_FIELD_UDP_LENGTH:
            case WINDIVERT_FILTER_FIELD_UDP_CHECKSUM:
            case WINDIVERT_FILTER_FIELD_UDP_PAYLOADLENGTH:
                filter0[i].protocol = WINDIVERT_FILTER_PROTOCOL_UDP;
                break;
            default:
                goto windivert_filter_compile_exit;
        }
    }
    
    result = (filter_t)ExAllocatePoolWithTag(NonPagedPool,
        i*sizeof(struct filter_s), WINDIVERT_FILTER_TAG);
    if (result != NULL)
    {
        RtlMoveMemory(result, filter0, i*sizeof(struct filter_s));
    }

windivert_filter_compile_exit:

    if (filter0 != NULL)
    {
        ExFreePoolWithTag(filter0, WINDIVERT_FILTER_TAG);
    }
    return result;
}

