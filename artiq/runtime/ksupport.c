#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include <link.h>
#include <dlfcn.h>
#include <dyld.h>
#include <unwind.h>

#include "ksupport.h"
#include "kloader.h"
#include "mailbox.h"
#include "messages.h"
#include "bridge.h"
#include "artiq_personality.h"
#include "ttl.h"
#include "dds.h"
#include "rtio.h"

void ksupport_abort(void);

int64_t now;

/* compiler-rt symbols */
extern void __divsi3, __modsi3, __ledf2, __gedf2, __unorddf2, __eqdf2, __ltdf2,
    __nedf2, __gtdf2, __negsf2, __negdf2, __addsf3, __subsf3, __mulsf3,
    __divsf3, __lshrdi3, __muldi3, __divdi3, __ashldi3, __ashrdi3,
    __udivmoddi4, __floatsisf, __floatunsisf, __fixsfsi, __fixunssfsi,
    __adddf3, __subdf3, __muldf3, __divdf3, __floatsidf, __floatunsidf,
    __floatdidf, __fixdfsi, __fixdfdi, __fixunsdfsi, __clzsi2, __ctzsi2,
    __udivdi3, __umoddi3, __moddi3;

/* artiq_personality symbols */
extern void __artiq_personality;

struct symbol {
    const char *name;
    void *addr;
};

static const struct symbol runtime_exports[] = {
    /* compiler-rt */
    {"__divsi3", &__divsi3},
    {"__modsi3", &__modsi3},
    {"__ledf2", &__ledf2},
    {"__gedf2", &__gedf2},
    {"__unorddf2", &__unorddf2},
    {"__eqdf2", &__eqdf2},
    {"__ltdf2", &__ltdf2},
    {"__nedf2", &__nedf2},
    {"__gtdf2", &__gtdf2},
    {"__negsf2", &__negsf2},
    {"__negdf2", &__negdf2},
    {"__addsf3", &__addsf3},
    {"__subsf3", &__subsf3},
    {"__mulsf3", &__mulsf3},
    {"__divsf3", &__divsf3},
    {"__lshrdi3", &__lshrdi3},
    {"__muldi3", &__muldi3},
    {"__divdi3", &__divdi3},
    {"__ashldi3", &__ashldi3},
    {"__ashrdi3", &__ashrdi3},
    {"__udivmoddi4", &__udivmoddi4},
    {"__floatsisf", &__floatsisf},
    {"__floatunsisf", &__floatunsisf},
    {"__fixsfsi", &__fixsfsi},
    {"__fixunssfsi", &__fixunssfsi},
    {"__adddf3", &__adddf3},
    {"__subdf3", &__subdf3},
    {"__muldf3", &__muldf3},
    {"__divdf3", &__divdf3},
    {"__floatsidf", &__floatsidf},
    {"__floatunsidf", &__floatunsidf},
    {"__floatdidf", &__floatdidf},
    {"__fixdfsi", &__fixdfsi},
    {"__fixdfdi", &__fixdfdi},
    {"__fixunsdfsi", &__fixunsdfsi},
    {"__clzsi2", &__clzsi2},
    {"__ctzsi2", &__ctzsi2},
    {"__udivdi3", &__udivdi3},
    {"__umoddi3", &__umoddi3},
    {"__moddi3", &__moddi3},

    /* exceptions */
    {"_Unwind_Resume", &_Unwind_Resume},
    {"__artiq_personality", &__artiq_personality},
    {"__artiq_raise", &__artiq_raise},
    {"__artiq_reraise", &__artiq_reraise},
    {"abort", &ksupport_abort},

    /* proxified syscalls */
    {"now", &now},

    {"watchdog_set", &watchdog_set},
    {"watchdog_clear", &watchdog_clear},

    {"log", &log},
    {"lognonl", &lognonl},
    {"send_rpc", &send_rpc},
    {"recv_rpc", &recv_rpc},

    /* direct syscalls */
    {"rtio_get_counter", &rtio_get_counter},

    {"ttl_set_o", &ttl_set_o},
    {"ttl_set_oe", &ttl_set_oe},
    {"ttl_set_sensitivity", &ttl_set_sensitivity},
    {"ttl_get", &ttl_get},
    {"ttl_clock_set", &ttl_clock_set},

    {"dds_init", &dds_init},
    {"dds_batch_enter", &dds_batch_enter},
    {"dds_batch_exit", &dds_batch_exit},
    {"dds_set", &dds_set},

    /* end */
    {NULL, NULL}
};

/* called by libunwind */
int fprintf(FILE *stream, const char *fmt, ...)
{
    struct msg_log request;

    request.type = MESSAGE_TYPE_LOG;
    request.fmt = fmt;
    request.no_newline = 1;
    va_start(request.args, fmt);
    mailbox_send_and_wait(&request);
    va_end(request.args);

    return 0;
}

/* called by libunwind */
int dladdr (const void *address, Dl_info *info) {
    /* we don't try to resolve names */
    return 0;
}

/* called by libunwind */
int dl_iterate_phdr (int (*callback) (struct dl_phdr_info *, size_t, void *), void *data) {
    Elf32_Ehdr *ehdr;
    struct dl_phdr_info phdr_info;
    int retval;

    ehdr = (Elf32_Ehdr *)(KERNELCPU_EXEC_ADDRESS - KSUPPORT_HEADER_SIZE);
    phdr_info = (struct dl_phdr_info){
        .dlpi_addr  = 0, /* absolutely linked */
        .dlpi_name  = "<ksupport>",
        .dlpi_phdr  = (Elf32_Phdr*) ((intptr_t)ehdr + ehdr->e_phoff),
        .dlpi_phnum = ehdr->e_phnum,
    };
    retval = callback(&phdr_info, sizeof(phdr_info), data);
    if(retval)
        return retval;

    ehdr = (Elf32_Ehdr *)KERNELCPU_PAYLOAD_ADDRESS;
    phdr_info = (struct dl_phdr_info){
        .dlpi_addr  = KERNELCPU_PAYLOAD_ADDRESS,
        .dlpi_name  = "<kernel>",
        .dlpi_phdr  = (Elf32_Phdr*) ((intptr_t)ehdr + ehdr->e_phoff),
        .dlpi_phnum = ehdr->e_phnum,
    };
    retval = callback(&phdr_info, sizeof(phdr_info), data);
    return retval;
}

static Elf32_Addr resolve_runtime_export(const char *name) {
    const struct symbol *sym = runtime_exports;
    while(sym->name) {
        if(!strcmp(sym->name, name))
            return (Elf32_Addr)sym->addr;
        ++sym;
    }
    return 0;
}

void exception_handler(unsigned long vect, unsigned long *regs,
                       unsigned long pc, unsigned long ea);
void exception_handler(unsigned long vect, unsigned long *regs,
                       unsigned long pc, unsigned long ea)
{
    artiq_raise_from_c("InternalError",
        "Hardware exception {0} at PC {1}, EA {2}",
        vect, pc, ea);
}

int main(void);
int main(void)
{
    struct msg_load_request *request = mailbox_receive();
    struct msg_load_reply load_reply = {
        .type = MESSAGE_TYPE_LOAD_REPLY,
        .error = NULL
    };

    if(request == NULL) {
        bridge_main();
        while(1);
    }

    if(request->library != NULL) {
        if(!dyld_load(request->library, KERNELCPU_PAYLOAD_ADDRESS,
                      resolve_runtime_export, request->library_info,
                      &load_reply.error)) {
            mailbox_send(&load_reply);
            while(1);
        }
    }

    if(request->run_kernel) {
        void (*kernel_init)() = request->library_info->init;

        mailbox_send_and_wait(&load_reply);

        now = now_init();
        kernel_init();
        now_save(now);

        struct msg_base finished_reply;
        finished_reply.type = MESSAGE_TYPE_FINISHED;
        mailbox_send_and_wait(&finished_reply);
    } else {
        mailbox_send(&load_reply);
    }

    while(1);
}

/* called from __artiq_personality */
void __artiq_terminate(struct artiq_exception *artiq_exn,
                       struct artiq_backtrace_item *backtrace,
                       size_t backtrace_size) {
    struct msg_exception msg;

    msg.type = MESSAGE_TYPE_EXCEPTION;
    msg.exception = artiq_exn;
    msg.backtrace = backtrace;
    msg.backtrace_size = backtrace_size;
    mailbox_send(&msg);

    while(1);
}

void ksupport_abort() {
    artiq_raise_from_c("InternalError", "abort() called; check device log for details",
                       0, 0, 0);
}

long long int now_init(void)
{
    struct msg_base request;
    struct msg_now_init_reply *reply;
    long long int now;

    request.type = MESSAGE_TYPE_NOW_INIT_REQUEST;
    mailbox_send_and_wait(&request);

    reply = mailbox_wait_and_receive();
    if(reply->type != MESSAGE_TYPE_NOW_INIT_REPLY) {
        log("Malformed MESSAGE_TYPE_NOW_INIT_REQUEST reply type %d",
            reply->type);
        while(1);
    }
    now = reply->now;
    mailbox_acknowledge();

    if(now < 0) {
        rtio_init();
        now = rtio_get_counter() + (272000 << RTIO_FINE_TS_WIDTH);
    }

    return now;
}

void now_save(long long int now)
{
    struct msg_now_save request;

    request.type = MESSAGE_TYPE_NOW_SAVE;
    request.now = now;
    mailbox_send_and_wait(&request);
}

int watchdog_set(int ms)
{
    struct msg_watchdog_set_request request;
    struct msg_watchdog_set_reply *reply;
    int id;

    request.type = MESSAGE_TYPE_WATCHDOG_SET_REQUEST;
    request.ms = ms;
    mailbox_send_and_wait(&request);

    reply = mailbox_wait_and_receive();
    if(reply->type != MESSAGE_TYPE_WATCHDOG_SET_REPLY) {
        log("Malformed MESSAGE_TYPE_WATCHDOG_SET_REQUEST reply type %d",
            reply->type);
        while(1);
    }
    id = reply->id;
    mailbox_acknowledge();

    return id;
}

void watchdog_clear(int id)
{
    struct msg_watchdog_clear request;

    request.type = MESSAGE_TYPE_WATCHDOG_CLEAR;
    request.id = id;
    mailbox_send_and_wait(&request);
}

void send_rpc(int service, const char *tag, ...)
{
    struct msg_rpc_send request;

    request.type = MESSAGE_TYPE_RPC_SEND;
    request.service = service;
    request.tag = tag;
    va_start(request.args, tag);
    mailbox_send_and_wait(&request);
    va_end(request.args);
}

int recv_rpc(void *slot) {
    struct msg_rpc_recv_request request;
    struct msg_rpc_recv_reply *reply;

    request.type = MESSAGE_TYPE_RPC_RECV_REQUEST;
    request.slot = slot;
    mailbox_send_and_wait(&request);

    reply = mailbox_wait_and_receive();
    if(reply->type != MESSAGE_TYPE_RPC_RECV_REPLY) {
        log("Malformed MESSAGE_TYPE_RPC_RECV_REQUEST reply type %d",
            reply->type);
        while(1);
    }

    if(reply->exception) {
        struct artiq_exception exception;
        memcpy(&exception, reply->exception,
               sizeof(struct artiq_exception));
        mailbox_acknowledge();
        __artiq_raise(&exception);
    } else {
        int alloc_size = reply->alloc_size;
        mailbox_acknowledge();
        return alloc_size;
    }
}

void lognonl(const char *fmt, ...)
{
    struct msg_log request;

    request.type = MESSAGE_TYPE_LOG;
    request.fmt = fmt;
    request.no_newline = 1;
    va_start(request.args, fmt);
    mailbox_send_and_wait(&request);
    va_end(request.args);
}

void log(const char *fmt, ...)
{
    struct msg_log request;

    request.type = MESSAGE_TYPE_LOG;
    request.fmt = fmt;
    request.no_newline = 0;
    va_start(request.args, fmt);
    mailbox_send_and_wait(&request);
    va_end(request.args);
}