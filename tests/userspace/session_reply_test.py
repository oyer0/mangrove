#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Original sessiond reply only: failure rolls back its exact created ID."""
from pathlib import Path
import os
import re
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/shared'))
from host_fixture import HostFixture
CC=sys.argv[1] if len(sys.argv)>1 else 'gcc'
s=(ROOT/'userspace/sessiond/main.c').read_text()
def fn(name):
    m=re.search(r'^static [^\n;{}]*\b'+name+r'\s*\(',s,re.M)
    assert m
    b=s.index('{',m.end());e=b+1;d=1
    while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
    return s[m.start():e]+'\n'
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mg/error.h>
#include <mg/ipc.h>
#include <mg/session.h>
#include <mg/session_service.h>
#include <mg/log_service.h>
#include <mg/service.h>
static unsigned reply_calls,end_calls,create_calls;
static mg_result_t reply_result,create_result;
static void log_session_event(mg_log_severity_t s,const char*e,const char*u)
{(void)s;(void)e;(void)u;}
mg_result_t ipc_reply(mg_handle_t h,const mg_ipc_message_t*m)
{assert(h==7&&m->type==MG_SESSION_RESPONSE);reply_calls++;return reply_result;}
mg_result_t session_create_from_request(mg_handle_t h,const char*u,mg_session_info_t*out)
{assert(h==7&&!strcmp(u,"developer"));create_calls++;out->id=41;return create_result;}
mg_result_t session_end(mg_session_id_t id)
{assert(id==41);end_calls++;return MG_ERR_BUSY;}
mg_result_t session_query(mg_session_id_t id,mg_session_status_t*out)
{(void)id;(void)out;return MG_ERR_NOT_FOUND;}
mg_result_t session_list(u32 at,mg_session_status_t*s,u32 cap,u32*n,u32*t)
{(void)at;(void)s;(void)cap;*n=*t=0;return MG_OK;}
'''+''.join(fn(n) for n in ('service_request_from_logind','bounded_string','send_response','handle_request'))+r'''
int main(void){mg_ipc_received_t received={0};received.request=7;
 received.requester.system_service=true;received.requester.service_id=MG_SERVICE_LOGIND;
 received.message.type=MG_SESSION_REQUEST;
 received.message.payload_length=sizeof(mg_session_request_t);
 mg_session_request_t request={.version=MG_SESSION_PROTOCOL_VERSION,.operation=MG_SESSION_OP_CREATE};
 strcpy(request.username,"developer");memcpy(received.message.payload,&request,sizeof(request));
 for(unsigned mode=0;mode<3;mode++) {
  reply_calls=end_calls=create_calls=0;reply_result=mode?MG_ERR_NOT_FOUND:MG_OK;
  create_result=mode==2?MG_ERR_BUSY:MG_OK;handle_request(&received);
  assert(reply_calls==1&&create_calls==1&&end_calls==(mode==1?1U:0U));
 }
 /* A failed duplicate must not be interpreted as original non-delivery. */
 reply_calls=end_calls=0;reply_result=MG_OK;create_result=MG_OK;handle_request(&received);
 assert(reply_calls==1&&!end_calls);reply_result=MG_ERR_NOT_FOUND;
 mg_session_response_t duplicate={.result=MG_OK};assert(!send_response(&received,&duplicate));assert(!end_calls);
 puts("sessiond: original reply once, exact-ID rollback, creation failure and duplicate negative control PASS");}
'''
with HostFixture('session-reply') as fixture:
    source=fixture.path/'reply.c';source.write_text(code)
    binary=fixture.path/'reply'
    fixture.run([CC,'-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-idirafter',str(ROOT/'libc/include'),
        '-idirafter',str(ROOT/'include'),str(source),'-o',str(binary)])
    print(fixture.run([str(binary)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}).stdout,end='')
