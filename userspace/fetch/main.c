#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fetch_url.h"
#include "../common/help.h"

#define FETCH_MAX_RESPONSE 65536U
#define FETCH_HEADER_MAX 8192U

static bool find_crlfcrlf(const u8 *data, usize length, usize *offset)
{
    if (!data || !offset) return false;
    for (usize i = 3; i < length; i++)
        if (data[i-3]=='\r' && data[i-2]=='\n' && data[i-1]=='\r' && data[i]=='\n') { *offset=i+1; return true; }
    return false;
}

static bool ascii_equal_ci(const u8 *a, const char *b, usize length)
{
    for (usize i = 0; i < length; i++) {
        u8 left = a[i];
        u8 right = (u8)b[i];
        if (left >= 'A' && left <= 'Z') left = (u8)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (u8)(right - 'A' + 'a');
        if (left != right) return false;
    }
    return true;
}

static bool ascii_space(u8 c)
{
    return c == ' ' || c == '\t';
}

static bool transfer_encoding_has_chunked(const u8 *value, usize length)
{
    usize p = 0;
    while (p < length) {
        usize start;
        usize end;
        while (p < length && (ascii_space(value[p]) || value[p] == ',')) p++;
        start = p;
        while (p < length && value[p] != ',') p++;
        end = p;
        while (end > start && ascii_space(value[end - 1])) end--;
        if (end - start == 7 && ascii_equal_ci(value + start, "chunked", 7)) return true;
        if (p < length) p++;
    }
    return false;
}

static bool parse_decimal(const u8 *p, usize n, usize *value)
{
    usize v=0; if (!p||!n||!value) return false;
    for (usize i=0;i<n;i++) { if (p[i]<'0'||p[i]>'9') return false; v=v*10U+(usize)(p[i]-'0'); }
    *value=v; return true;
}

static bool hex_value(u8 c, u32 *v)
{
    if(c>='0'&&c<='9')*v=c-'0'; else if(c>='a'&&c<='f')*v=c-'a'+10; else if(c>='A'&&c<='F')*v=c-'A'+10; else return false; return true;
}

static bool decode_chunked(const u8 *in, usize length, u8 *out, usize capacity, usize *out_length)
{
    usize p=0,o=0;
    while (p<length) {
        usize line=p; u32 size=0; bool have=false;
        while (p+1<length && !(in[p]=='\r'&&in[p+1]=='\n')) p++;
        if (p+1>=length) return false;
        for (usize i=line;i<p;i++) { u32 digit; if(in[i]==';')break; if(in[i]==' '||in[i]=='\t')continue; if(!hex_value(in[i],&digit))return false; if(size>0x0fffffffU) return false; size=(size<<4)|digit; have=true; }
        if(!have || size>capacity-o || p+2+size+2>length)return false;
        p+=2; if(size==0){
            /* Consume trailers through the terminating empty line.  Trailer
             * bytes are metadata, never part of the entity body. */
            while (p < length) {
                usize trailer_start = p;
                while (p + 1 < length && !(in[p] == '\r' && in[p + 1] == '\n')) p++;
                if (p + 1 >= length) return false;
                p += 2;
                if (p == trailer_start + 2) { *out_length = o; return true; }
            }
            return false;
        }
        memcpy(out+o,in+p,size);o+=size;p+=size;
        if (p + 1 >= length || in[p] != '\r' || in[p + 1] != '\n') return false;
        p += 2;
    }
    return false;
}

static bool parse_response(const u8 *data, usize length, u8 *body, usize capacity,
                           usize *body_length, u32 *status)
{
    usize header_end, line_end=0, content_length=0, parsed_status=0; bool have_length=false, chunked=false;
    if(!find_crlfcrlf(data,length,&header_end)||header_end>FETCH_HEADER_MAX)return false;
    while(line_end+1<header_end && !(data[line_end]=='\r'&&data[line_end+1]=='\n'))line_end++;
    if(line_end<12||memcmp(data,"HTTP/1.",7)!=0||data[8]!=' '||!parse_decimal(data+9,3,&parsed_status)||parsed_status>999)return false;
    *status=(u32)parsed_status;
    line_end+=2;
    while(line_end+1<header_end-2){usize end=line_end;while(end+1<header_end&&!(data[end]=='\r'&&data[end+1]=='\n'))end++;if(end==line_end)return false;usize colon=line_end;while(colon<end&&data[colon]!=':')colon++;if(colon==end)return false;usize v=colon+1;while(v<end&&(data[v]==' '||data[v]=='\t'))v++;usize n=end-v;if(ascii_equal_ci(data+line_end,"Content-Length",colon-line_end)) { if(!parse_decimal(data+v,n,&content_length))return false;have_length=true; } else if(ascii_equal_ci(data+line_end,"Transfer-Encoding",colon-line_end)&&transfer_encoding_has_chunked(data+v,n))chunked=true; line_end=end+2;}
    if(chunked)return decode_chunked(data+header_end,length-header_end,body,capacity,body_length);
    if(have_length){if(content_length>capacity||length-header_end<content_length)return false;memcpy(body,data+header_end,content_length);*body_length=content_length;return true;}
    if (length - header_end > capacity) return false;
    memcpy(body, data + header_end, length - header_end);
    *body_length = length - header_end;
    return true;
}

int main(int argc, char **argv)
{
    fetch_url_t url; fetch_url_parse_result_t url_result; mg_ipv4_addr_t address; mg_net_endpoint_t endpoint; mg_handle_t stream; mg_handle_t file = 0; mg_result_t result; char request[1024]; char filename[FETCH_FILENAME_MAX]; u8 *response, *body; usize used=0,total=0,body_length; u32 status;
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if(argc!=2||!argv[1]){
        command_usage_error(argv[0], "fetch <url>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    url_result=fetch_parse_url(argv[1],&url);
    if(url_result==FETCH_URL_PARSE_HTTPS_UNSUPPORTED){printf("HTTPS is not supported yet.\n");return 1;}
    if(url_result!=FETCH_URL_PARSE_OK){printf("Invalid HTTP URL.\n");return 1;}
    if(!fetch_url_filename(&url,filename,sizeof(filename))){printf("Invalid download filename.\n");return 1;}
    if(!mg_ipv4_parse(url.host,&address)&&mg_net_resolve_a(url.host,&address,MG_NET_TIMEOUT_DEFAULT)<0){printf("Could not resolve %s.\n",url.host);return 1;}
    response=(u8 *)malloc(FETCH_MAX_RESPONSE);body=(u8 *)malloc(FETCH_MAX_RESPONSE);
    if(!response||!body){free(response);free(body);printf("HTTP response buffer allocation failed.\n");return 1;}
    endpoint=(mg_net_endpoint_t){.address=address,.port=url.port};
    used=(usize)snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mangrove/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n",url.path,url.host);
    if(used>=sizeof(request)||mg_stream_connect(&endpoint,5000,&stream)<0){free(response);free(body);printf("HTTP connection failed.\n");return 1;}
    if(mg_stream_send(stream,request,used,5000)<0){(void)mg_stream_close(stream);free(response);free(body);printf("HTTP request failed.\n");return 1;}
    while(total<FETCH_MAX_RESPONSE){result=mg_stream_receive(stream,response+total,FETCH_MAX_RESPONSE-total,5000);if(result<0){if(result==MG_ERR_CONNECTION_CLOSED)break;break;}if(!result)break;total+=(usize)result;}
    (void)mg_stream_close(stream);
    if(!parse_response(response,total,body,FETCH_MAX_RESPONSE,&body_length,&status)){free(response);free(body);printf("Malformed or oversized HTTP response.\n");return 1;}
    if(status < 200 || status >= 300){free(response);free(body);printf("HTTP request failed: status %u.\n", status);return 1;}

    result = file_create(filename);
    if (result == MG_ERR_ALREADY_EXISTS) {
        free(response); free(body);
        printf("Could not save \"%s\": file already exists.\n", filename);
        return 1;
    }
    if (result < 0) {
        free(response); free(body);
        printf("Could not save \"%s\": %s.\n", filename, error_string(result));
        return 1;
    }

    result = file_open(filename, MG_OPEN_WRITE);
    if (result < 0) {
        (void)path_remove(filename);
        free(response); free(body);
        printf("Could not save \"%s\": %s.\n", filename, error_string(result));
        return 1;
    }
    file = (mg_handle_t)result;
    result = object_write_all(file, body, body_length);
    if (result < 0 || (usize)result != body_length) {
        mg_result_t failure = result < 0 ? result : MG_ERR_IO;
        (void)handle_close(file);
        (void)path_remove(filename);
        free(response); free(body);
        printf("Could not save \"%s\": %s.\n", filename, error_string(failure));
        return 1;
    }
    result = handle_close(file);
    if (result < 0) {
        (void)path_remove(filename);
        free(response); free(body);
        printf("Could not save \"%s\": %s.\n", filename, error_string(result));
        return 1;
    }

    printf("Saved %s\n", filename);
    free(response);free(body);
    return 0;
}
