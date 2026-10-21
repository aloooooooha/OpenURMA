// SPDX-License-Identifier: Apache-2.0
// Kernel-facing QUERY_DEV_ATTR simulation for the fixed June-24 TLV ABI.
// Mapping follows unmodified 0993431 urma_ioctl_query_dev_attr ATTR definitions.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "urma_cmd.h"
#include "urma_cmd_tlv.h"
int ioctl(int fd,unsigned long request,...) {
    va_list ap;va_start(ap,request);void* argument=va_arg(ap,void*);va_end(ap);
    static int (*real_ioctl)(int,unsigned long,...);
    if(!real_ioctl)real_ioctl=dlsym(RTLD_NEXT,"ioctl");
    const char* root=getenv("OPENURMA_FAKE_ROOT");
    char fdpath[64],path[4096];snprintf(fdpath,sizeof(fdpath),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(fdpath,path,sizeof(path)-1);
    if(n>=0)path[n]=0;
    if(!root||n<0||strncmp(path,root,strlen(root))!=0||
       strstr(path+strlen(root),"/dev/uburma/")!=path+strlen(root)||request!=URMA_CMD)
        return real_ioctl(fd,request,argument);
    urma_cmd_hdr_t* h=argument;
    if(!h||h->command!=URMA_CMD_QUERY_DEV_ATTR) return real_ioctl(fd,request,argument);
    if(h->args_len>URMA_CMD_MAX_ARGS_SIZE||h->args_len%sizeof(urma_cmd_attr_t)){errno=EINVAL;return -1;}
    urma_device_attr_t caps={0};
    caps.port_cnt=1;caps.port_attr[0].state=URMA_PORT_ACTIVE;
    caps.port_attr[0].active_mtu=URMA_MTU_4096;caps.port_attr[0].max_mtu=URMA_MTU_4096;
    caps.dev_cap.max_jfc=caps.dev_cap.max_jfs=caps.dev_cap.max_jfr=caps.dev_cap.max_jetty=4096;
    caps.dev_cap.max_jfc_depth=4096;caps.dev_cap.max_jfs_depth=256;caps.dev_cap.max_jfr_depth=256;
    caps.dev_cap.max_jfs_sge=caps.dev_cap.max_jfr_sge=8;caps.dev_cap.max_jfs_rsge=1;
    caps.dev_cap.max_msg_size=caps.dev_cap.max_read_size=caps.dev_cap.max_write_size=4096;
    caps.dev_cap.max_eid_cnt=1;caps.dev_cap.trans_mode=URMA_TM_RM;caps.dev_cap.rm_tp_cap.bs.ctp=1;
    caps.dev_cap.priority_info[6].tp_type.bs.ctp=1;caps.dev_cap.priority_info[7].tp_type.bs.ctp=1;
    urma_cmd_attr_t* attrs=(void*)(uintptr_t)h->args_addr;
    for(unsigned i=0;i<h->args_len/sizeof(*attrs);++i){
        urma_cmd_attr_t* a=attrs+i;if(a->type<URMA_CMD_OUT_TYPE_INIT)continue;
        const void* source=NULL;size_t size=0;
        switch(a->type){
            case QUERY_DEVICE_OUT_GUID: source=&caps.guid; size=sizeof(caps.guid); break;
            case QUERY_DEVICE_OUT_DEV_CAP_FEATURE: source=&caps.dev_cap.feature; size=sizeof(caps.dev_cap.feature); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC: source=&caps.dev_cap.max_jfc; size=sizeof(caps.dev_cap.max_jfc); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS: source=&caps.dev_cap.max_jfs; size=sizeof(caps.dev_cap.max_jfs); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR: source=&caps.dev_cap.max_jfr; size=sizeof(caps.dev_cap.max_jfr); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY: source=&caps.dev_cap.max_jetty; size=sizeof(caps.dev_cap.max_jetty); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_GRP: source=&caps.dev_cap.max_jetty_grp; size=sizeof(caps.dev_cap.max_jetty_grp); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_IN_JETTY_GRP: source=&caps.dev_cap.max_jetty_in_jetty_grp; size=sizeof(caps.dev_cap.max_jetty_in_jetty_grp); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC_DEPTH: source=&caps.dev_cap.max_jfc_depth; size=sizeof(caps.dev_cap.max_jfc_depth); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_DEPTH: source=&caps.dev_cap.max_jfs_depth; size=sizeof(caps.dev_cap.max_jfs_depth); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_DEPTH: source=&caps.dev_cap.max_jfr_depth; size=sizeof(caps.dev_cap.max_jfr_depth); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_INLINE_LEN: source=&caps.dev_cap.max_jfs_inline_len; size=sizeof(caps.dev_cap.max_jfs_inline_len); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_SGE: source=&caps.dev_cap.max_jfs_sge; size=sizeof(caps.dev_cap.max_jfs_sge); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_RSGE: source=&caps.dev_cap.max_jfs_rsge; size=sizeof(caps.dev_cap.max_jfs_rsge); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_SGE: source=&caps.dev_cap.max_jfr_sge; size=sizeof(caps.dev_cap.max_jfr_sge); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_MSG_SIZE: source=&caps.dev_cap.max_msg_size; size=sizeof(caps.dev_cap.max_msg_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_READ_SIZE: source=&caps.dev_cap.max_read_size; size=sizeof(caps.dev_cap.max_read_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_WRITE_SIZE: source=&caps.dev_cap.max_write_size; size=sizeof(caps.dev_cap.max_write_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_CAS_SIZE: source=&caps.dev_cap.max_cas_size; size=sizeof(caps.dev_cap.max_cas_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_SWAP_SIZE: source=&caps.dev_cap.max_swap_size; size=sizeof(caps.dev_cap.max_swap_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_ADD_SIZE: source=&caps.dev_cap.max_fetch_and_add_size; size=sizeof(caps.dev_cap.max_fetch_and_add_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_SUB_SIZE: source=&caps.dev_cap.max_fetch_and_sub_size; size=sizeof(caps.dev_cap.max_fetch_and_sub_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_AND_SIZE: source=&caps.dev_cap.max_fetch_and_and_size; size=sizeof(caps.dev_cap.max_fetch_and_and_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_OR_SIZE: source=&caps.dev_cap.max_fetch_and_or_size; size=sizeof(caps.dev_cap.max_fetch_and_or_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_XOR_SIZE: source=&caps.dev_cap.max_fetch_and_xor_size; size=sizeof(caps.dev_cap.max_fetch_and_xor_size); break;
            case QUERY_DEVICE_OUT_DEV_CAP_ATOMIC_FEAT: source=&caps.dev_cap.atomic_feat; size=sizeof(caps.dev_cap.atomic_feat); break;
            case QUERY_DEVICE_OUT_DEV_CAP_TRANS_MODE: source=&caps.dev_cap.trans_mode; size=sizeof(caps.dev_cap.trans_mode); break;
            case QUERY_DEVICE_OUT_DEV_CAP_RESERVED: source=&caps.dev_cap.reserved; size=sizeof(caps.dev_cap.reserved); break;
            case QUERY_DEVICE_OUT_DEV_CAP_CONGESTION_CTRL_ALG: source=&caps.dev_cap.congestion_ctrl_alg; size=sizeof(caps.dev_cap.congestion_ctrl_alg); break;
            case QUERY_DEVICE_OUT_DEV_CAP_CEQ_CNT: source=&caps.dev_cap.ceq_cnt; size=sizeof(caps.dev_cap.ceq_cnt); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_TP_IN_TPG: source=&caps.dev_cap.max_tp_in_tpg; size=sizeof(caps.dev_cap.max_tp_in_tpg); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_EID_CNT: source=&caps.dev_cap.max_eid_cnt; size=sizeof(caps.dev_cap.max_eid_cnt); break;
            case QUERY_DEVICE_OUT_DEV_CAP_PAGE_SIZE_CAP: source=&caps.dev_cap.page_size_cap; size=sizeof(caps.dev_cap.page_size_cap); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_OOR_CNT: source=&caps.dev_cap.max_oor_cnt; size=sizeof(caps.dev_cap.max_oor_cnt); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MN: source=&caps.dev_cap.mn; size=sizeof(caps.dev_cap.mn); break;
            case QUERY_DEVICE_OUT_DEV_CAP_MAX_NETADDR_CN: source=&caps.dev_cap.max_netaddr_cnt; size=sizeof(caps.dev_cap.max_netaddr_cnt); break;
            case QUERY_DEVICE_OUT_PORT_CNT: source=&caps.port_cnt; size=sizeof(caps.port_cnt); break;
            case QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MIN: source=&caps.reserved_jetty_id_min; size=sizeof(caps.reserved_jetty_id_min); break;
            case QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MAX: source=&caps.reserved_jetty_id_max; size=sizeof(caps.reserved_jetty_id_max); break;
            case QUERY_DEVICE_OUT_PORT_ATTR_MAX_MTU:
                if(a->field_size!=sizeof(caps.port_attr[0].max_mtu) || a->attr_data.bs.el_num>MAX_PORT_CNT){errno=EINVAL;return -1;}
                for(unsigned j=0;j<a->attr_data.bs.el_num;++j)
                    memcpy((char*)(uintptr_t)a->data+j*a->attr_data.bs.el_size,&caps.port_attr[j].max_mtu,a->field_size);
                continue;
            case QUERY_DEVICE_OUT_PORT_ATTR_STATE:
                if(a->field_size!=sizeof(caps.port_attr[0].state) || a->attr_data.bs.el_num>MAX_PORT_CNT){errno=EINVAL;return -1;}
                for(unsigned j=0;j<a->attr_data.bs.el_num;++j)
                    memcpy((char*)(uintptr_t)a->data+j*a->attr_data.bs.el_size,&caps.port_attr[j].state,a->field_size);
                continue;
            case QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_WIDTH:
                if(a->field_size!=sizeof(caps.port_attr[0].active_width) || a->attr_data.bs.el_num>MAX_PORT_CNT){errno=EINVAL;return -1;}
                for(unsigned j=0;j<a->attr_data.bs.el_num;++j)
                    memcpy((char*)(uintptr_t)a->data+j*a->attr_data.bs.el_size,&caps.port_attr[j].active_width,a->field_size);
                continue;
            case QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_SPEED:
                if(a->field_size!=sizeof(caps.port_attr[0].active_speed) || a->attr_data.bs.el_num>MAX_PORT_CNT){errno=EINVAL;return -1;}
                for(unsigned j=0;j<a->attr_data.bs.el_num;++j)
                    memcpy((char*)(uintptr_t)a->data+j*a->attr_data.bs.el_size,&caps.port_attr[j].active_speed,a->field_size);
                continue;
            case QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_MTU:
                if(a->field_size!=sizeof(caps.port_attr[0].active_mtu) || a->attr_data.bs.el_num>MAX_PORT_CNT){errno=EINVAL;return -1;}
                for(unsigned j=0;j<a->attr_data.bs.el_num;++j)
                    memcpy((char*)(uintptr_t)a->data+j*a->attr_data.bs.el_size,&caps.port_attr[j].active_mtu,a->field_size);
                continue;
            case QUERY_DEVICE_OUT_DEV_CAP_RM_ORDER_CAP: source=&caps.dev_cap.rm_order_cap.value; size=sizeof(caps.dev_cap.rm_order_cap.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_RC_ORDER_CAP: source=&caps.dev_cap.rc_order_cap.value; size=sizeof(caps.dev_cap.rc_order_cap.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_RM_TP_CAP: source=&caps.dev_cap.rm_tp_cap.value; size=sizeof(caps.dev_cap.rm_tp_cap.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_RC_TP_CAP: source=&caps.dev_cap.rc_tp_cap.value; size=sizeof(caps.dev_cap.rc_tp_cap.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_UM_TP_CAP: source=&caps.dev_cap.um_tp_cap.value; size=sizeof(caps.dev_cap.um_tp_cap.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_TP_FEATURE: source=&caps.dev_cap.tp_feature.value; size=sizeof(caps.dev_cap.tp_feature.value); break;
            case QUERY_DEVICE_OUT_DEV_CAP_PRIORITY_INFO: source=&caps.dev_cap.priority_info; size=sizeof(caps.dev_cap.priority_info); break;
            default:errno=EINVAL;return -1;
        }
        if(!source||a->field_size!=size||!a->data){errno=EINVAL;return -1;}
        memcpy((void*)(uintptr_t)a->data,source,size);
    }
    return 0;
}
