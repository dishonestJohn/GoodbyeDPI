﻿/*
 * GoodbyeDPI — Passive DPI blocker and Active DPI circumvention utility.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <string.h>
//#include <in6addr.h>
#include <ws2tcpip.h>
#include <Iphlpapi.h>
#include <TlHelp32.h>
#include "windivert.h"
#include "goodbyedpi.h"
#include "utils/repl_str.h"
#include "dnsredir.h"
#include "blackwhitelist.h"
#include "../resource.h"
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#define PVOID char *

#define GOODBYEDPI_VERSION "v0.1.5"

#define die() do { Sleep(20); exit(EXIT_FAILURE); } while (0)

#define MAX_FILTERS 4
#define MAX_PACKET_SIZE 9016

#define DIVERT_NO_LOCALNETSv4_DST "(" \
                   "(ip.DstAddr < 127.0.0.1 or ip.DstAddr > 127.255.255.255) and " \
                   "(ip.DstAddr < 10.0.0.0 or ip.DstAddr > 10.255.255.255) and " \
                   "(ip.DstAddr < 192.168.0.0 or ip.DstAddr > 192.168.255.255) and " \
                   "(ip.DstAddr < 172.16.0.0 or ip.DstAddr > 172.31.255.255) and " \
                   "(ip.DstAddr < 169.254.0.0 or ip.DstAddr > 169.254.255.255)" \
                   ")"
#define DIVERT_NO_LOCALNETSv4_SRC "(" \
                   "(ip.SrcAddr < 127.0.0.1 or ip.SrcAddr > 127.255.255.255) and " \
                   "(ip.SrcAddr < 10.0.0.0 or ip.SrcAddr > 10.255.255.255) and " \
                   "(ip.SrcAddr < 192.168.0.0 or ip.SrcAddr > 192.168.255.255) and " \
                   "(ip.SrcAddr < 172.16.0.0 or ip.SrcAddr > 172.31.255.255) and " \
                   "(ip.SrcAddr < 169.254.0.0 or ip.SrcAddr > 169.254.255.255)" \
                   ")"

#define DIVERT_NO_LOCALNETSv6_DST "(" \
                   "(ipv6.DstAddr > ::1) and " \
                   "(ipv6.DstAddr < 2001::0 or ipv6.DstAddr > 2001:1::0) and " \
                   "(ipv6.DstAddr < fc00::0 or ipv6.DstAddr > fe00::0) and " \
                   "(ipv6.DstAddr < fe80::0 or ipv6.DstAddr > fec0::0) and " \
                   "(ipv6.DstAddr < ff00::0 or ipv6.DstAddr > ffff::0)" \
                   ")"
#define DIVERT_NO_LOCALNETSv6_SRC "(" \
                   "(ipv6.SrcAddr > ::1) and " \
                   "(ipv6.SrcAddr < 2001::0 or ipv6.SrcAddr > 2001:1::0) and " \
                   "(ipv6.SrcAddr < fc00::0 or ipv6.SrcAddr > fe00::0) and " \
                   "(ipv6.SrcAddr < fe80::0 or ipv6.SrcAddr > fec0::0) and " \
                   "(ipv6.SrcAddr < ff00::0 or ipv6.SrcAddr > ffff::0)" \
                   ")"

/* #IPID# is a template to find&replace */
#define IPID_TEMPLATE "#IPID#"
#define FILTER_STRING_TEMPLATE \
        "(tcp and !impostor and !loopback and " \
        "((inbound and (" \
         "(" \
          "(" \
           "(ipv6 or (ip.Id >= 0x0 and ip.Id <= 0xF) " IPID_TEMPLATE \
           ") and " \
           "tcp.SrcPort == 80 and tcp.Ack" \
          ") or " \
          "((tcp.SrcPort == 80 or tcp.SrcPort == 443) and tcp.Ack and tcp.Syn)" \
         ")" \
         " and (" DIVERT_NO_LOCALNETSv4_SRC " or " DIVERT_NO_LOCALNETSv6_SRC "))) or " \
        "(outbound and " \
         "(tcp.DstPort == 80 or tcp.DstPort == 443) and tcp.Ack and " \
         "(" DIVERT_NO_LOCALNETSv4_DST " or " DIVERT_NO_LOCALNETSv6_DST "))" \
        "))"
#define FILTER_PASSIVE_STRING_TEMPLATE "inbound and ip and tcp and " \
        "!impostor and !loopback and " \
        "((ip.Id <= 0xF and ip.Id >= 0x0) " IPID_TEMPLATE ") and " \
        "(tcp.SrcPort == 443 or tcp.SrcPort == 80) and tcp.Rst and " \
        DIVERT_NO_LOCALNETSv4_SRC

#define SET_HTTP_FRAGMENT_SIZE_OPTION(fragment_size) do { \
    if (!http_fragment_size) { \
        http_fragment_size = (unsigned int)fragment_size; \
    } \
    else if (http_fragment_size != (unsigned int)fragment_size) { \
        printf( \
            "WARNING: HTTP fragment size is already set to %d, not changing.\n", \
            http_fragment_size \
        ); \
    } \
} while (0)

static HANDLE filters[MAX_FILTERS];
static int filter_num = 0;
static const char http10_redirect_302[] = "HTTP/1.0 302 ";
static const char http11_redirect_302[] = "HTTP/1.1 302 ";
static const char http_host_find[] = "\r\nHost: ";
static const char http_host_replace[] = "\r\nhoSt: ";
static const char http_useragent_find[] = "\r\nUser-Agent: ";
static const char location_http[] = "\r\nLocation: http://";
static const char connection_close[] = "\r\nConnection: close";
static const char *http_methods[] = {
    "GET ",
    "HEAD ",
    "POST ",
    "PUT ",
    "DELETE ",
    "CONNECT ",
    "OPTIONS ",
};
//static const char *filter_

static char *filter_string = NULL;
static char *filter_passive_string = NULL;

static void finalize_filter_strings() {
    char *newstr;

    newstr = repl_str(filter_string, IPID_TEMPLATE, "");
    free(filter_string);
    filter_string = newstr;

    newstr = repl_str(filter_passive_string, IPID_TEMPLATE, "");
    free(filter_passive_string);
    filter_passive_string = newstr;
}

static char* dumb_memmem(const char* haystack, unsigned int hlen,
                         const char* needle, size_t nlen)
{
    // naive implementation
    if (nlen > hlen) return NULL;
    size_t i;
    for (i=0; i<hlen-nlen+1; i++) {
        if (memcmp(haystack+i,needle,nlen)==0) {
            return (char*)(haystack+i);
        }
    }
    return NULL;
}

static HANDLE init(char *filter, UINT64 flags) {
    LPTSTR errormessage = NULL;
    DWORD errorcode = 0;
    filter = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, flags);
    if (filter != INVALID_HANDLE_VALUE)
        return filter;
    errorcode = GetLastError();
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL, errorcode, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                  (LPTSTR)&errormessage, 0, NULL);
    printf("Error opening filter: %s", errormessage);
    LocalFree(errormessage);
    if (errorcode == 577)
        printf("Windows Server 2016 systems must have secure boot disabled to be "
               "able to load WinDivert driver.\n"
               "Windows 7 systems must be up-to-date or at least have KB3033929 installed.\n"
               "https://www.microsoft.com/en-us/download/details.aspx?id=46078\n\n"
               "WARNING! If you see this error on Windows 7, it means your system is horribly "
               "outdated and SHOULD NOT BE USED TO ACCESS THE INTERNET!\n"
               "Most probably, you don't have security patches installed and anyone in you LAN or "
               "public Wi-Fi network can get full access to your computer (MS17-010 and others).\n"
               "You should install updates IMMEDIATELY.\n");
    return NULL;
}

static int deinit(HANDLE handle) {
    if (handle) {
        WinDivertClose(handle);
        return TRUE;
    }
    return FALSE;
}

void deinit_all() {
    for (int i = 0; i < filter_num; i++) {
        deinit(filters[i]);
    }
}

static void sigint_handler(int sig) {
    deinit_all();
    exit(EXIT_SUCCESS);
}

static void mix_case(char *pktdata, unsigned int pktlen) {
    unsigned int i;

    if (pktlen <= 0) return;
    for (i = 0; i < pktlen; i++) {
        if (i % 2) {
            pktdata[i] = (char) toupper(pktdata[i]);
        }
    }
}

static int is_passivedpi_redirect(const char *pktdata, unsigned int pktlen) {
    /* First check if this is HTTP 302 redirect */
    if (memcmp(pktdata, http11_redirect_302, sizeof(http11_redirect_302)-1) == 0 ||
        memcmp(pktdata, http10_redirect_302, sizeof(http10_redirect_302)-1) == 0)
    {
        /* Then check if this is a redirect to new http site with Connection: close */
        if (dumb_memmem(pktdata, pktlen, location_http, sizeof(location_http)-1) &&
            dumb_memmem(pktdata, pktlen, connection_close, sizeof(connection_close)-1)) {
            return TRUE;
        }
    }
    return FALSE;
}

static int find_header_and_get_info(const char *pktdata, unsigned int pktlen,
                const char *hdrname,
                char **hdrnameaddr,
                char **hdrvalueaddr, unsigned int *hdrvaluelen) {
    char *data_addr_rn;
    char *hdr_begin;

    *hdrvaluelen = 0u;
    *hdrnameaddr = NULL;
    *hdrvalueaddr = NULL;

    /* Search for the header */
    hdr_begin = dumb_memmem(pktdata, pktlen,
                hdrname, strlen(hdrname));
    if (!hdr_begin) return FALSE;
    if ((PVOID)pktdata > (PVOID)hdr_begin) return FALSE;

    /* Set header address */
    *hdrnameaddr = hdr_begin;
    *hdrvalueaddr = (PVOID)hdr_begin + strlen(hdrname);

    /* Search for header end (\r\n) */
    data_addr_rn = dumb_memmem(*hdrvalueaddr,
                        pktlen - ((PVOID)*hdrvalueaddr - (PVOID)pktdata),
                        "\r\n", 2);
    if (data_addr_rn) {
        *hdrvaluelen = (PVOID)data_addr_rn - (PVOID)*hdrvalueaddr;
        if (*hdrvaluelen > 0u && *hdrvaluelen <= 512u)
            return TRUE;
    }
    return FALSE;
}

static inline void change_window_size(const PWINDIVERT_TCPHDR ppTcpHdr, unsigned int size) {
    if (size >= 1 && size <= 0xFFFFu) {
        ppTcpHdr->Window = htons((u_short)size);
    }
}

/* HTTP method end without trailing space */
static PVOID find_http_method_end(const char *pkt, unsigned int http_frag, int *is_fragmented) {
    unsigned int i;
    for (i = 0; i<(sizeof(http_methods) / sizeof(*http_methods)); i++) {
        if (memcmp(pkt, http_methods[i], strlen(http_methods[i])) == 0) {
            if (is_fragmented)
                *is_fragmented = 0;
            return (char*)pkt + strlen(http_methods[i]) - 1;
        }
        /* Try to find HTTP method in a second part of fragmented packet */
        if ((http_frag == 1 || http_frag == 2) &&
            memcmp(pkt, http_methods[i] + http_frag,
                   strlen(http_methods[i]) - http_frag) == 0
           )
        {
            if (is_fragmented)
                *is_fragmented = 1;
            return (char*)pkt + strlen(http_methods[i]) - http_frag - 1;
        }
    }
    return NULL;
}

static char *print_ipaddress(uint32_t ipaddr)
{
  static char str[16];
  char *ptr = str;
  while (ipaddr) {
    char tmp[5];
    _itoa(ipaddr >> 24, tmp, 10);
    ptr = strcpy(ptr, tmp) + strlen(tmp);
    *ptr = '.';
    ptr++;
    ipaddr <<= 8;
  }
  *--ptr = 0;
  return str;
}

static BOOL filterByProcessName(u_short src_port, u_short dest_port, char *proc)
{
  MIB_TCPTABLE_OWNER_PID *pTCPInfo;
  MIB_TCPROW_OWNER_PID *owner;
  DWORD size;
  DWORD dwResult;

  dwResult = GetExtendedTcpTable(NULL, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
  pTCPInfo = (MIB_TCPTABLE_OWNER_PID*)malloc(size);
  dwResult = GetExtendedTcpTable(pTCPInfo, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
  for (DWORD dwLoop = 0; dwLoop < pTCPInfo->dwNumEntries; dwLoop++)
  {
    owner = &pTCPInfo->table[dwLoop];
    int open = ntohs(owner->dwLocalPort);
    int remote = ntohs(owner->dwRemotePort);

    if (open == ntohs(src_port) && remote == ntohs(dest_port)) {
      //debug("pid: %d ", owner->dwOwningPid);

      HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (hSnapshot) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnapshot, &pe32)) {
          do {
            if (pe32.th32ProcessID == owner->dwOwningPid) {
              //printf("(%s)\n", pe32.szExeFile);
              if (!strcmp(proc, pe32.szExeFile))
                return TRUE;
              return FALSE;
              break;
            }
          } while (Process32Next(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
      }
    }
  }
  return FALSE;
}

BOOL extractResource(const HINSTANCE hInstance, WORD resourceID, LPCTSTR szFilename)
{
  BOOL bSuccess = FALSE;

  // Find and load the resource
  HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(resourceID), "dll");
  HGLOBAL hFileResource = LoadResource(NULL, hResource);
  debug("%d", hFileResource);

  // Open and map this to a disk file
  LPVOID lpFile = LockResource(hFileResource);
  DWORD dwSize = SizeofResource(hInstance, hResource);

  // Open the file and filemap
  HANDLE hFile = CreateFile(szFilename, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE hFileMap = CreateFileMapping(hFile, NULL, PAGE_READWRITE, 0, dwSize, NULL);
  LPVOID lpAddress = MapViewOfFile(hFileMap, FILE_MAP_WRITE, 0, 0, 0);

  // Write the file
  CopyMemory(lpAddress, lpFile, dwSize);

  // Un-map the file and close the handles
  UnmapViewOfFile(lpAddress);
  CloseHandle(hFileMap);
  CloseHandle(hFile);
  bSuccess = TRUE;

  return bSuccess;
}

int main(int argc, char *argv[]) {
    //extractResource(GetModuleHandle(NULL), IDR_DLL1, "WinDivert.dll");

    static enum packet_type_e {
        unknown,
        ipv4_tcp, ipv4_tcp_data, ipv4_udp_data,
        ipv6_tcp, ipv6_tcp_data, ipv6_udp_data
    } packet_type;
    int i, should_reinject, should_recalc_checksum = 0;
    int opt;
    int packet_v4, packet_v6;
    HANDLE w_filter = NULL;
    WINDIVERT_ADDRESS addr;
    char packet[MAX_PACKET_SIZE];
    PVOID packet_data;
    UINT packetLen;
    UINT packet_dataLen;
    PWINDIVERT_IPHDR ppIpHdr;
    PWINDIVERT_IPV6HDR ppIpV6Hdr;
    PWINDIVERT_TCPHDR ppTcpHdr;
    PWINDIVERT_UDPHDR ppUdpHdr;
    conntrack_info_t dns_conn_info;

    int do_passivedpi = 0, do_fragment_http = 0,
        do_fragment_http_persistent = 0,
        do_fragment_http_persistent_nowait = 0,
        do_fragment_https = 0, do_host = 0,
        do_host_removespace = 0, do_additional_space = 0,
        do_http_allports = 0,
        do_host_mixedcase = 0,
        do_dnsv4_redirect = 0, do_dnsv6_redirect = 0,
        do_dns_verb = 0, do_blacklist = 0;
    unsigned int http_fragment_size = 0;
    unsigned int https_fragment_size = 0;
    uint32_t dnsv4_addr = 0;
    struct in6_addr dnsv6_addr = {0};
    struct in6_addr dns_temp_addr = {0};
    uint16_t dnsv4_port = htons(53);
    uint16_t dnsv6_port = htons(53);
    char *host_addr, *useragent_addr, *method_addr;
    unsigned int host_len, useragent_len;
    int http_req_fragmented;

    char *hdr_name_addr = NULL, *hdr_value_addr = NULL;
    unsigned int hdr_value_len;

    // Make sure to search DLLs only in safe path, not in current working dir.
    SetDllDirectory("");
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);

    filter_string = _strdup(FILTER_STRING_TEMPLATE);
    filter_passive_string = _strdup(FILTER_PASSIVE_STRING_TEMPLATE);

    printf(
        "GoodbyeDPI " GOODBYEDPI_VERSION
        ": Passive DPI blocker and Active DPI circumvention utility\n"
        "https://github.com/ValdikSS/GoodbyeDPI\n\n"
    );

    http_fragment_size = https_fragment_size = 2;
    do_passivedpi = do_host = do_host_removespace \
            = do_fragment_http = do_fragment_https \
            = do_fragment_http_persistent \
            = do_fragment_http_persistent_nowait = 1;

    printf("Block passive: %d\nFragment HTTP: %d\nFragment persistent HTTP: %d\n"
           "Fragment HTTPS: %d\nhoSt: %d\nHost no space: %d\nAdditional space: %d\n"
           "Mix Host: %d\nHTTP AllPorts: %d\nHTTP Persistent Nowait: %d\n"
           "DNS redirect: %d\nDNSv6 redirect: %d\n",
           do_passivedpi, (do_fragment_http ? http_fragment_size : 0),
           (do_fragment_http_persistent ? http_fragment_size : 0),
           (do_fragment_https ? https_fragment_size : 0),
           do_host, do_host_removespace, do_additional_space, do_host_mixedcase,
           do_http_allports, do_fragment_http_persistent_nowait, do_dnsv4_redirect,
           do_dnsv6_redirect
          );

    if (do_fragment_http && http_fragment_size > 2) {
        printf("WARNING: HTTP fragmentation values > 2 are not fully compatible "
               "with other options. Please use values <= 2 or disable HTTP fragmentation "
               "completely.\n");
    }

    printf("\nOpening filter\n");
    finalize_filter_strings();
    filter_num = 0;

    if (do_passivedpi) {
        /* IPv4 only filter for inbound RST packets with ID [0x0; 0xF] */
        filters[filter_num] = init(
            filter_passive_string,
            WINDIVERT_FLAG_DROP);
        if (filters[filter_num] == NULL)
            die();
        filter_num++;
    }

    /* 
     * IPv4 & IPv6 filter for inbound HTTP redirection packets and
     * active DPI circumvention
     */
    filters[filter_num] = init(filter_string, 0);

    w_filter = filters[filter_num];
    filter_num++;

    for (i = 0; i < filter_num; i++) {
        if (filters[i] == NULL)
            die();
    }

    printf("Filter activated!\n");
    signal(SIGINT, sigint_handler);

    int ports[65535] = { 0, };

    while (1) {
        if (WinDivertRecv(w_filter, packet, sizeof(packet), &addr, &packetLen)) {
            //debug("=============================================\n");
            //debug("Got %s packet, len=%d!\n", addr.Direction ? "inbound" : "outbound",
            //       packetLen);
            should_reinject = 1;
            should_recalc_checksum = 0;

            ppIpHdr = (PWINDIVERT_IPHDR)NULL;
            ppIpV6Hdr = (PWINDIVERT_IPV6HDR)NULL;
            ppTcpHdr = (PWINDIVERT_TCPHDR)NULL;
            ppUdpHdr = (PWINDIVERT_UDPHDR)NULL;
            packet_v4 = packet_v6 = 0;
            packet_type = unknown;

            // Parse network packet and set it's type
            if ((packet_v4 = WinDivertHelperParsePacket(packet, packetLen, &ppIpHdr,
                NULL, NULL, NULL, &ppTcpHdr, NULL, &packet_data, &packet_dataLen)))
            {
                packet_type = ipv4_tcp_data;
            }
            else if ((packet_v6 = WinDivertHelperParsePacket(packet, packetLen, NULL,
                &ppIpV6Hdr, NULL, NULL, &ppTcpHdr, NULL, &packet_data, &packet_dataLen)))
            {
                packet_type = ipv6_tcp_data;
            }
            else if ((packet_v4 = WinDivertHelperParsePacket(packet, packetLen, &ppIpHdr,
                NULL, NULL, NULL, &ppTcpHdr, NULL, NULL, NULL)))
            {
                packet_type = ipv4_tcp;
            }
            else if ((packet_v6 = WinDivertHelperParsePacket(packet, packetLen, NULL,
                &ppIpV6Hdr, NULL, NULL, &ppTcpHdr, NULL, NULL, NULL)))
            {
                packet_type = ipv6_tcp;
            }
            else if ((packet_v4 = WinDivertHelperParsePacket(packet, packetLen, &ppIpHdr,
                NULL, NULL, NULL, NULL, &ppUdpHdr, &packet_data, &packet_dataLen)))
            {
                packet_type = ipv4_udp_data;
            }
            else if ((packet_v6 = WinDivertHelperParsePacket(packet, packetLen, NULL,
                &ppIpV6Hdr, NULL, NULL, NULL, &ppUdpHdr, &packet_data, &packet_dataLen)))
            {
                packet_type = ipv6_udp_data;
            }

            //debug("packet_type: %d, packet_v4: %d, packet_v6: %d\n", packet_type, packet_v4, packet_v6);

            if (ppTcpHdr != NULL && filterByProcessName(ppTcpHdr->SrcPort, ppTcpHdr->DstPort, "Koromo Copy UX.exe"))
            {
              debug("=============================================\n");

              debug("src-port: %d, ", ntohs(ppTcpHdr->SrcPort));
              debug("dest-port: %d\n", ntohs(ppTcpHdr->DstPort));

              if (ppIpHdr != NULL)
              {
                debug("src: %s, ", print_ipaddress(ppIpHdr->SrcAddr));
                debug("dest: %s\n", print_ipaddress(ppIpHdr->DstAddr));
              }
              else
              {
                debug("src: %s, ", print_ipaddress(ppIpV6Hdr->SrcAddr));
                debug("dest: %s\n", print_ipaddress(ppIpV6Hdr->DstAddr));
              }

              ports[ntohs(ppTcpHdr->SrcPort)]++;

              for (int i = 0; i < 65535; i++)
                if (ports[i] > 0)
                  debug(" -- %d (%d)\n", i, ports[i]);
            }

            if (packet_type == ipv4_tcp_data || packet_type == ipv6_tcp_data) {
                //printf("Got parsed packet, len=%d!\n", packet_dataLen);
                /* Got a TCP packet WITH DATA */

                /* Handle INBOUND packet with data and find HTTP REDIRECT in there */
                if (addr.Direction == WINDIVERT_DIRECTION_INBOUND && packet_dataLen > 16) {
                    /* If INBOUND packet with DATA (tcp.Ack) */

                    /* Drop packets from filter with HTTP 30x Redirect */
                    if (do_passivedpi && is_passivedpi_redirect(packet_data, packet_dataLen)) {
                        if (packet_v4) {
                            //printf("Dropping HTTP Redirect packet!\n");
                            should_reinject = 0;
                        }
                        else if (packet_v6 && WINDIVERT_IPV6HDR_GET_FLOWLABEL(ppIpV6Hdr) == 0x0) {
                                /* Contrary to IPv4 where we get only packets with IP ID 0x0-0xF,
                                 * for IPv6 we got all the incoming data packets since we can't
                                 * filter them in a driver.
                                 *
                                 * Handle only IPv6 Flow Label == 0x0 for now
                                 */
                                //printf("Dropping HTTP Redirect packet!\n");
                                should_reinject = 0;
                        }
                    }
                }
                /* Handle OUTBOUND packet on port 80, search for Host header */
                else if (addr.Direction == WINDIVERT_DIRECTION_OUTBOUND && 
                        packet_dataLen > 16 &&
                        (do_http_allports ? 1 : (ppTcpHdr->DstPort == htons(80))) &&
                        find_http_method_end(packet_data,
                                             (do_fragment_http ? http_fragment_size : 0u),
                                             &http_req_fragmented) &&
                        (do_host || do_host_removespace ||
                        do_host_mixedcase || do_fragment_http_persistent))
                {

                    /* Find Host header */
                    if (find_header_and_get_info(packet_data, packet_dataLen,
                        http_host_find, &hdr_name_addr, &hdr_value_addr, &hdr_value_len) &&
                        hdr_value_len > 0 && hdr_value_len <= HOST_MAXLEN &&
                        (do_blacklist ? blackwhitelist_check_hostname(hdr_value_addr, hdr_value_len) : 1))
                    {
                        host_addr = hdr_value_addr;
                        host_len = hdr_value_len;

                        /*
                         * Handle new HTTP request in new
                         * connection (when Window Size modification disabled)
                         * or already established connection (keep-alive).
                         * We split HTTP request into two packets: one of http_fragment_size length
                         * and another of original_size - http_fragment_size length.
                         *
                         * The second packet of a splitted part is not really needed to be sent
                         * as Windows understand that is hasn't been sent by checking
                         * ack number of received packet and retransmitting missing part again,
                         * but it's better to send it anyway since it eliminates one RTT.
                         */
                        if (do_fragment_http_persistent && !http_req_fragmented &&
                            (packet_dataLen > http_fragment_size))
                        {
                            if (packet_v4)
                                ppIpHdr->Length = htons(
                                    ntohs(ppIpHdr->Length) -
                                    packet_dataLen + http_fragment_size
                                );
                            else if (packet_v6)
                                ppIpV6Hdr->Length = htons(
                                    ntohs(ppIpV6Hdr->Length) -
                                    packet_dataLen + http_fragment_size
                                );

                            WinDivertHelperCalcChecksums(
                                packet, packetLen - packet_dataLen + http_fragment_size, &addr, 0
                            );
                            WinDivertSend(
                                w_filter, packet,
                                packetLen - packet_dataLen + http_fragment_size,
                                &addr, NULL
                            );

                            if (do_fragment_http_persistent_nowait) {
                                if (packet_v4)
                                    ppIpHdr->Length = htons(
                                        ntohs(ppIpHdr->Length) -
                                        http_fragment_size + packet_dataLen - http_fragment_size
                                    );
                                else if (packet_v6)
                                    ppIpV6Hdr->Length = htons(
                                        ntohs(ppIpV6Hdr->Length) -
                                        http_fragment_size + packet_dataLen - http_fragment_size
                                    );
                                memmove(packet_data,
                                        packet_data + http_fragment_size,
                                        packet_dataLen);
                                packet_dataLen -= http_fragment_size;
                                packetLen -= http_fragment_size;
                                hdr_value_addr -= http_fragment_size;
                                hdr_name_addr -= http_fragment_size;
                                host_addr = hdr_value_addr;

                                ppTcpHdr->SeqNum = htonl(ntohl(ppTcpHdr->SeqNum) + http_fragment_size);
                                should_recalc_checksum = 1;
                            }
                            else {
                                continue;
                            }
                        }

                        if (do_host_mixedcase) {
                            mix_case(host_addr, host_len);
                            should_recalc_checksum = 1;
                        }

                        if (do_host) {
                            /* Replace "Host: " with "hoSt: " */
                            memcpy(hdr_name_addr, http_host_replace, strlen(http_host_replace));
                            should_recalc_checksum = 1;
                            //printf("Replaced Host header!\n");
                        }

                        /* If removing space between host header and its value
                         * and adding additional space between Method and Request-URI */
                        if (do_additional_space && do_host_removespace) {
                            /* End of "Host:" without trailing space */
                            method_addr = find_http_method_end(packet_data,
                                                            (do_fragment_http ? http_fragment_size : 0),
                                                            NULL);

                            if (method_addr) {
                                memmove(method_addr + 1, method_addr,
                                        (PVOID)host_addr - (PVOID)method_addr - 1);
                                should_recalc_checksum = 1;
                            }
                        }
                        /* If just removing space between host header and its value */
                        else if (do_host_removespace) {
                            if (find_header_and_get_info(packet_data, packet_dataLen,
                                                        http_useragent_find, &hdr_name_addr,
                                                         &hdr_value_addr, &hdr_value_len))
                            {
                                useragent_addr = hdr_value_addr;
                                useragent_len = hdr_value_len;

                                /* We move Host header value by one byte to the left and then
                                 * "insert" stolen space to the end of User-Agent value because
                                 * some web servers are not tolerant to additional space in the
                                 * end of Host header.
                                 *
                                 * Nothing is done if User-Agent header is missing.
                                 */
                                if (useragent_addr && useragent_len > 0) {
                                    /* useragent_addr is in the beginning of User-Agent value */

                                    if (useragent_addr > host_addr) {
                                        /* Move one byte to the LEFT from "Host:"
                                        * to the end of User-Agent
                                        */
                                        memmove(host_addr - 1, host_addr,
                                                (PVOID)useragent_addr + useragent_len - (PVOID)host_addr);
                                        host_addr -= 1;
                                        /* Put space in the end of User-Agent header */
                                        *(char*)((PVOID)useragent_addr + useragent_len - 1) = ' ';
                                        should_recalc_checksum = 1;
                                        //printf("Replaced Host header!\n");
                                    }
                                    else {
                                        /* User-Agent goes BEFORE Host header */

                                        /* Move one byte to the RIGHT from the end of User-Agent
                                        * to the "Host:"
                                        */
                                        memmove((PVOID)useragent_addr + useragent_len + 1,
                                                (PVOID)useragent_addr + useragent_len,
                                                (PVOID)host_addr - 1 - ((PVOID)useragent_addr + useragent_len));
                                        /* Put space in the end of User-Agent header */
                                        *(char*)((PVOID)useragent_addr + useragent_len) = ' ';
                                        should_recalc_checksum = 1;
                                        //printf("Replaced Host header!\n");
                                    }
                                } /* if (host_len <= HOST_MAXLEN && useragent_addr) */
                            } /* if (find_header_and_get_info http_useragent) */
                        } /* else if (do_host_removespace) */
                    } /* if (find_header_and_get_info http_host) */
                } /* Handle OUTBOUND packet with data */
            } /* Handle TCP packet with data */

            /* Else if we got TCP packet without data */
            else if (packet_type == ipv4_tcp || packet_type == ipv6_tcp) {
                /* If we got INBOUND SYN+ACK packet */
                if (addr.Direction == WINDIVERT_DIRECTION_INBOUND &&
                    ppTcpHdr->Syn == 1 && ppTcpHdr->Ack == 1) {
                    //printf("Changing Window Size!\n");
                    /*
                     * Window Size is changed even if do_fragment_http_persistent
                     * is enabled as there could be non-HTTP data on port 80
                     */
                    if (do_fragment_http && ppTcpHdr->SrcPort == htons(80)) {
                        change_window_size(ppTcpHdr, http_fragment_size);
                        should_recalc_checksum = 1;
                    }
                    else if (do_fragment_https && ppTcpHdr->SrcPort != htons(80)) {
                        change_window_size(ppTcpHdr, https_fragment_size);
                        should_recalc_checksum = 1;
                    }
                }
            }

            /* Else if we got UDP packet with data */
            else if ((do_dnsv4_redirect && (packet_type == ipv4_udp_data)) ||
                     (do_dnsv6_redirect && (packet_type == ipv6_udp_data)))
            {
                if (addr.Direction == WINDIVERT_DIRECTION_INBOUND) {
                    if ((packet_v4 && dns_handle_incoming(&ppIpHdr->DstAddr, ppUdpHdr->DstPort,
                                        packet_data, packet_dataLen,
                                        &dns_conn_info, 0))
                        ||
                        (packet_v6 && dns_handle_incoming(ppIpV6Hdr->DstAddr, ppUdpHdr->DstPort,
                                        packet_data, packet_dataLen,
                                        &dns_conn_info, 1)))
                    {
                        /* Changing source IP and port to the values
                         * from DNS conntrack */
                        if (packet_v4)
                            ppIpHdr->SrcAddr = dns_conn_info.dstip[0];
                        else if (packet_v6)
                            ipv6_copy_addr(ppIpV6Hdr->SrcAddr, dns_conn_info.dstip);
                        ppUdpHdr->DstPort = dns_conn_info.srcport;
                        ppUdpHdr->SrcPort = dns_conn_info.dstport;
                        should_recalc_checksum = 1;
                    }
                    else {
                        if (dns_is_dns_packet(packet_data, packet_dataLen, 0))
                            should_reinject = 0;

                        if (do_dns_verb && !should_reinject) {
                            printf("[DNS] Error handling incoming packet: srcport = %hu, dstport = %hu\n",
                               ntohs(ppUdpHdr->SrcPort), ntohs(ppUdpHdr->DstPort));
                        }
                    }
                }

                else if (addr.Direction == WINDIVERT_DIRECTION_OUTBOUND) {
                    if ((packet_v4 && dns_handle_outgoing(&ppIpHdr->SrcAddr, ppUdpHdr->SrcPort,
                                        &ppIpHdr->DstAddr, ppUdpHdr->DstPort,
                                        packet_data, packet_dataLen, 0))
                        ||
                        (packet_v6 && dns_handle_outgoing(ppIpV6Hdr->SrcAddr, ppUdpHdr->SrcPort,
                                        ppIpV6Hdr->DstAddr, ppUdpHdr->DstPort,
                                        packet_data, packet_dataLen, 1)))
                    {
                        /* Changing destination IP and port to the values
                         * from configuration */
                        if (packet_v4) {
                            ppIpHdr->DstAddr = dnsv4_addr;
                            ppUdpHdr->DstPort = dnsv4_port;
                        }
                        else if (packet_v6) {
                            ipv6_copy_addr(ppIpV6Hdr->DstAddr, (uint32_t*)dnsv6_addr.s6_addr);
                            ppUdpHdr->DstPort = dnsv6_port;
                        }
                        should_recalc_checksum = 1;
                    }
                    else {
                        if (dns_is_dns_packet(packet_data, packet_dataLen, 1))
                            should_reinject = 0;

                        if (do_dns_verb && !should_reinject) {
                            printf("[DNS] Error handling outgoing packet: srcport = %hu, dstport = %hu\n",
                               ntohs(ppUdpHdr->SrcPort), ntohs(ppUdpHdr->DstPort));
                        }
                    }
                }
            }

            if (should_reinject) {
                //printf("Re-injecting!\n");
                if (should_recalc_checksum) {
                    WinDivertHelperCalcChecksums(packet, packetLen, &addr, NULL);
                }
                WinDivertSend(w_filter, packet, packetLen, &addr, NULL);
            }
        }
        else {
            // error, ignore
            printf("Error receiving packet!\n");
            break;
        }
    }
}
